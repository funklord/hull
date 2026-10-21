/*
 * hull.c -- the command line (project.md section 10).
 *
 *   hull plan [-f FILE] [--backend B] [--accept TOKEN]... [--explain] [--resolved]
 *   hull check [-f FILE] --backend B[,B...] [--accept TOKEN]...
 *   hull run [-f FILE] [--backend B] [--accept TOKEN]... [NAME]
 *   hull engines
 *
 * `plan` resolves each machine for the backend it names, or for --backend;
 * `check` does so for every backend listed, and is the CI gate (J.8).
 * Both print what was rejected, emulated or ignored, and --explain prints
 * every field; --resolved prints each machine as the plan resolved it,
 * every default written in (project.md section 4.7). Messages name the
 * file, line and column of the entry that asked, as a compiler would.
 *
 * `run` plans one machine, the file's or the one NAME names, and runs it
 * in the foreground. Paths in a file are relative to the file: the plan
 * carries its directory as `(base ...)` (section 4.8).
 *
 * Exit status for plan and check: 0 when nothing was rejected, 1 when
 * something was, 2 when the file could not be read or checked, or the
 * command was wrong. For run, docker's convention: the workload's own
 * status, 128 and the signal that ended it, or 125 when hull could not
 * plan or start it.
 */

#define _GNU_SOURCE

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <libgen.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include "access.h"
#include "builtin.h"
#include "core.h"
#include "matrix.h"
#include "plan.h"
#include "registry.h"
#include "spec.h"
#include "store.h"
#include "wire.h"

#define MAX_FILE      (16u << 20)
#define MAX_TOKENS    32u
#define MAX_BACKENDS  16u

typedef struct options {
	const char *file;
	const char *backends[MAX_BACKENDS + 1u];
	const char *tokens[MAX_TOKENS + 1u];
	unsigned    backend_count;
	unsigned    token_count;
	int         explain;
	int         resolved;
	FILE       *stream;  /* where reports go: stdout, or stderr under run */
	char       *base;    /* the file's directory, absolute */
} options;

/* One machine on one backend, as the report callback sees it. */
typedef struct report {
	const options        *options;
	const hull_spec      *spec;
	const char           *machine;
	const hull_choice    *choice;
	unsigned              rejected;
} report;

static void usage(FILE *out)
{
	fputs("usage: hull plan [-f FILE] [--backend B] [--accept TOKEN]... [--explain]"
	      " [--resolved]\n"
	      "       hull check [-f FILE] --backend B[,B...] [--accept TOKEN]...\n"
	      "       hull run [-f FILE] [--backend B] [--accept TOKEN]... [NAME]\n"
	      "       hull ps | hull stop [--timeout S] ID | hull logs [-f] ID | hull attach ID\n"
	      "       hull exec [-t] ID [--] CMD [ARG...]\n"
	      "       hull kill [-s SIGNAL] ID | hull stats [--over S] ID\n"
	      "       hull ssh ID | hull proxy-ssh HOST\n"
	      "       hull ssh-config [--remove] | hull gc\n"
	      "       hull commit [--arch A] DIR REF | hull materialize REF DIR | hull images\n"
	      "       hull diff REF REF\n"
	      "       hull core\n"
	      "       hull engines\n"
	      "       hull --version\n", out);
}

static void version(void)
{
	printf("hull %s\n", HULL_VERSION);
	printf("Copyright (C) 2026 Nabeel Sowan <nabeel@vibes.se>\n");
}

/* The kernel's name for the host's arch, in the spec's spelling. */
static const char *host_arch(void)
{
	static struct utsname host;

	if (uname(&host) != 0)
		return NULL;
	if (strcmp(host.machine, "x86_64") == 0)
		return "amd64";
	if (strcmp(host.machine, "aarch64") == 0)
		return "arm64";
	return host.machine;
}

static const char *outcome_name(hull_outcome outcome)
{
	switch (outcome) {
	case HULL_OUTCOME_NATIVE:   return "native";
	case HULL_OUTCOME_EMULATED: return "emulated";
	case HULL_OUTCOME_ACCEPTED: return "accepted";
	case HULL_OUTCOME_REJECTED: return "rejected";
	case HULL_OUTCOME_IGNORED:  return "ignored";
	}
	return "unknown";
}

static void print_resolution(void *context, const hull_resolution *resolution)
{
	report  *r = context;
	uint32_t line;
	uint32_t column;

	if (resolution->outcome == HULL_OUTCOME_REJECTED)
		r->rejected++;
	if (resolution->outcome == HULL_OUTCOME_NATIVE && !r->options->explain)
		return;
	hull_sexpr_locate(&r->spec->doc, resolution->offset, &line, &column);
	fprintf(r->options->stream, "%s:%u:%u: %s on %s (%s): %s: %s", r->options->file, line,
	        column, r->machine, r->choice->engine->caps->engine_name, r->choice->class_name,
	        resolution->id, outcome_name(resolution->outcome));
	if (resolution->note != NULL)
		fprintf(r->options->stream, ": %s", resolution->note);
	if (resolution->outcome == HULL_OUTCOME_REJECTED && resolution->token != NULL)
		fprintf(r->options->stream, " (--accept %s honours it)", resolution->token);
	fprintf(r->options->stream, "\n");
}

/* Read all of `path`, or say why not. */
static uint8_t *read_file(const char *path, size_t *length)
{
	FILE    *file = fopen(path, "rb");
	uint8_t *data;

	if (file == NULL) {
		perror(path);
		return NULL;
	}
	data = malloc(MAX_FILE);
	if (data == NULL) {
		fprintf(stderr, "hull: out of memory\n");
		fclose(file);
		return NULL;
	}
	*length = fread(data, 1u, MAX_FILE, file);
	if (ferror(file) || !feof(file)) {
		fprintf(stderr, "%s: unreadable, or larger than %u bytes\n", path, MAX_FILE);
		fclose(file);
		free(data);
		return NULL;
	}
	fclose(file);
	return data;
}

/* --- a source the store holds --------------------------------------------- */

/* One source rewritten: the bytes from `at`, `cut` of them, become `text`. */
typedef struct rewrite {
	uint32_t at;
	uint32_t cut;
	char     text[4600];
} rewrite;

#define MAX_REWRITES 64u

/*
 * A machine whose source is a bare ref the store holds runs that tree, as
 * J.1 says a bare string is an image ref in the store or a registry: its
 * source becomes a dir source on the tree's checkout, under an overlay
 * where it says no (mutable ...), so that every engine taking a dir source
 * runs it and none writes the checkout. It is written into the
 * file's text, which `data` holds in MAX_FILE bytes, before anything reads
 * it as a spec, so the registry, the matrix and the plan all judge the dir
 * source the engine is given. A ref the store does not hold is left alone,
 * an image ref for docker. With `create` the checkout is made, as a run
 * needs; a plan only names it. Returns 0, or -1 having said why.
 */
static int store_sources(const char *file, uint8_t *data, size_t *length, int create)
{
	static rewrite  rewrites[MAX_REWRITES];
	hull_sexpr_form machines[MAX_REWRITES];
	unsigned        machine_count = 0u;
	unsigned        count = 0u;
	hull_spec       spec;
	hull_store      store;
	int             opened = -2;  /* not yet tried */
	int             result = 0;

	/* A file that does not read is said by the caller, which reads it. */
	if (hull_spec_open(&spec, data, *length, NULL) != HULL_SPEC_OK)
		return 0;
	if (spec.has_machine) {
		machines[machine_count++] = spec.machine;
	} else if (spec.has_machines) {
		hull_sexpr_form entry;
		int             more;

		hull_sexpr_child(&spec.doc, &spec.machines, &entry);
		for (more = hull_sexpr_next(&spec.doc, &entry, &entry); more && machine_count < MAX_REWRITES;
		     more = hull_sexpr_next(&spec.doc, &entry, &entry))
			machines[machine_count++] = entry;
	}
	for (unsigned i = 0u; i < machine_count && result == 0; i++) {
		static const char *const schemes[] = { "dir:", "tar:", "pack:", "image:" };
		hull_sexpr_form          entry;
		hull_sexpr_form          head;
		hull_sexpr_form          value;
		hull_sexpr_form          mutable_entry;
		char                     name[300];
		char                     path[4400];
		char                     why[4400] = "";
		uint64_t                 skipped = 0u;
		int                      has_mutable;
		int                      schemed = 0;

		if (!hull_spec_find(&spec, &machines[i], "source", &entry) ||
		    !hull_sexpr_child(&spec.doc, &entry, &head) ||
		    !hull_sexpr_next(&spec.doc, &head, &value) || value.content_length >= sizeof name)
			continue;
		memcpy(name, spec.doc.data + value.content, value.content_length);
		name[value.content_length] = '\0';
		for (unsigned k = 0u; k < sizeof schemes / sizeof schemes[0]; k++)
			schemed |= strncmp(name, schemes[k], strlen(schemes[k])) == 0;
		if (schemed)
			continue;
		if (opened == -2) {
			opened = hull_store_open(&store, 0, why, sizeof why);
			if (opened < 0) {
				fprintf(stderr, "hull: the store: %s\n", why);
				return -1;
			}
		}
		if (opened != 0 || hull_store_checkout(&store, name, 0, path, sizeof path, &skipped,
		                                        why, sizeof why) != 0)
			continue;  /* not the store's: an image ref */
		has_mutable = hull_spec_find(&spec, &machines[i], "mutable", &mutable_entry);
		if (has_mutable) {
			hull_sexpr_form mutable_head;
			hull_sexpr_form mutable_value;

			hull_sexpr_child(&spec.doc, &mutable_entry, &mutable_head);
			if (hull_sexpr_next(&spec.doc, &mutable_head, &mutable_value) &&
			    hull_sexpr_is(&spec.doc, &mutable_value, "direct")) {
				fprintf(stderr, "%s: %s is a stored tree, which nothing writes: "
				        "(mutable direct) needs a dir source\n", file, name);
				result = -1;
				break;
			}
		}
		if (create && hull_store_checkout(&store, name, 1, path, sizeof path, &skipped, why,
		                                  sizeof why) != 0) {
			fprintf(stderr, "%s: %s: %s\n", file, name, why);
			result = -1;
			break;
		}
		if (strpbrk(path, "\"\\") != NULL) {
			fprintf(stderr, "%s: %s: the store's path cannot be written as a source\n", file,
			        name);
			result = -1;
			break;
		}
		for (const unsigned char *p = (const unsigned char *)path; *p != '\0'; p++)
			if (*p < 0x20u || *p == 0x7fu) {
				fprintf(stderr, "%s: %s: the store's path cannot be written as a source\n",
				        file, name);
				result = -1;
				break;
			}
		if (result != 0)
			break;
		/* From the value to the end of the entry, so that the overlay is
		 * an entry of the machine's own, after the source. */
		rewrites[count].at  = value.offset;
		rewrites[count].cut = entry.offset + entry.extent - value.offset;
		snprintf(rewrites[count].text, sizeof rewrites[count].text, "\"dir:%s\")%s", path,
		         has_mutable ? "" : " (mutable overlay)");
		count++;
	}
	if (opened == 0)
		hull_store_close(&store);
	/* Last first, so that each rewrite's offset is still the file's. */
	for (unsigned i = count; result == 0 && i-- > 0u;) {
		size_t added = strlen(rewrites[i].text);
		size_t tail  = *length - rewrites[i].at - rewrites[i].cut;

		if (*length - rewrites[i].cut + added > MAX_FILE) {
			fprintf(stderr, "%s: larger than %u bytes once its stored sources are named\n",
			        file, MAX_FILE);
			return -1;
		}
		memmove(data + rewrites[i].at + added, data + rewrites[i].at + rewrites[i].cut, tail);
		memcpy(data + rewrites[i].at, rewrites[i].text, added);
		*length = *length - rewrites[i].cut + added;
	}
	return result;
}

