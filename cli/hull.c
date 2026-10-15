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

#define _XOPEN_SOURCE 700

#include <errno.h>
#include <libgen.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>

#include "builtin.h"
#include "matrix.h"
#include "plan.h"
#include "registry.h"
#include "spec.h"

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

/* --- run ---------------------------------------------------------------- */

/* What the engine reports back while one machine runs. */
struct hull_core {
	pthread_mutex_t lock;
	pthread_cond_t  cond;
	int             exited;
	int             code;
	int             signal;
};

static hull_core              core = { PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER,
                                       0, 0, 0 };
static volatile sig_atomic_t  interrupted;

static void on_data(hull_core *c, hull_stream *stream, int fd, const uint8_t *data, size_t n)
{
	(void)c;
	(void)stream;
	while (n > 0u) {
		ssize_t wrote = write(fd == 2 ? 2 : 1, data, n);

		if (wrote < 0 && errno == EINTR)
			continue;
		if (wrote <= 0)
			return;
		data += wrote;
		n -= (size_t)wrote;
	}
}

static void on_eof(hull_core *c, hull_stream *stream, int fd)
{
	(void)c;
	(void)stream;
	(void)fd;
}

static void on_exit_status(hull_core *c, hull_machine *machine, hull_stream *stream, int code,
                           int signal)
{
	(void)machine;
	(void)stream;
	pthread_mutex_lock(&c->lock);
	c->exited = 1;
	c->code   = code;
	c->signal = signal;
	pthread_cond_broadcast(&c->cond);
	pthread_mutex_unlock(&c->lock);
}

static void on_state(hull_core *c, hull_machine *machine, const char *state)
{
	(void)c;
	(void)machine;
	(void)state;
}

static void on_log(hull_core *c, int level, const char *message)
{
	(void)c;
	if (level >= 2)
		fprintf(stderr, "hull: %s\n", message);
}

static const hull_host_ops host_ops = {
	.struct_size = sizeof (hull_host_ops),
	.on_data     = on_data,
	.on_eof      = on_eof,
	.on_exit     = on_exit_status,
	.on_state    = on_state,
	.log         = on_log,
};

static void interrupt(int signal)
{
	(void)signal;
	interrupted = 1;
}

/* Until the workload exits, asking the engine to stop it on SIGINT or
 * SIGTERM. Returns the status run exits with. */
static int wait_for(const hull_engine_ops *ops, hull_machine *machine)
{
	int stopping = 0;

	pthread_mutex_lock(&core.lock);
	while (!core.exited) {
		struct timespec deadline;

		if (interrupted && !stopping) {
			hull_error error = { .struct_size = sizeof error };

			stopping = 1;
			pthread_mutex_unlock(&core.lock);
			ops->stop(&core, machine, 10000, &error);
			pthread_mutex_lock(&core.lock);
			continue;
		}
		clock_gettime(CLOCK_REALTIME, &deadline);
		deadline.tv_nsec += 200000000L;
		if (deadline.tv_nsec >= 1000000000L) {
			deadline.tv_sec++;
			deadline.tv_nsec -= 1000000000L;
		}
		pthread_cond_timedwait(&core.cond, &core.lock, &deadline);
	}
	pthread_mutex_unlock(&core.lock);
	return core.signal != 0 ? 128 + core.signal : core.code;
}

static int run(options *o, const hull_registry *registry, const char *name)
{
	static hull_plan     plan;
	hull_spec            spec;
	hull_spec_error      error;
	hull_sexpr_form      machine;
	hull_choice          choice;
	hull_machine        *handle = NULL;
	hull_error           failure = { .struct_size = sizeof failure };
	const hull_engine_ops *ops;
	const char          *reason = NULL;
	const char          *shown;
	struct sigaction     action;
	report               r;
	hull_plan_status     planned;
	uint8_t             *data;
	size_t               length;
	int                  status;

	data = read_file(o->file, &length);
	if (data == NULL)
		return 125;
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
	ops = choice.engine->ops;
	if (ops->create == NULL || ops->start == NULL || ops->stop == NULL || ops->destroy == NULL) {
		fprintf(stderr, "%s: %s: the %s engine cannot run a machine yet\n", o->file, shown,
		        choice.engine->caps->engine_name);
		return 125;
	}

	memset(&action, 0, sizeof action);
	action.sa_handler = interrupt;
	sigaction(SIGINT, &action, NULL);
	sigaction(SIGTERM, &action, NULL);

	if (ops->create(&core, (hull_buf){ plan.canonical, plan.canonical_length }, &handle,
	                &failure) != HULL_OK) {
		fprintf(stderr, "%s: %s on %s: %s\n", o->file, shown,
		        choice.engine->caps->engine_name, failure.detail);
		return 125;
	}
	if (ops->start(&core, handle, &failure) != HULL_OK) {
		fprintf(stderr, "%s: %s on %s: %s\n", o->file, shown,
		        choice.engine->caps->engine_name, failure.detail);
		ops->destroy(&core, handle, &failure);
		return 125;
	}
	status = wait_for(ops, handle);
	ops->destroy(&core, handle, &failure);
	free(data);
	return status;
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
	if (hull_registry_add(&registry, hull_engine_docker(&host_ops), &reason) != HULL_REGISTRY_OK ||
	    hull_registry_add(&registry, hull_engine_qemu(&host_ops), &reason) != HULL_REGISTRY_OK ||
	    hull_registry_add(&registry, hull_engine_chroot(&host_ops), &reason) != HULL_REGISTRY_OK) {
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