/* One machine on one backend. Returns 1 if anything was rejected, 0 if
 * not, and 2 if it could not be planned at all. `name` is NULL for a
 * file's single `(machine ...)`. */
static int resolve_one(const options *o, const hull_registry *registry, const hull_spec *spec,
                       const hull_sexpr_form *machine, const char *name, const char *backend)
{
	static hull_plan     plan;
	hull_choice          choice;
	const char          *reason = NULL;
	const char          *shown  = name != NULL ? name : "machine";
	report               r;
	hull_registry_status status;
	hull_plan_status     planned;

	status = hull_registry_choose(registry, spec, machine, backend, &choice, &reason);
	if (status != HULL_REGISTRY_OK) {
		fprintf(stderr, "%s: %s on %s: %s\n", o->file, shown,
		        backend != NULL ? backend : "its own backend", reason);
		return 2;
	}
	r.options  = o;
	r.spec     = spec;
	r.machine  = shown;
	r.choice   = &choice;
	r.rejected = 0u;
	planned = hull_plan_build(&plan, spec, machine, name, o->base, &choice,
	                          registry->host_arch, o->tokens, print_resolution, &r);
	if (planned != HULL_PLAN_OK && planned != HULL_PLAN_REJECTED) {
		fprintf(stderr, "%s: %s on %s: %s\n", o->file, shown,
		        choice.engine->caps->engine_name,
		        planned == HULL_PLAN_ERR_SIZE ? "the machine is too large to plan" :
		        planned == HULL_PLAN_ERR_CLASS ? "the engine does not implement the class" :
		        "the plan failed its own check, which is a bug in hull");
		return 2;
	}
	if (o->resolved)
		printf("%.*s", (int)plan.length, plan.text);
	if (r.rejected != 0u)
		printf("%s: %s on %s (%s): rejected, %u field%s\n", o->file, shown,
		       choice.engine->caps->engine_name, choice.class_name, r.rejected,
		       r.rejected == 1u ? "" : "s");
	else
		printf("%s: %s on %s (%s): ok\n", o->file, shown, choice.engine->caps->engine_name,
		       choice.class_name);
	return r.rejected != 0u;
}

/* Every machine in the file, on every backend asked for. */
static int resolve_all(const options *o, const hull_registry *registry)
{
	hull_spec       spec;
	hull_spec_error error;
	uint8_t        *data;
	size_t          length;
	int             worst = 0;

	data = read_file(o->file, &length);
	if (data == NULL)
		return 2;
	if (store_sources(o->file, data, &length, 0) != 0) {
		free(data);
		return 2;
	}
	if (hull_spec_open(&spec, data, length, &error) != HULL_SPEC_OK) {
		fprintf(stderr, "%s:%u:%u: %s%s%s\n", o->file, error.line, error.column,
		        error.path, error.path[0] != '\0' ? ": " : "", error.reason);
		free(data);
		return 2;
	}
	for (unsigned b = 0u; b < (o->backend_count == 0u ? 1u : o->backend_count); b++) {
		const char *backend = o->backend_count == 0u ? NULL : o->backends[b];
		int         result  = 0;

		if (spec.has_machine) {
			result = resolve_one(o, registry, &spec, &spec.machine, NULL, backend);
		} else if (spec.has_machines) {
			hull_sexpr_form entry;
			hull_sexpr_form name;
			int             more;

			hull_sexpr_child(&spec.doc, &spec.machines, &entry);
			for (more = hull_sexpr_next(&spec.doc, &entry, &entry); more;
			     more = hull_sexpr_next(&spec.doc, &entry, &entry)) {
				char text[64];
				int  one;

				hull_sexpr_child(&spec.doc, &entry, &name);
				snprintf(text, sizeof text, "%.*s", (int)name.content_length,
				         (const char *)spec.doc.data + name.content);
				one = resolve_one(o, registry, &spec, &entry, text, backend);
				if (one > result)
					result = one;
			}
		} else {
			printf("%s: no machine to plan, only a build\n", o->file);
		}
		if (result > worst)
			worst = result;
	}
	free(data);
	return worst;
}

/* The absolute directory `file` is in, which its relative paths mean, or
 * NULL. The caller frees it. */
static char *base_of(const char *file)
{
	char *copy = strdup(file);
	char *base = copy != NULL ? realpath(dirname(copy), NULL) : NULL;

	free(copy);
	return base;
}

/* --- the core ----------------------------------------------------------- */

static volatile sig_atomic_t interrupted;

static void interrupt(int signal)
{
	(void)signal;
	interrupted = 1;
}

/* $XDG_RUNTIME_DIR/hull/<leaf>, or 0 if the runtime directory is unset. */
static int runtime_path(char *out, size_t cap, const char *leaf)
{
	const char *runtime = getenv("XDG_RUNTIME_DIR");

	if (runtime == NULL || runtime[0] != '/')
		return 0;
	return (size_t)snprintf(out, cap, "%s/hull/%s", runtime, leaf) < cap;
}

static int dial_core(void)
{
	struct sockaddr_un address;
	char               path[sizeof address.sun_path];
	int                fd;

	char               dir[4096];
	int                at = -1;

	if (!runtime_path(dir, sizeof dir, ""))
		return -1;
	memset(&address, 0, sizeof address);
	address.sun_family = AF_UNIX;
	/* As the core names it: through a descriptor on the directory when
	 * the whole path does not fit in a socket address. */
	if (runtime_path(path, sizeof path, "core.sock")) {
		strcpy(address.sun_path, path);
	} else {
		at = open(dir, O_PATH | O_DIRECTORY | O_CLOEXEC);
		if (at < 0)
			return -1;
		snprintf(address.sun_path, sizeof address.sun_path, "/proc/self/fd/%d/core.sock", at);
	}
	fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (fd >= 0 && connect(fd, (struct sockaddr *)&address, sizeof address) != 0) {
		close(fd);
		fd = -1;
	}
	if (at >= 0)
		close(at);
	return fd;
}

/* Start a core, as `hull core`, in a session of its own and reparented at
 * once, so it is never this process's zombie; its output goes to
 * core.log, truncated, beside the socket. */
static void start_core(void)
{
	char  dir[4096];
	char  log[4096];
	pid_t child;

	if (!runtime_path(dir, sizeof dir, "") || !runtime_path(log, sizeof log, "core.log"))
		return;
	mkdir(dir, 0700);
	child = fork();
	if (child == 0) {
		if (fork() == 0) {
			int null = open("/dev/null", O_RDONLY);
			int out  = open(log, O_WRONLY | O_CREAT | O_TRUNC, 0600);

			setsid();
			if (null >= 0)
				dup2(null, 0);
			if (out >= 0) {
				dup2(out, 1);
				dup2(out, 2);
			}
			execl("/proc/self/exe", "hull", "core", (char *)NULL);
			_exit(127);
		}
		_exit(0);
	}
	if (child > 0)
		waitpid(child, NULL, 0);
}

/* Whether a core that went left machines for the next to take back:
 * hull/machines/ holds one. Then a command that starts no core of its own
 * starts one, as they are the core's to hold, not this command's. */
static int kept_machines(void)
{
	char           dir[4096];
	DIR           *kept;
	struct dirent *entry;
	int            found = 0;

	if (!runtime_path(dir, sizeof dir, "machines") || (kept = opendir(dir)) == NULL)
		return 0;
	/* An id has no dot, as neither `.`, `..` nor a half-written ID.new. */
	while (!found && (entry = readdir(kept)) != NULL)
		found = strchr(entry->d_name, '.') == NULL;
	closedir(kept);
	return found;
}

/* A connection to the core, said hello on. With `start`, or with machines
 * kept for one, a core is started when none answers. Returns 0; 1, saying
 * nothing, when no core answers and none was to be started; or -1 having
 * said why. One dial answers both whether a core runs and the connection,
 * since an idle core can leave between two. */
static int connect_core(hull_wire *wire, int start)
{
	hull_msg       hello = { 0 };
	hull_sexpr_doc body;
	int            fd = dial_core();

	start = start || kept_machines();
	if (fd < 0 && start && getenv("XDG_RUNTIME_DIR") != NULL) {
		start_core();
		for (int tries = 0; fd < 0 && tries < 100; tries++) {
			struct timespec pause = { 0, 50000000L };

			nanosleep(&pause, NULL);
			fd = dial_core();
		}
	}
	if (fd < 0 && !start)
		return 1;
	if (fd < 0) {
		if (getenv("XDG_RUNTIME_DIR") == NULL)
			fprintf(stderr, "hull: XDG_RUNTIME_DIR is not set, so there is no core\n");
		else if (start)
			fprintf(stderr, "hull: the core did not start; see $XDG_RUNTIME_DIR/hull/core.log\n");
		return -1;
	}
	if (hull_wire_open(wire, fd, HULL_WIRE_FROM_CORE) != 0) {
		close(fd);
		return -1;
	}
	hull_msg_raw(&hello, "(5:hello(2:ch1:0)(5:proto1:1))");
	if (hull_wire_send(fd, &hello) != 0 || hull_wire_read(wire, &body) != HULL_WIRE_OK ||
	    !hull_msg_is(&body, "hello")) {
		fprintf(stderr, "hull: the core did not answer hello\n");
		hull_msg_free(&hello);
		hull_wire_close(wire);
		return -1;
	}
	hull_msg_free(&hello);
	return 0;
}

static void send_request(hull_wire *wire, const char *type, uint32_t ch, const char *id,
                         const char *rest)
{
	hull_msg m = { 0 };

	hull_msg_raw(&m, type);
	hull_msg_channel(&m, ch);
	if (rest != NULL && strncmp(rest, "(6:follow", 9u) == 0)
		hull_msg_raw(&m, rest);
	if (id != NULL) {
		hull_msg_raw(&m, "(7:machine");
		hull_msg_text(&m, id);
		hull_msg_raw(&m, ")");
	}
	if (rest != NULL && strncmp(rest, "(6:follow", 9u) != 0)
		hull_msg_raw(&m, rest);
	hull_msg_raw(&m, ")");
	hull_wire_send(wire->fd, &m);
	hull_msg_free(&m);
}

static void write_all(int fd, const uint8_t *data, size_t n)
{
	while (n > 0u) {
		ssize_t wrote = write(fd, data, n);

		if (wrote < 0 && errno == EINTR)
			continue;
		if (wrote <= 0)
			return;
		data += wrote;
		n -= (size_t)wrote;
	}
}

/* Messages on channel `ch` until it ends: output written out, an exit
 * turned into the status `run` exits with. `stop_id`, when set, is the
 * machine a SIGINT or SIGTERM stops. Returns the status, 0 for a plain
 * end, or 125 for an error, which is printed. */
static int follow(hull_wire *wire, uint32_t ch, const char *stop_id)
{
	int stopping = 0;

	for (;;) {
		hull_sexpr_doc   body;
		hull_sexpr_form  value;
		long long        channel = -1;
		hull_wire_status status;

		if (interrupted && stop_id != NULL && !stopping) {
			stopping = 1;
			send_request(wire, "(4:stop", ch + 1u, stop_id, "(10:timeout_ms5:10000)");
		}
		status = hull_wire_read(wire, &body);
		if (status == HULL_WIRE_WAIT)
			continue;
		if (status != HULL_WIRE_OK) {
			fprintf(stderr, "hull: the core went away\n");
			return 125;
		}
		if (!hull_msg_find(&body, "ch", &value) || !hull_msg_long(&body, &value, &channel) ||
		    channel != (long long)ch)
			continue;
		if (hull_msg_is(&body, "data")) {
			hull_sexpr_form buf;
			hull_sexpr_form fd;

			if (hull_msg_find(&body, "buf", &buf) && hull_msg_find(&body, "fd", &fd))
				write_all(hull_sexpr_is(&body, &fd, "2") ? 2 : 1, body.data + buf.content,
				          buf.content_length);
		} else if (hull_msg_is(&body, "lost")) {
			long long bytes = 0;

			/* The machine is not held back for a reader; one that falls
			 * behind by more than the core keeps is told what it missed. */
			if (hull_msg_find(&body, "bytes", &value))
				hull_msg_long(&body, &value, &bytes);
			fprintf(stderr, "hull: %lld bytes of output lost: this reader fell behind\n", bytes);
		} else if (hull_msg_is(&body, "exited")) {
			long long code = 0;
			long long sig  = 0;

			if (hull_msg_find(&body, "code", &value))
				hull_msg_long(&body, &value, &code);
			if (hull_msg_find(&body, "signal", &value))
				hull_msg_long(&body, &value, &sig);
			return sig != 0 ? 128 + (int)sig : (int)code;
		} else if (hull_msg_is(&body, "error")) {
			char reason[512] = "an error";

			if (hull_msg_find(&body, "reason", &value))
				hull_msg_string(&body, &value, reason, sizeof reason);
			fprintf(stderr, "hull: %s\n", reason);
			return 125;
		} else if (hull_msg_is(&body, "eof") || hull_msg_is(&body, "done")) {
			return 0;
		}
	}
}

/* The machine's id: its name in (machines ...), else its hostname, else
 * the file's directory's name, lowercased into a DNS label. */
static int machine_id(const hull_spec *spec, const hull_sexpr_form *machine, const char *name,
                      const char *base, char *out, size_t cap)
{
	hull_sexpr_form entry;
	const char     *from = name;
	char            hostname[64];
	size_t          n = 0u;

	if (from == NULL && hull_spec_find(spec, machine, "hostname", &entry)) {
		hull_sexpr_form head;
		hull_sexpr_form value;
		size_t          length;

		hull_sexpr_child(&spec->doc, &entry, &head);
		hull_sexpr_next(&spec->doc, &head, &value);
		if (hull_sexpr_value(&spec->doc, &value, (uint8_t *)hostname, sizeof hostname - 1u,
		                     &length) == HULL_SEXPR_OK) {
			hostname[length] = '\0';
			from = hostname;
		}
	}
	if (from == NULL) {
		from = strrchr(base, '/');
		from = from != NULL ? from + 1 : base;
	}
	for (; *from != '\0' && n + 1u < cap && n < 63u; from++) {
		char c = *from;

		if (c >= 'A' && c <= 'Z')
			c = (char)(c - 'A' + 'a');
		if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')))
			c = '-';
		if (c == '-' && (n == 0u || out[n - 1u] == '-'))
			continue;
		out[n++] = c;
	}
	while (n > 0u && out[n - 1u] == '-')
		n--;
	out[n] = '\0';
	return n != 0u;
}

/* --- run ---------------------------------------------------------------- */

/* --- secrets ------------------------------------------------------------- */

/*
 * J.3's sources, resolved here, as the user, before the run is sent: this
 * process has the user's terminal, where `exec:` and `age:` may prompt.
 *
 * - `file:PATH` is the file, PATH relative to the spec's directory;
 * - `exec:COMMAND` is what COMMAND writes to stdout, run by /bin/sh -c in
 *   the spec's directory, with this process's stdin and stderr;
 * - `age:PATH` is `age --decrypt` of the file, with `-i` and the file
 *   $HULL_AGE_IDENTITY names, if it names one; else age asks for the
 *   file's passphrase.
 *
 * A command that fails, by its status or a signal, fails the run. Each
 * value is at most SECRET_LIMIT, and they and the plan are one message.
 */
#define SECRET_LIMIT 65536u

/* What `fd` holds, to its end, into `out`: its length, or -1 when it is
 * more than SECRET_LIMIT or cannot be read. */
static long read_secret(int fd, uint8_t *out)
{
	size_t n = 0u;

	for (;;) {
		ssize_t got = read(fd, out + n, SECRET_LIMIT + 1u - n);

		if (got < 0 && errno == EINTR)
			continue;
		if (got < 0)
			return -1;
		if (got == 0)
			return (long)n;
		n += (size_t)got;
		if (n > SECRET_LIMIT)
			return -1;
	}
}

/* `argv`'s stdout, run in `dir`. Its length, or -1 having said why. */
static long secret_from(char *const argv[], const char *dir, const char *name, uint8_t *out)
{
	int   pipe_fds[2];
	int   status;
	long  n;
	pid_t child;

	if (pipe2(pipe_fds, O_CLOEXEC) != 0)
		return -1;
	child = fork();
	if (child == 0) {
		dup2(pipe_fds[1], 1);
		if (dir != NULL && chdir(dir) != 0) {
			fprintf(stderr, "hull: secret %s: %s: %s\n", name, dir, strerror(errno));
			_exit(126);
		}
		execvp(argv[0], argv);
		fprintf(stderr, "hull: secret %s: %s: %s\n", name, argv[0], strerror(errno));
		_exit(127);
	}
	close(pipe_fds[1]);
	n = child > 0 ? read_secret(pipe_fds[0], out) : -1;
	close(pipe_fds[0]);
	if (child > 0)
		while (waitpid(child, &status, 0) < 0 && errno == EINTR) {
		}
	if (child < 0) {
		fprintf(stderr, "hull: secret %s: %s cannot be run\n", name, argv[0]);
		return -1;
	}
	if (n < 0) {
		fprintf(stderr, "hull: secret %s: more than %u bytes\n", name, SECRET_LIMIT);
		return -1;
	}
	if (WIFSIGNALED(status) || WEXITSTATUS(status) != 0) {
		fprintf(stderr, "hull: secret %s: %s %s %d\n", name, argv[0],
		        WIFSIGNALED(status) ? "was killed by signal" : "exited",
		        WIFSIGNALED(status) ? WTERMSIG(status) : WEXITSTATUS(status));
		return -1;
	}
	return n;
}

/* One secret's value from its source. Its length, or -1 having said why. */
static long resolve_secret(const hull_sexpr_doc *doc, const char *name, const char *source,
                           const char *dir, uint8_t *out)
{
	const char *identity = getenv("HULL_AGE_IDENTITY");
	char       *path;
	long        n;

	if (strncmp(source, "exec:", 5u) == 0) {
		char *argv[] = { "/bin/sh", "-c", (char *)source + 5, NULL };

		return secret_from(argv, dir, name, out);
	}
	if (strncmp(source, "file:", 5u) != 0 && strncmp(source, "age:", 4u) != 0) {
		fprintf(stderr, "hull: secret %s: %s is not a source\n", name, source);
		return -1;
	}
	path = hull_access_path(doc, strchr(source, ':') + 1);
	if (path == NULL)
		return -1;
	if (source[0] == 'a') {
		char *with[]    = { "age", "--decrypt", "-i", (char *)identity, "--", path, NULL };
		char *without[] = { "age", "--decrypt", "--", path, NULL };

		n = secret_from(identity != NULL && identity[0] != '\0' ? with : without, dir, name,
		                out);
	} else {
		int fd = open(path, O_RDONLY | O_CLOEXEC);

		n = fd >= 0 ? read_secret(fd, out) : -1;
		if (fd < 0)
			fprintf(stderr, "hull: secret %s: %s: %s\n", name, path, strerror(errno));
		else if (n < 0)
			fprintf(stderr, "hull: secret %s: %s: more than %u bytes, or unreadable\n", name,
			        path, SECRET_LIMIT);
		if (fd >= 0)
			close(fd);
	}
	free(path);
	return n;
}

/* `(secrets (NAME VALUE) ...)` for the plan's machine, onto `m`: 0, or -1
 * having said why. A machine with none adds nothing. */
static int add_secrets(hull_msg *m, const uint8_t *plan, size_t length)
{
	static uint8_t  value[SECRET_LIMIT + 1u];
	hull_sexpr_doc  doc;
	hull_sexpr_form machine;
	hull_sexpr_form top;
	hull_sexpr_form secrets;
	hull_sexpr_form item;
	hull_sexpr_form name_form;
	char           *dir;
	int             more;
	int             result = 0;

	if (!hull_access_machine(&doc, plan, length, &machine) ||
	    !hull_access_find(&doc, &machine, "secrets", &secrets))
		return 0;
	hull_sexpr_first(&doc, &top);
	dir = hull_access_value(&doc, &top, "base");
	hull_msg_raw(m, "(7:secrets");
	hull_sexpr_child(&doc, &secrets, &item);
	for (more = hull_sexpr_next(&doc, &item, &item); more && result == 0;
	     more = hull_sexpr_next(&doc, &item, &item)) {
		char *name   = hull_sexpr_child(&doc, &item, &name_form) ?
		               hull_access_text(&doc, &name_form) : NULL;
		char *source = hull_access_value(&doc, &item, "source");
		char *expose = hull_access_value(&doc, &item, "expose");
		long  n      = name != NULL && source != NULL ?
		               resolve_secret(&doc, name, source, dir, value) : -1;

		if (n < 0) {
			result = -1;
		} else {
			if (expose != NULL && strcmp(expose, "env") == 0)
				fprintf(stderr, "hull: warning: secret %s is in the machine's environment, "
				        "where every process of it can read it\n", name);
			hull_msg_raw(m, "(");
			hull_msg_text(m, name);
			hull_msg_atom(m, value, (size_t)n);
			hull_msg_raw(m, ")");
		}
		free(name);
		free(source);
		free(expose);
	}
	hull_msg_raw(m, ")");
	free(dir);
	return result;
}

static int run(options *o, const hull_registry *registry, const char *name)
{
	static hull_plan     plan;
	hull_spec            spec;
	hull_spec_error      error;
	hull_sexpr_form      machine;
	hull_choice          choice;
	hull_wire            wire;
	hull_msg             request = { 0 };
	const char          *reason = NULL;
	const char          *shown;
	struct sigaction     action;
	report               r;
	hull_plan_status     planned;
	uint8_t             *data;
	size_t               length;
	char                 id[64];
	int                  status;

	data = read_file(o->file, &length);
	if (data == NULL)
		return 125;
	if (store_sources(o->file, data, &length, 1) != 0) {
		free(data);
		return 125;
	}
	if (hull_spec_open(&spec, data, length, &error) != HULL_SPEC_OK) {
		fprintf(stderr, "%s:%u:%u: %s%s%s\n", o->file, error.line, error.column,
		        error.path, error.path[0] != '\0' ? ": " : "", error.reason);
		return 125;
	}
	if (spec.has_machine && name == NULL) {
		machine = spec.machine;
	} else if (spec.has_machines && name != NULL &&
	           hull_spec_find(&spec, &spec.machines, name, &machine)) {
		;
	} else {
		fprintf(stderr, "%s: %s\n", o->file, spec.has_machines ?
		        "name the machine to run, one of (machines ...)" :
		        name != NULL ? "a single machine takes no name" : "there is no machine to run");
		return 125;
	}
	shown = name != NULL ? name : "machine";
	if (!machine_id(&spec, &machine, name, o->base, id, sizeof id)) {
		fprintf(stderr, "%s: name the machine with (hostname ...)\n", o->file);
		return 125;
	}
	if (hull_registry_choose(registry, &spec, &machine,
	                         o->backend_count != 0u ? o->backends[0] : NULL, &choice,
	                         &reason) != HULL_REGISTRY_OK) {
		fprintf(stderr, "%s: %s: %s\n", o->file, shown, reason);
		return 125;
	}
	r.options  = o;
	r.spec     = &spec;
	r.machine  = shown;
	r.choice   = &choice;
	r.rejected = 0u;
	planned = hull_plan_build(&plan, &spec, &machine, name, o->base, &choice,
	                          registry->host_arch, o->tokens, print_resolution, &r);
	if (planned != HULL_PLAN_OK) {
		fprintf(stderr, "%s: %s on %s (%s): not run: %s\n", o->file, shown,
		        choice.engine->caps->engine_name, choice.class_name,
		        planned == HULL_PLAN_REJECTED ? "the plan is rejected" : "it could not be planned");
		return 125;
	}
	free(data);
	{
		const hull_engine_ops *ops = choice.engine->ops;

		if (ops->create == NULL || ops->start == NULL || ops->stop == NULL ||
		    ops->destroy == NULL) {
			fprintf(stderr, "%s: %s: the %s engine cannot run a machine yet\n", o->file,
			        shown, choice.engine->caps->engine_name);
			return 125;
		}
	}

	/* Before the core is reached, and before SIGINT would stop a machine:
	 * a command asked for a secret may prompt, and ^C there ends this. */
	hull_msg_raw(&request, "(3:run(2:ch1:1)(7:machine");
	hull_msg_text(&request, id);
	hull_msg_raw(&request, ")");
	hull_msg_bytes(&request, plan.canonical, plan.canonical_length);
	if (add_secrets(&request, plan.canonical, plan.canonical_length) != 0) {
		fprintf(stderr, "%s: %s: not run: a secret could not be had\n", o->file, shown);
		hull_msg_free(&request);
		return 125;
	}
	hull_msg_raw(&request, ")");

	/* SIGINT and SIGTERM stop the machine; a hangup only leaves it, to
	 * whatever (on_disconnect ...) says. */
	memset(&action, 0, sizeof action);
	action.sa_handler = interrupt;
	sigaction(SIGINT, &action, NULL);
	sigaction(SIGTERM, &action, NULL);
	if (connect_core(&wire, 1) != 0) {
		hull_msg_free(&request);
		return 125;
	}
	if (request.failed || hull_wire_send(wire.fd, &request) != 0) {
		fprintf(stderr, "hull: the run could not be sent\n");
		hull_msg_free(&request);
		hull_wire_close(&wire);
		return 125;
	}
	hull_msg_free(&request);
	status = follow(&wire, 1u, id);
	hull_wire_close(&wire);
	return status;
}

/* --- the other commands -------------------------------------------------- */

/* Whether a core answers, without starting one; or would, holding the
 * machines a core that went kept for it. */
static int list_machines(void)
{
	hull_wire      wire;
	hull_sexpr_doc body;
	hull_sexpr_form message;
	hull_sexpr_form item;
	int            more;
	int            status = connect_core(&wire, 0);

	if (status == 1) {
		printf("%-24s %-15s %-10s %-6s %s\n", "ID", "ADDRESS", "ENGINE", "CLASS", "STATE");
		return 0;
	}
	if (status != 0)
		return 2;
	send_request(&wire, "(4:list", 1u, NULL, NULL);
	if (hull_wire_read(&wire, &body) != HULL_WIRE_OK || !hull_msg_is(&body, "machines")) {
		hull_wire_close(&wire);
		return 2;
	}
	printf("%-24s %-15s %-10s %-6s %s\n", "ID", "ADDRESS", "ENGINE", "CLASS", "STATE");
	hull_sexpr_first(&body, &message);
	hull_sexpr_child(&body, &message, &item);
	for (more = hull_sexpr_next(&body, &item, &item); more;
	     more = hull_sexpr_next(&body, &item, &item)) {
		const char *keys[] = { "id", "address", "engine", "class", "state" };
		char        values[5][64] = { "", "", "", "", "" };
		hull_sexpr_form head;

		hull_sexpr_child(&body, &item, &head);
		if (!hull_sexpr_is(&body, &head, "machine"))
			continue;
		for (unsigned k = 0u; k < 5u; k++) {
			hull_sexpr_form entry;
			hull_sexpr_form name;
			hull_sexpr_form value;
			int             entries;

			hull_sexpr_child(&body, &item, &entry);
			for (entries = hull_sexpr_next(&body, &entry, &entry); entries;
			     entries = hull_sexpr_next(&body, &entry, &entry))
				if (hull_sexpr_child(&body, &entry, &name) &&
				    hull_sexpr_is(&body, &name, keys[k]) &&
				    hull_sexpr_next(&body, &name, &value))
					hull_msg_string(&body, &value, values[k], sizeof values[k]);
		}
		printf("%-24s %-15s %-10s %-6s %s\n", values[0], values[1], values[2], values[3],
		       values[4]);
	}
	hull_wire_close(&wire);
	return 0;
}

/* `hull gc`: what the engines keep that nothing wants, reclaimed by the
 * core, which is started for it, and each engine's report written out. An
 * engine that could not look says so on stderr, and is not a failure of
 * gc's: a host without docker has nothing of docker's to reclaim. */
/* --- the store ---------------------------------------------------------- */

/* Section 3's store, made if `create`: 0, or 2 having said why, or 1 when
 * there is none and none was to be made. */
static int open_store(hull_store *store, int create)
{
	char why[4400] = "";
	int  opened = hull_store_open(store, create, why, sizeof why);

	if (opened < 0)
		fprintf(stderr, "hull: the store: %s\n", why);
	return opened < 0 ? 2 : opened;
}

static int commit(const char *dir, const char *ref, const char *arch)
{
	hull_store  store;
	hull_commit made;
	char        why[4400] = "";
	int         status;

	if ((status = open_store(&store, 1)) != 0)
		return status;
	status = hull_store_commit(&store, dir, arch, ref, &made, why, sizeof why);
	hull_store_close(&store);
	if (status != 0) {
		fprintf(stderr, "hull: commit: %s\n", why);
		return 1;
	}
	printf("%s sha256:%s\n", ref, made.manifest);
	fprintf(stderr, "hull: %llu entries, %llu objects, %llu new, %llu bytes%s\n",
	        (unsigned long long)made.entries, (unsigned long long)made.objects,
	        (unsigned long long)made.new_objects, (unsigned long long)made.total_size,
	        made.skipped != 0u ? "; some were left out, as said" : "");
	return 0;
}

static int materialize(const char *ref, const char *dir)
{
	hull_store store;
	uint64_t   skipped = 0u;
	char       why[4400] = "";
	int        status;

	if ((status = open_store(&store, 0)) != 0) {
		if (status == 1)
			fprintf(stderr, "hull: materialize: there is no store, so no %s\n", ref);
		return 1;
	}
	status = hull_store_materialize(&store, ref, dir, &skipped, why, sizeof why);
	hull_store_close(&store);
	if (status != 0) {
		fprintf(stderr, "hull: materialize: %s\n", why);
		return 1;
	}
	if (skipped != 0u)
		fprintf(stderr, "hull: materialize: %llu kept as the user could, as said\n",
		        (unsigned long long)skipped);
	return 0;
}

static void print_ref(void *context, const char *ref, const char *manifest)
{
	(void)context;
	printf("%-40s sha256:%s\n", ref, manifest);
}

static int images(void)
{
	hull_store store;
	char       why[256] = "";
	int        status;

	printf("%-40s %s\n", "REF", "MANIFEST");
	if ((status = open_store(&store, 0)) != 0)
		return status == 1 ? 0 : status;
	status = hull_store_list(&store, print_ref, NULL, why, sizeof why);
	hull_store_close(&store);
	return status == 0 ? 0 : 2;
}

/* A path on a line of its own: a backslash, and a byte below a space or
 * DEL, as \ and three octal digits, so a newline in a name cannot start
 * a line of its own. */
static void put_path(const char *path)
{
	for (const unsigned char *p = (const unsigned char *)path; *p != '\0'; p++) {
		if (*p == '\\' || *p < 0x20u || *p == 0x7fu)
			printf("\\%03o", *p);
		else
			putchar(*p);
	}
}

static void print_change(void *context, char change, const char *path, const char *what)
{
	(void)context;
	if (path == NULL) {
		printf("%s\n", what);
		return;
	}
	printf("%c ", change);
	put_path(path);
	printf("%s%s\n", what[0] != '\0' ? ": " : "", what);
}

/* diff(1)'s statuses: 0 the same, 1 different, 2 trouble. */
static int diff(const char *from, const char *to)
{
	hull_store store;
	char       why[4400] = "";
	int        status;

	if ((status = open_store(&store, 0)) != 0) {
		if (status == 1)
			fprintf(stderr, "hull: diff: there is no store, so no %s\n", from);
		return 2;
	}
	status = hull_store_diff(&store, from, to, print_change, NULL, why, sizeof why);
	hull_store_close(&store);
	if (status < 0) {
		fprintf(stderr, "hull: diff: %s\n", why);
		return 2;
	}
	return status;
}

/* What the engines keep that nothing wants, through the core, and what
 * no ref reaches in the store, if there is one. */
static int collect(void)
{
	hull_store store;
	hull_wire  wire;
	uint64_t   manifests = 0u;
	uint64_t   objects = 0u;
	uint64_t   checkouts = 0u;
	char       why[4400] = "";
	int        status;
	int        result = 0;

	if ((status = open_store(&store, 0)) == 2) {
		result = 1;
	} else if (status == 0) {
		if (hull_store_gc(&store, &manifests, &objects, &checkouts, why, sizeof why) != 0) {
			fprintf(stderr, "hull: gc: the store: %s\n", why);
			result = 1;
		} else {
			printf("store: removed %llu manifests, %llu objects and %llu checkouts no ref "
			       "reaches\n", (unsigned long long)manifests, (unsigned long long)objects,
			       (unsigned long long)checkouts);
		}
		hull_store_close(&store);
	}
	if (connect_core(&wire, 1) != 0)
		return 2;
	send_request(&wire, "(2:gc", 1u, NULL, NULL);
	status = follow(&wire, 1u, NULL);
	hull_wire_close(&wire);
	return status == 125 || result != 0 ? 1 : 0;
}

/* --- exec -------------------------------------------------------------- */

/* What `hull exec` forwards, by the names the protocol uses. */
static const struct {
	int         number;
	const char *name;
} forwarded[] = {
	{ SIGHUP, "HUP" }, { SIGINT, "INT" }, { SIGQUIT, "QUIT" }, { SIGTERM, "TERM" },
	{ SIGUSR1, "USR1" }, { SIGUSR2, "USR2" },
};

static volatile sig_atomic_t arrived[sizeof forwarded / sizeof forwarded[0]];
static volatile sig_atomic_t resized;

static void note_signal(int signal)
{
	if (signal == SIGWINCH)
		resized = 1;
	for (unsigned i = 0u; i < sizeof forwarded / sizeof forwarded[0]; i++)
		if (forwarded[i].number == signal)
			arrived[i] = 1;
}

/* The terminal as it was before raw mode, put back on every way out. */
static struct termios saved_terminal;
static int            terminal_raw;

static void restore_terminal(void)
{
	if (terminal_raw)
		tcsetattr(0, TCSAFLUSH, &saved_terminal);
	terminal_raw = 0;
}

/* `(4:cols..)(4:rows..)`: this terminal's size, or 80 by 24 where it
 * does not say. */
static void add_window(hull_msg *m)
{
	struct winsize size;

	if (ioctl(0, TIOCGWINSZ, &size) != 0 || size.ws_col == 0 || size.ws_row == 0) {
		size.ws_col = 80;
		size.ws_row = 24;
	}
	hull_msg_raw(m, "(4:cols");
	hull_msg_number(m, size.ws_col);
	hull_msg_raw(m, ")(4:rows");
	hull_msg_number(m, size.ws_row);
	hull_msg_raw(m, ")");
}

static void send_winch(int fd)
{
	hull_msg m = { 0 };

	hull_msg_raw(&m, "(5:winch(2:ch1:1)");
	add_window(&m);
	hull_msg_raw(&m, ")");
	hull_wire_send(fd, &m);
	hull_msg_free(&m);
}

/* `hull exec [-t] ID [--] CMD [ARG...]`: the command's output written out,
 * this process's stdin its input, and its status this process's. The
 * signals above are the command's rather than this process's: each is
 * forwarded, and the command decides. With -t it runs on a terminal of
 * its own, this one is raw while it does, and its size follows this
 * one's. */
static int exec_in(const char *id, char **argv, int argc, int tty, unsigned port)
{
	hull_wire        wire;
	hull_msg         request = { 0 };
	struct sigaction action;
	sigset_t         held;
	sigset_t         waiting;
	int              input_open = 1;
	size_t           credit = 0u;  /* stdin the core will take now */
	int              started = 0;
	int              result = 125;
	int              status;

	if (tty && !isatty(0)) {
		fprintf(stderr, "hull: -t needs a terminal on stdin\n");
		return 125;
	}
	if ((status = connect_core(&wire, 0)) != 0) {
		if (status == 1)
			fprintf(stderr, "hull: no core is running, so no machine is\n");
		return 125;
	}
	if (port > 0u) {
		hull_msg_raw(&request, "(4:dial");
		hull_msg_channel(&request, 1u);
		hull_msg_raw(&request, "(7:machine");
		hull_msg_text(&request, id);
		hull_msg_raw(&request, ")(4:port");
		hull_msg_number(&request, (long long)port);
		hull_msg_raw(&request, "))");
	} else {
		hull_msg_raw(&request, "(4:exec(4:argv");
		for (int i = 0; i < argc; i++)
			hull_msg_text(&request, argv[i]);
		hull_msg_raw(&request, ")");
		hull_msg_channel(&request, 1u);
		hull_msg_raw(&request, "(7:machine");
		hull_msg_text(&request, id);
		hull_msg_raw(&request, ")(3:tty");
		if (tty)
			add_window(&request);
		else
			hull_msg_raw(&request, "5:false");
		hull_msg_raw(&request, "))");
	}

	/* Held outside ppoll(), so that none arrives between the look at
	 * what has and the wait. */
	sigemptyset(&held);
	memset(&action, 0, sizeof action);
	action.sa_handler = note_signal;
	for (unsigned i = 0u; i < sizeof forwarded / sizeof forwarded[0]; i++) {
		sigaddset(&held, forwarded[i].number);
		sigaction(forwarded[i].number, &action, NULL);
	}
	if (tty) {
		sigaddset(&held, SIGWINCH);
		sigaction(SIGWINCH, &action, NULL);
	}
	sigprocmask(SIG_BLOCK, &held, &waiting);

	if (hull_wire_send(wire.fd, &request) != 0) {
		hull_msg_free(&request);
		hull_wire_close(&wire);
		return 125;
	}
	hull_msg_free(&request);
	if (tty) {
		struct termios raw;

		if (tcgetattr(0, &saved_terminal) == 0) {
			raw = saved_terminal;
			cfmakeraw(&raw);
			if (tcsetattr(0, TCSAFLUSH, &raw) == 0)
				terminal_raw = 1;
		}
	}
	for (;;) {
		struct pollfd    fds[2] = { { wire.fd, POLLIN, 0 }, { 0, POLLIN, 0 } };
		hull_sexpr_doc   body;
		hull_wire_status status;
		int              ended = 0;
		size_t           taken = 0u;  /* output written out, for credit */

		for (unsigned i = 0u; i < sizeof forwarded / sizeof forwarded[0]; i++) {
			hull_msg m = { 0 };

			if (!arrived[i])
				continue;
			arrived[i] = 0;
			hull_msg_raw(&m, "(6:signal(2:ch1:1)(6:signal");
			hull_msg_text(&m, forwarded[i].name);
			hull_msg_raw(&m, "))");
			hull_wire_send(wire.fd, &m);
			hull_msg_free(&m);
		}
		if (resized) {
			resized = 0;
			send_winch(wire.fd);
		}
		/* stdin is read only while the core will take it, so a command
		 * that does not read holds its frontend back, as a pipe would. */
		if (ppoll(fds, input_open && credit > 0u ? 2u : 1u, NULL, &waiting) < 0) {
			if (errno == EINTR)
				continue;
			break;
		}
		if (input_open && credit > 0u && fds[1].revents != 0) {
			uint8_t  chunk[16384];
			ssize_t  n = read(0, chunk, credit < sizeof chunk ? credit : sizeof chunk);
			hull_msg m = { 0 };

			if (n < 0 && errno == EINTR)
				continue;
			if (n > 0) {
				credit -= (size_t)n;
				hull_msg_raw(&m, "(4:data(3:buf");
				hull_msg_atom(&m, chunk, (size_t)n);
				hull_msg_raw(&m, ")(2:ch1:1)(2:fd1:0))");
			} else {
				hull_msg_raw(&m, "(3:eof(2:ch1:1))");
				input_open = 0;
			}
			hull_wire_send(wire.fd, &m);
			hull_msg_free(&m);
		}
		if (fds[0].revents == 0)
			continue;
		status = hull_wire_fill(&wire);
		if (status == HULL_WIRE_CLOSED || status == HULL_WIRE_BROKEN) {
			restore_terminal();
			fprintf(stderr, "hull: the core went away\n");
			break;
		}
		while (!ended && (status = hull_wire_next(&wire, &body)) == HULL_WIRE_OK) {
			hull_sexpr_form value;
			long long       code = 0;
			long long       sig  = 0;

			if (hull_msg_is(&body, "data")) {
				hull_sexpr_form buf;
				hull_sexpr_form fd;

				if (hull_msg_find(&body, "buf", &buf) && hull_msg_find(&body, "fd", &fd)) {
					write_all(hull_sexpr_is(&body, &fd, "2") ? 2 : 1, body.data + buf.content,
					          buf.content_length);
					taken += buf.content_length;
				}
			} else if (hull_msg_is(&body, "credit")) {
				long long bytes = 0;

				if (hull_msg_find(&body, "bytes", &value) && hull_msg_long(&body, &value, &bytes) &&
				    bytes > 0)
					credit += (size_t)bytes;
			} else if (hull_msg_is(&body, "exited")) {
				if (hull_msg_find(&body, "code", &value))
					hull_msg_long(&body, &value, &code);
				if (hull_msg_find(&body, "signal", &value))
					hull_msg_long(&body, &value, &sig);
				result = sig != 0 ? 128 + (int)sig : (int)code;
				ended  = 1;
			} else if (hull_msg_is(&body, "error")) {
				char reason[512] = "an error";

				if (hull_msg_find(&body, "reason", &value))
					hull_msg_string(&body, &value, reason, sizeof reason);
				/* Once it has started, what is refused is a resize or a
				 * signal, and the command runs on. */
				if (!started) {
					restore_terminal();
					ended = 1;
				}
				fprintf(stderr, started && terminal_raw ? "hull: %s\r\n" : "hull: %s\n",
				        reason);
			} else if (hull_msg_is(&body, "started")) {
				started = 1;
			}
		}
		if (ended || status == HULL_WIRE_BROKEN)
			break;
		/* The output is written out before more is asked for, so a
		 * reader of it that falls behind holds the command back. */
		if (taken > 0u) {
			hull_msg m = { 0 };

			hull_msg_raw(&m, "(6:credit(5:bytes");
			hull_msg_number(&m, (long long)taken);
			hull_msg_raw(&m, ")(2:ch1:1))");
			hull_wire_send(wire.fd, &m);
			hull_msg_free(&m);
		}
	}
	restore_terminal();
	hull_wire_close(&wire);
	return result;
}

/* What the host spends on a running machine, as its engine measures it:
 * CPU time, storage io and memory resident. */
/*
 * `hull ssh ID`: section 8.1's fallback, a login shell in the machine on
 * a terminal of its own, as `hull exec -t ID /bin/sh -l` would give. An
 * exec carries the machine's environment and none of the frontend's, so
 * TERM goes in as the shell's argument, which needs nothing in the
 * machine but /bin/sh, and is never read as shell text.
 */
static int ssh_into(const char *id)
{
	const char *term = getenv("TERM");
	char       *login[] = { "/bin/sh", "-l" };
	char       *termed[] = { "/bin/sh", "-c", "TERM=\"$1\" exec /bin/sh -l", "sh", NULL };

	if (!isatty(0)) {
		fprintf(stderr, "hull: ssh needs a terminal on stdin\n");
		return 125;
	}
	if (term == NULL || term[0] == '\0')
		return exec_in(id, login, 2, 1, 0u);
	termed[4] = (char *)term;
	return exec_in(id, termed, 5, 1, 0u);
}

/*
 * `hull proxy-ssh HOST`: section 8.1's ProxyCommand, so that `ssh web.hull`
 * reaches the machine's sshd, its port 22, declared or not, on every
 * engine that gives a machine ports of its own. HOST is the machine's
 * name, with or without `.hull`; stdin and stdout are the connection, as
 * ssh expects of a ProxyCommand.
 */
static int proxy_ssh(const char *host)
{
	char   id[64];
	size_t n = strlen(host);

	if (n > 0u && host[n - 1u] == '.')
		n--;
	if (n >= 5u && strncmp(host + n - 5u, ".hull", 5u) == 0)
		n -= 5u;
	if (n == 0u || n >= sizeof id) {
		fprintf(stderr, "hull: %s names no machine\n", host);
		return 125;
	}
	memcpy(id, host, n);
	id[n] = '\0';
	return exec_in(id, NULL, 0, 0, 22u);
}

/*
 * `hull ssh-config [--remove]`: section 8.1's block in ~/.ssh/config, so
 * that `ssh web.hull`, scp, rsync and an IDE's remote development reach a
 * machine's sshd through `hull proxy-ssh`, this hull's, by its full path.
 *
 * ssh applies what comes before the first Host or Match to every host, so
 * the block goes just before that line: ahead of the user's blocks, where
 * a `Host *` of theirs cannot take the ProxyCommand first, and after their
 * global lines, which it must not take into its own Host. With neither,
 * at the end. It is marked, so that running again replaces it and
 * --remove takes exactly it out. The file is written whole and renamed
 * into place, keeping its mode; one that does not exist is made 0600, in
 * a ~/.ssh made 0700.
 */
#define SSH_BEGIN "# hull ssh-config: begin -- hull keeps these lines; `hull ssh-config --remove` drops them\n"
#define SSH_END   "# hull ssh-config: end\n"
#define SSH_LIMIT (1024 * 1024)

/* Whether `line` opens a Host or a Match block, in either case, after any
 * blanks, with the keyword ended by a blank or `=`. */
static int opens_block(const char *line)
{
	while (*line == ' ' || *line == '\t')
		line++;
	return (strncasecmp(line, "Host", 4u) == 0 &&
	        (line[4] == ' ' || line[4] == '\t' || line[4] == '=')) ||
	       (strncasecmp(line, "Match", 5u) == 0 &&
	        (line[5] == ' ' || line[5] == '\t' || line[5] == '='));
}

static int ssh_config(int remove)
{
	const char *home = getenv("HOME");
	char        dir[4096];
	char        path[4200];
	char        fresh[4300];
	char        self[4096];
	char        block[4500];
	char       *old = NULL;
	char       *out = NULL;
	size_t      old_len = 0u;
	size_t      out_len = 0u;
	size_t      at = 0u;
	mode_t      mode = 0600;
	struct stat st;
	ssize_t     n;
	int         fd;
	int         placed = 0;

	if (home == NULL || home[0] != '/') {
		fprintf(stderr, "hull: ssh-config: HOME is not set\n");
		return 1;
	}
	snprintf(dir, sizeof dir, "%s/.ssh", home);
	snprintf(path, sizeof path, "%s/config", dir);
	snprintf(fresh, sizeof fresh, "%s/config.hull-new", dir);
	n = readlink("/proc/self/exe", self, sizeof self - 1u);
	if (n <= 0 || (size_t)n >= sizeof self - 1u) {
		fprintf(stderr, "hull: ssh-config: where this hull is cannot be read\n");
		return 1;
	}
	self[n] = '\0';
	if (strpbrk(self, "\"%\\\n") != NULL) {
		fprintf(stderr, "hull: ssh-config: %s is not a path ssh can be given\n", self);
		return 1;
	}
	snprintf(block, sizeof block, SSH_BEGIN "Host *.hull\n\tProxyCommand \"%s\" proxy-ssh %%h\n" SSH_END,
	         self);

	if (mkdir(dir, 0700) != 0 && errno != EEXIST) {
		fprintf(stderr, "hull: ssh-config: %s: %s\n", dir, strerror(errno));
		return 1;
	}
	fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd >= 0) {
		if (fstat(fd, &st) == 0)
			mode = st.st_mode & 07777;
		old = malloc(SSH_LIMIT + 1u);
		while (old != NULL && (n = read(fd, old + old_len, SSH_LIMIT - old_len)) > 0)
			old_len += (size_t)n;
		close(fd);
		if (old == NULL || old_len == SSH_LIMIT) {
			fprintf(stderr, "hull: ssh-config: %s is larger than a MiB\n", path);
			free(old);
			return 1;
		}
		old[old_len] = '\0';
	} else if (errno != ENOENT) {
		fprintf(stderr, "hull: ssh-config: %s: %s\n", path, strerror(errno));
		return 1;
	}
	out = malloc(old_len + sizeof block + 2u);
	if (out == NULL) {
		free(old);
		return 1;
	}
	/* Line by line: the block as it was dropped, and as it is put in
	 * before the first Host or Match. */
	while (at < old_len) {
		char  *end = memchr(old + at, '\n', old_len - at);
		size_t len = end != NULL ? (size_t)(end - (old + at)) + 1u : old_len - at;

		if (strncmp(old + at, SSH_BEGIN, len) == 0 && len == strlen(SSH_BEGIN)) {
			char *close_mark = strstr(old + at, SSH_END);

			at = close_mark != NULL ? (size_t)(close_mark - old) + strlen(SSH_END) : old_len;
			continue;
		}
		if (!remove && !placed && opens_block(old + at)) {
			memcpy(out + out_len, block, strlen(block));
			out_len += strlen(block);
			placed = 1;
		}
		memcpy(out + out_len, old + at, len);
		out_len += len;
		at += len;
	}
	if (!remove && !placed) {
		if (out_len > 0u && out[out_len - 1u] != '\n')
			out[out_len++] = '\n';
		memcpy(out + out_len, block, strlen(block));
		out_len += strlen(block);
	}
	free(old);
	fd = open(fresh, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
	if (fd < 0 || fchmod(fd, mode) != 0 || write(fd, out, out_len) != (ssize_t)out_len ||
	    close(fd) != 0 || rename(fresh, path) != 0) {
		fprintf(stderr, "hull: ssh-config: %s: %s\n", path, strerror(errno));
		unlink(fresh);
		free(out);
		return 1;
	}
	free(out);
	printf(remove ? "hull: ssh-config: took Host *.hull out of %s\n" :
	       "hull: ssh-config: Host *.hull in %s, through %s\n", path, self);
	return 0;
}

/* One reading of `id`'s figures, cpu, io and memory, into `values`: 0, 1
 * for the core's refusal, said, or 2 for an answer that is not one. */
static int read_stats(const char *id, long long values[3])
{
	static const char *const keys[] = { "cpu", "io", "memory" };
	hull_wire       wire;
	hull_sexpr_doc  body;
	hull_sexpr_form value;
	int             status = connect_core(&wire, 0);

	if (status == 1) {
		fprintf(stderr, "hull: no core is running, so no machine is\n");
		return 1;
	}
	if (status != 0)
		return 2;
	send_request(&wire, "(5:stats", 1u, id, NULL);
	if (hull_wire_read(&wire, &body) != HULL_WIRE_OK) {
		status = 2;
	} else if (hull_msg_is(&body, "error")) {
		char reason[512] = "an error";

		if (hull_msg_find(&body, "reason", &value))
			hull_msg_string(&body, &value, reason, sizeof reason);
		fprintf(stderr, "hull: %s\n", reason);
		status = 1;
	} else if (!hull_msg_is(&body, "stats")) {
		status = 2;
	} else {
		for (unsigned i = 0u; i < 3u; i++)
			if (!hull_msg_find(&body, keys[i], &value) ||
			    !hull_msg_long(&body, &value, &values[i]))
				status = 2;
	}
	hull_wire_close(&wire);
	if (status == 2)
		fprintf(stderr, "hull: the core's answer to stats is not stats\n");
	return status;
}

/* `hull stats ID`: one reading. With `--over S`, two, S seconds apart on
 * the monotonic clock, and the rates between them: CPU as a percentage of
 * one CPU, so that a machine using four whole CPUs is 400 %, as top
 * counts; io in bytes a second; and memory as the second reading has it,
 * being what is resident and not a flow. */
static int stats_of(const char *id, long over)
{
	long long       first[3] = { 0, 0, 0 };
	long long       second[3] = { 0, 0, 0 };
	struct timespec before;
	struct timespec after;
	struct timespec pause = { over, 0 };
	double          elapsed;
	int             status;

	if ((status = read_stats(id, first)) != 0)
		return status;
	if (over == 0) {
		printf("cpu     %lld.%06lld s\nio      %lld bytes\nmemory  %lld bytes\n",
		       first[0] / 1000000, first[0] % 1000000, first[1], first[2]);
		return 0;
	}
	clock_gettime(CLOCK_MONOTONIC, &before);
	while (nanosleep(&pause, &pause) != 0 && errno == EINTR)
		;
	if ((status = read_stats(id, second)) != 0)
		return status;
	clock_gettime(CLOCK_MONOTONIC, &after);
	elapsed = (double)(after.tv_sec - before.tv_sec) +
	          (double)(after.tv_nsec - before.tv_nsec) / 1e9;
	printf("cpu     %.1f %%\nio      %.0f bytes/s\nmemory  %lld bytes\n",
	       (double)(second[0] - first[0]) / 1e4 / elapsed,
	       (double)(second[1] - first[1]) / elapsed, second[2]);
	return 0;
}

/* stop, kill, logs and attach: one request about one machine, followed
 * to its end. A Ctrl-C under attach or logs only leaves. `signal_name`
 * is kill's, a name the core checks. */
static int about(const char *command, const char *id, int follow_logs, long timeout_ms,
                 const char *signal_name)
{
	hull_wire wire;
	char      rest[64];
	int       status;

	if ((status = connect_core(&wire, 0)) != 0) {
		if (status == 1)
			fprintf(stderr, "hull: no core is running, so no machine is\n");
		return status == 1 ? 1 : 2;
	}
	if (strcmp(command, "stop") == 0) {
		snprintf(rest, sizeof rest, "(10:timeout_ms%zu:%ld)",
		         (size_t)snprintf(NULL, 0, "%ld", timeout_ms), timeout_ms);
		send_request(&wire, "(4:stop", 1u, id, rest);
	} else if (strcmp(command, "kill") == 0) {
		snprintf(rest, sizeof rest, "(6:signal%zu:%s)", strlen(signal_name), signal_name);
		send_request(&wire, "(4:kill", 1u, id, rest);
	} else if (strcmp(command, "logs") == 0) {
		send_request(&wire, "(4:logs", 1u, id,
		             follow_logs ? "(6:follow4:true)" : "(6:follow5:false)");
	} else {
		send_request(&wire, "(6:attach", 1u, id, NULL);
	}
	status = follow(&wire, 1u, NULL);
	hull_wire_close(&wire);
	if (strcmp(command, "attach") == 0)
		return status;
	return status == 125 ? 1 : 0;
}

/* kill's -s: TERM, SIGTERM or term, as the protocol's TERM, or NULL for
 * one too long to be a signal's name. Which signals the core forwards is
 * the core's to say. */
static const char *canonical_signal(const char *given)
{
	static char name[8];
	size_t      n;

	if (strncasecmp(given, "SIG", 3u) == 0)
		given += 3;
	n = strlen(given);
	if (n == 0u || n >= sizeof name)
		return NULL;
	for (size_t i = 0u; i <= n; i++)
		name[i] = (char)toupper((unsigned char)given[i]);
	return name;
}

static void list_engines(const hull_registry *registry)
{
	for (uint32_t i = 0u; i < registry->count; i++) {
		const hull_capabilities *caps = registry->engines[i]->caps;

		printf("%s:", caps->engine_name);
		for (const char *const *c = caps->classes; *c != NULL; c++)
			printf(" %s", *c);
		printf("%s\n", caps->requires_root ? " (runs as root)" : "");
		for (const hull_cap_entry *e = caps->entries; e->field != NULL; e++) {
			printf("  %-28s %s", e->field, e->support == HULL_SUP_NATIVE ? "native" :
			       e->support == HULL_SUP_EMULATED ? "emulated" : "rejected");
			if (e->note != NULL)
				printf(": %s", e->note);
			if (e->accept != NULL)
				printf(" (--accept %s)", e->accept);
			printf("\n");
		}
	}
}

/* Split "a,b,c" into `o->backends`, in place. */
static int add_backends(options *o, char *list)
{
	for (char *next = strtok(list, ","); next != NULL; next = strtok(NULL, ",")) {
		if (o->backend_count == MAX_BACKENDS) {
			fprintf(stderr, "hull: at most %u backends\n", MAX_BACKENDS);
			return 0;
		}
		o->backends[o->backend_count++] = next;
	}
	return 1;
}

int main(int argc, char **argv)
{
	hull_registry registry;
	options       o;
	const char   *command;
	const char   *reason = NULL;
	const char   *name   = NULL;

	memset(&o, 0, sizeof o);
	o.file   = "hull.conf";
	o.stream = stdout;
	if (argc < 2) {
		usage(stderr);
		return 2;
	}
	command = argv[1];
	if (strcmp(command, "--version") == 0) {
		version();
		return 0;
	}
	if (strcmp(command, "--help") == 0 || strcmp(command, "help") == 0) {
		usage(stdout);
		return 0;
	}

	hull_registry_init(&registry, host_arch());
	if (strcmp(command, "core") == 0 && argc == 2)
		return hull_core_run(host_arch());
	if (strcmp(command, "ps") == 0 && argc == 2)
		return list_machines();
	if (strcmp(command, "gc") == 0 && argc == 2)
		return collect();
	if (strcmp(command, "commit") == 0 && argc == 4)
		return commit(argv[2], argv[3], host_arch());
	if (strcmp(command, "commit") == 0 && argc == 6 && strcmp(argv[2], "--arch") == 0)
		return commit(argv[4], argv[5], argv[3]);
	if (strcmp(command, "materialize") == 0 && argc == 4)
		return materialize(argv[2], argv[3]);
	if (strcmp(command, "images") == 0 && argc == 2)
		return images();
	if (strcmp(command, "diff") == 0 && argc == 4)
		return diff(argv[2], argv[3]);
	if (strcmp(command, "proxy-ssh") == 0 && argc == 3)
		return proxy_ssh(argv[2]);
	if (strcmp(command, "ssh-config") == 0 && argc == 2)
		return ssh_config(0);
	if (strcmp(command, "ssh-config") == 0 && argc == 3 && strcmp(argv[2], "--remove") == 0)
		return ssh_config(1);
	if (strcmp(command, "exec") == 0) {
		int at  = 2;
		int tty = 0;
		int first;

		if (argc > at && strcmp(argv[at], "-t") == 0) {
			tty = 1;
			at++;
		}
		if (argc <= at + 1 || argv[at][0] == '-') {
			usage(stderr);
			return 125;
		}
		first = at + 1;
		if (strcmp(argv[first], "--") == 0)
			first++;
		if (first >= argc) {
			usage(stderr);
			return 125;
		}
		return exec_in(argv[at], argv + first, argc - first, tty, 0u);
	}
	if ((strcmp(command, "stop") == 0 || strcmp(command, "attach") == 0 ||
	     strcmp(command, "logs") == 0 || strcmp(command, "kill") == 0) && argc >= 3) {
		const char *id          = NULL;
		const char *signal_name = "KILL";
		int         follow      = 0;
		long        timeout     = 10000;

		for (int i = 2; i < argc; i++) {
			if (strcmp(command, "logs") == 0 && strcmp(argv[i], "-f") == 0)
				follow = 1;
			else if (strcmp(command, "stop") == 0 && strcmp(argv[i], "--timeout") == 0 &&
			         i + 1 < argc)
				timeout = strtol(argv[++i], NULL, 10) * 1000L;
			else if (strcmp(command, "kill") == 0 && strcmp(argv[i], "-s") == 0 &&
			         i + 1 < argc)
				signal_name = canonical_signal(argv[++i]);
			else if (argv[i][0] != '-' && id == NULL)
				id = argv[i];
			else {
				id = NULL;
				break;
			}
		}
		if (id == NULL || signal_name == NULL) {
			usage(stderr);
			return 2;
		}
		return about(command, id, follow, timeout < 0 ? 0 : timeout, signal_name);
	}

	if (strcmp(command, "stats") == 0 && argc == 3 && argv[2][0] != '-')
		return stats_of(argv[2], 0);
	if (strcmp(command, "stats") == 0 && argc == 5 && strcmp(argv[2], "--over") == 0 &&
	    argv[4][0] != '-') {
		char *end = NULL;
		long  over = strtol(argv[3], &end, 10);

		if (end == argv[3] || *end != '\0' || over < 1 || over > 3600) {
			fprintf(stderr, "hull: stats --over takes 1 to 3600 seconds\n");
			return 2;
		}
		return stats_of(argv[4], over);
	}
	if (strcmp(command, "ssh") == 0 && argc == 3 && argv[2][0] != '-')
		return ssh_into(argv[2]);

	/* Planning needs the engines' declarations, not their operations,
	 * which run in the core: nothing here reports to a core. */
	if (hull_registry_add(&registry, hull_engine_docker(NULL), &reason) != HULL_REGISTRY_OK ||
	    hull_registry_add(&registry, hull_engine_qemu(NULL), &reason) != HULL_REGISTRY_OK ||
	    hull_registry_add(&registry, hull_engine_chroot(NULL), &reason) != HULL_REGISTRY_OK) {
		fprintf(stderr, "hull: a built-in engine is refused: %s\n", reason);
		return 2;
	}

	if (strcmp(command, "engines") == 0 && argc == 2) {
		list_engines(&registry);
		return 0;
	}
	if (strcmp(command, "plan") != 0 && strcmp(command, "check") != 0 &&
	    strcmp(command, "run") != 0) {
		usage(stderr);
		return 2;
	}
	for (int i = 2; i < argc; i++) {
		const char *arg = argv[i];

		if (strcmp(arg, "-f") == 0 && i + 1 < argc) {
			o.file = argv[++i];
		} else if (strcmp(arg, "--backend") == 0 && i + 1 < argc) {
			if (!add_backends(&o, argv[++i]))
				return 2;
		} else if (strcmp(arg, "--accept") == 0 && i + 1 < argc) {
			if (o.token_count == MAX_TOKENS) {
				fprintf(stderr, "hull: at most %u tokens\n", MAX_TOKENS);
				return 2;
			}
			o.tokens[o.token_count++] = argv[++i];
		} else if (strcmp(arg, "--explain") == 0 && strcmp(command, "plan") == 0) {
			o.explain = 1;
		} else if (strcmp(arg, "--resolved") == 0 && strcmp(command, "plan") == 0) {
			o.resolved = 1;
		} else if (strcmp(command, "run") == 0 && arg[0] != '-' && name == NULL) {
			name = arg;
		} else {
			usage(stderr);
			return 2;
		}
	}
	if (strcmp(command, "check") != 0 && o.backend_count > 1u) {
		fprintf(stderr, "hull: %s takes one backend; check takes several\n", command);
		return strcmp(command, "run") == 0 ? 125 : 2;
	}
	o.base = base_of(o.file);
	if (o.base == NULL) {
		fprintf(stderr, "%s: %s\n", o.file, strerror(errno));
		return strcmp(command, "run") == 0 ? 125 : 2;
	}
	if (strcmp(command, "run") == 0) {
		o.stream = stderr;
		return run(&o, &registry, name);
	}
	if (strcmp(command, "check") == 0 && o.backend_count == 0u) {
		fprintf(stderr, "hull: check needs --backend, the backends to hold the spec to\n");
		return 2;
	}
	return resolve_all(&o, &registry);
}
