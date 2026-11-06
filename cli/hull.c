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
#include <linux/openat2.h>
#include <poll.h>
#include <pwd.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <fcntl.h>
#include <ftw.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/un.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include "access.h"
#include "builtin.h"
#include "core.h"
#include "doctor.h"
#include "export.h"
#include "hostnet.h"
#include "kernel_fetch.h"
#include "sshd_fetch.h"
#include "media.h"
#include "vagrant.h"
#include "vbox_fetch.h"
#include "vbox_host.h"
#include "import.h"
#include "dial.h"
#include "matrix.h"
#include "names.h"
#include "plan.h"
#include "registry.h"
#include "runtime.h"
#include "sha256.h"
#include "spec.h"
#include "pull.h"
#include "push.h"
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
	int         root_hash;  /* plan --root-hash: each machine's, to accept */
	long long   wait;       /* run --wait D in seconds, 0 --no-wait, -1 for ever (4.12) */
	const char *lease;      /* run --lease ID: a slot of that lease's, or NULL */
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
	unsigned              root_rejected;    /* fields only root mode would honour */
	char                  root_token[80];   /* this machine's root:sha256:HEX */
	char                  root_fields[512]; /* those fields, each after a space */
} report;

/* A run the core refused: 75, EX_TEMPFAIL, where its pool had no slot
 * within its wait, which a pipeline may try again (section 4.12); else
 * 125, as every refusal before it. The statuses are an interface. */
static int refused_status(const char *reason)
{
	return strncmp(reason, "no slot of pool ", 16u) == 0 ? 75 : 125;
}

static void usage(FILE *out)
{
	fputs("usage: hull plan [-f FILE] [--backend B] [--accept TOKEN]... [--explain]"
	      " [--resolved] [--root-hash]\n"
	      "       hull check [-f FILE] --backend B[,B...] [--accept TOKEN]...\n"
	      "       hull run [-f FILE] [--backend B] [--accept TOKEN]... [--wait D|--no-wait]\n"
	      "                [--lease ID] [NAME]\n"
	      "       hull reserve POOL [--count N] [--wait D|--no-wait] | hull release ID\n"
	      "       hull pool ls | hull lease ls\n"
	      "       hull pack DIR|REF FILE | hull unpack FILE|- DIR\n"
	      "       hull export --bootable DIR|REF IMAGE [--init PATH | --host TTY]\n"
	      "       hull kernel fetch [--arch ARCH]\n"
	      "       hull sshd fetch\n"
	      "       hull virtualbox fetch\n"
	      "       hull export --bootable -f FILE [NAME] IMAGE\n"
	      "       hull build [-f FILE] [--check] REF\n"
	      "       hull ps | hull stop [--timeout S] ID | hull logs [-f] ID | hull attach ID\n"
	      "       hull exec [-t] ID [--] CMD [ARG...]\n"
	      "       hull kill [-s SIGNAL] ID | hull stats [--guest] [--over S] ID\n"
	      "       hull pause ID | hull unpause ID\n"
	      "       hull segment NAME [impair MACHINE|all in|out|both [KEY=VALUE|down]...]\n"
	      "       hull segment NAME tamper MACHINE|all in|out|both [KEY=VALUE]...\n"
	      "       hull segment NAME inject MACHINE|all frame=HEX|size=N [count=N] [every=MS]\n"
	      "       hull segment NAME capture MACHINE|all in|out FILE [frames=N] | capture stop\n"
	      "       hull ssh ID | hull proxy-ssh HOST\n"
	      "       hull ssh-config [--remove] | hull gc\n"
	      "       hull systemd [-f FILE] [NAME]\n"
	      "       hull commit [--arch A] DIR|ID REF | hull commit --tar FILE REF\n"
	      "       hull materialize REF DIR | hull images\n"
	      "       hull diff REF REF | hull tag REF NEW | hull tag -d REF\n"
	      "       hull manifest show REF | hull manifest compile [REF]\n"
	      "       hull manifest composefs REF FILE\n"
	      "       hull pull [--arch A] IMAGE [REF] | hull push REF IMAGE | hull load FILE [REF]\n"
	      "       hull import [--build-arg N=V]... [--target STAGE] DOCKERFILE\n"
	      "       hull import COMPOSE-FILE | hull import IMAGE DIR\n"
	      "       hull media import IMAGE DIR | verify DIR | export DIR IMAGE\n"
	      "       hull media change ID DRIVE [IMAGE]\n"
	      "       hull vagrant box add NAME FILE\n"
	      "       hull up [-f COMPOSE-FILE] [--backend B] [--profile P]... [NAME]\n"
	      "       hull core\n"
	      "       hull engines | hull doctor\n"
	      "       hull -H ssh://[USER@]HOST[:PORT][/DIR] COMMAND ...\n"
	      "       hull --version\n", out);
}

/* The commit, where the build that made this one wrote it: fmake's does
 * not, and says so. */
#if defined(__has_include)
#if __has_include("commit.h")
#include "commit.h"
#endif
#endif
#ifndef HULL_COMMIT
#define HULL_COMMIT "unknown"
#endif

/* Which hull made a tree, as A.5's builder records it on every manifest
 * hull writes, a commit's as well as a build's and a pull's: so that a
 * stored image says, as long as it lasts, which hull it was made by. */
#define HULL_BUILDER "hull " HULL_VERSION " (" HULL_COMMIT ")"

/* The first line is what scripts read, so it keeps its shape; the commit
 * is a line of its own. */
static void version(void)
{
	printf("hull %s\n", HULL_VERSION);
	printf("Copyright (C) 2026 Nabeel Sowan <nabeel@vibes.se>\n");
	printf("commit %s\n", HULL_COMMIT);
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
	if (resolution->outcome == HULL_OUTCOME_REJECTED && resolution->token != NULL &&
	    strncmp(resolution->token, "root:", 5u) == 0) {
		size_t at = strlen(r->root_fields);

		r->root_rejected++;
		snprintf(r->root_token, sizeof r->root_token, "%s", resolution->token);
		snprintf(r->root_fields + at, sizeof r->root_fields - at, " %s", resolution->id);
	}
	if (resolution->outcome == HULL_OUTCOME_NATIVE && !r->options->explain)
		return;
	hull_sexpr_locate(&r->spec->doc, resolution->offset, &line, &column);
	fprintf(r->options->stream, "%s:%u:%u: %s on %s (%s): %s: %s", r->options->file, line,
	        column, r->machine, r->choice->engine->caps->engine_name, r->choice->class_name,
	        resolution->id, outcome_name(resolution->outcome));
	if (resolution->note != NULL)
		fprintf(r->options->stream, ": %s", resolution->note);
	if (resolution->outcome == HULL_OUTCOME_REJECTED && resolution->token != NULL &&
	    strcmp(resolution->token, HULL_MATRIX_ROOT_SELF) == 0)
		fprintf(r->options->stream, " (hull run as root honours it)");
	else if (resolution->outcome == HULL_OUTCOME_REJECTED && resolution->token != NULL)
		fprintf(r->options->stream, " (--accept %s honours it)", resolution->token);
	fprintf(r->options->stream, "\n");
}

/* Whether a root acceptance was given and this machine's is not it: its
 * plan changed since, or another's was given. Said loudly, with the hash
 * to accept now and the line of the host's listing that would grant it,
 * for status 77 (section 7). */
static int root_changed(const report *r)
{
	const struct passwd *who = getpwuid(getuid());
	int                  given = 0;

	for (unsigned i = 0u; r->options->tokens[i] != NULL; i++)
		given |= strncmp(r->options->tokens[i], "root:", 5u) == 0;
	if (!given || r->root_rejected == 0u)
		return 0;
	fprintf(stderr, "%s: %s on %s (%s): root mode was accepted for another plan: this one is "
	        "--accept %s, and /etc/hull/root.conf lists it as:\n%s %s%s\n", r->options->file,
	        r->machine, r->choice->engine->caps->engine_name, r->choice->class_name,
	        r->root_token, who != NULL ? who->pw_name : "USER", r->root_token + 5,
	        r->root_fields);
	return 1;
}

/* Root mode is offered only for a field every rootless engine on the host
 * refuses (section 7): where one honours a field the plan has root mode
 * honour, that engine's name, said, and the field into `*field`; NULL
 * where none does. */
static const char *rootless_honours(const hull_registry *registry, const hull_plan *plan,
                                    const char **field)
{
	for (unsigned f = 0u; f < plan->root_count; f++) {
		const hull_engine *rootless = hull_registry_rootless(registry, plan->root_fields[f]);

		if (rootless != NULL) {
			*field = plan->root_fields[f];
			return rootless->caps->engine_name;
		}
	}
	return NULL;
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

/* A spec's text: the file at `path`, or where it is a compose file, the
 * (machines ...) form section 9's compose dialect translates it to, in a
 * buffer of MAX_FILE as read_file()'s is. What it leaves out is said on
 * stderr, once in a process however often the file is read. */
static uint8_t *spec_text(const char *path, size_t *length)
{
	static int said;
	uint8_t   *data;
	char      *text;
	size_t     n;

	if (!hull_import_is_compose(path) && !hull_vagrant_is_file(path))
		return read_file(path, length);
	text = hull_vagrant_is_file(path) ? hull_vagrant_form(path, said ? NULL : stderr) :
	       hull_import_compose(path, said ? NULL : stderr);
	said = 1;
	if (text == NULL)
		return NULL;
	n = strlen(text);
	if (n > MAX_FILE || (data = malloc(MAX_FILE)) == NULL) {
		fprintf(stderr, "%s: its translation is too large\n", path);
		free(text);
		return NULL;
	}
	memcpy(data, text, n);
	free(text);
	*length = n;
	return data;
}

/* The compose file `hull up` runs, in the working directory, in the order
 * the Compose specification looks for one; NULL where there is none. */
static const char *compose_file(void)
{
	static const char *const names[] = {
		"compose.yaml", "compose.yml", "docker-compose.yaml", "docker-compose.yml", "Vagrantfile"
	};

	for (size_t i = 0u; i < sizeof names / sizeof names[0]; i++)
		if (access(names[i], F_OK) == 0)
			return names[i];
	return NULL;
}

/* --- a source the store holds --------------------------------------------- */

/* One rewrite of the file's text: the bytes from `at`, `cut` of them,
 * become `text`, which is malloc'd. */
typedef struct rewrite {
	uint32_t at;
	uint32_t cut;
	char    *text;
} rewrite;

#define MAX_REWRITES 256u

/* Text built up; `failed` once memory ran out. */
typedef struct text_buf {
	char  *data;
	size_t length;
	size_t cap;
	int    failed;
} text_buf;

static void add_bytes(text_buf *t, const void *bytes, size_t n)
{
	if (t->failed)
		return;
	if (t->length + n + 1u > t->cap) {
		size_t want = (t->length + n + 1u) * 2u + 64u;
		char  *grown = realloc(t->data, want);

		if (grown == NULL) {
			t->failed = 1;
			return;
		}
		t->data = grown;
		t->cap = want;
	}
	memcpy(t->data + t->length, bytes, n);
	t->length += n;
	t->data[t->length] = '\0';
}

static void add_text(text_buf *t, const char *text)
{
	add_bytes(t, text, strlen(text));
}

/* Back to `length`, as it was before what is taken back. */
static void cut_text(text_buf *t, size_t length)
{
	t->length = length;
	if (t->data != NULL)
		t->data[length] = '\0';
}

/* `bytes` as a hull.conf string: 0, or -1 for bytes a string cannot hold,
 * a control byte other than a newline or a tab. */
static int add_quoted(text_buf *t, const uint8_t *bytes, size_t n)
{
	add_text(t, "\"");
	for (size_t i = 0u; i < n; i++) {
		if (bytes[i] == '"' || bytes[i] == '\\') {
			char escaped[2] = { '\\', (char)bytes[i] };

			add_bytes(t, escaped, 2u);
		} else if (bytes[i] == '\n') {
			add_text(t, "\\n");
		} else if (bytes[i] == '\t') {
			add_text(t, "\\t");
		} else if (bytes[i] < 0x20u || bytes[i] == 0x7fu) {
			return -1;
		} else {
			add_bytes(t, bytes + i, 1u);
		}
	}
	add_text(t, "\"");
	return 0;
}

/* A name a hull.conf key can be as it stands. */
static int is_key(const uint8_t *bytes, size_t n)
{
	if (n == 0u)
		return 0;
	for (size_t i = 0u; i < n; i++)
		if (!((bytes[i] >= 'a' && bytes[i] <= 'z') || (bytes[i] >= 'A' && bytes[i] <= 'Z') ||
		      (bytes[i] >= '0' && bytes[i] <= '9') || bytes[i] == '_' || bytes[i] == '.' ||
		      bytes[i] == '-'))
			return 0;
	return 1;
}

/* `(key ...)` in a canonical record. */
static int record_find(const hull_sexpr_doc *doc, const hull_sexpr_form *record, const char *key,
                       hull_sexpr_form *found)
{
	hull_sexpr_form item;
	hull_sexpr_form name;
	int             more;

	hull_sexpr_child(doc, record, &item);
	for (more = hull_sexpr_next(doc, &item, &item); more; more = hull_sexpr_next(doc, &item, &item))
		if (item.kind == HULL_SEXPR_LIST && hull_sexpr_child(doc, &item, &name) &&
		    hull_sexpr_is(doc, &name, key)) {
			*found = item;
			return 1;
		}
	return 0;
}

/* Each word of `(key word ...)` in the runtime, quoted onto `t`. */
static int add_words(text_buf *t, const hull_sexpr_doc *doc, const hull_sexpr_form *runtime,
                     const char *key, int *any)
{
	hull_sexpr_form list;
	hull_sexpr_form word;
	int             more;

	if (!record_find(doc, runtime, key, &list))
		return 0;
	hull_sexpr_child(doc, &list, &word);
	for (more = hull_sexpr_next(doc, &word, &word); more; more = hull_sexpr_next(doc, &word, &word)) {
		add_text(t, " ");
		if (add_quoted(t, doc->data + word.content, word.content_length) != 0)
			return -1;
		*any = 1;
	}
	return 0;
}

static int add_rewrite(rewrite *rewrites, unsigned *count, uint32_t at, uint32_t cut, text_buf *t)
{
	if (*count >= MAX_REWRITES || t->failed) {
		free(t->data);
		return -1;
	}
	rewrites[*count] = (rewrite){ at, cut, t->data };
	(*count)++;
	*t = (text_buf){ 0 };
	return 0;
}

/*
 * A.4's defaults, from a stored tree's manifest, written where the machine
 * says nothing of its own: its command, the entrypoint and then the
 * command, as docker runs them; its workdir; and each env name it does not
 * set, so that its own win, as J.1 says the machine's env is merged over
 * the manifest's. The image's user is said and not taken, since J.1 has
 * no key to take it into; its ports and volumes are advisory.
 */
static int runtime_defaults(const char *file, const char *name, const hull_spec *spec,
                            const hull_sexpr_form *machine, const uint8_t *runtime_bytes,
                            size_t runtime_length, rewrite *rewrites, unsigned *count)
{
	hull_sexpr_doc  doc;
	hull_sexpr_form runtime;
	hull_sexpr_form found;
	hull_sexpr_form env;
	text_buf        at_end = { 0 };
	text_buf        into_env = { 0 };
	int             has_env = hull_spec_find(spec, machine, "env", &env);
	int             any = 0;

	if (hull_sexpr_open(&doc, runtime_bytes, runtime_length, HULL_SEXPR_CANONICAL, NULL) !=
	        HULL_SEXPR_OK ||
	    !hull_sexpr_first(&doc, &runtime))
		return 0;
	if (!hull_spec_find(spec, machine, "command", &found)) {
		text_buf words = { 0 };

		if (add_words(&words, &doc, &runtime, "entrypoint", &any) != 0 ||
		    add_words(&words, &doc, &runtime, "cmd", &any) != 0) {
			fprintf(stderr, "%s: %s: its command holds what a hull.conf string cannot\n", file,
			        name);
			free(words.data);
			return -1;
		}
		if (any) {
			add_text(&at_end, " (command");
			add_bytes(&at_end, words.data, words.length);
			add_text(&at_end, ")");
		}
		free(words.data);
	}
	if (!hull_spec_find(spec, machine, "workdir", &found) &&
	    record_find(&doc, &runtime, "workdir", &found)) {
		hull_sexpr_form head;
		hull_sexpr_form path;

		hull_sexpr_child(&doc, &found, &head);
		hull_sexpr_next(&doc, &head, &path);
		add_text(&at_end, " (workdir ");
		if (add_quoted(&at_end, doc.data + path.content, path.content_length) != 0) {
			free(at_end.data);
			return -1;
		}
		add_text(&at_end, ")");
	}
	if (record_find(&doc, &runtime, "env", &found)) {
		hull_sexpr_form pair;
		text_buf       *to = has_env ? &into_env : &at_end;
		size_t          before_env = at_end.length;
		int             more;
		int             added = 0;

		if (!has_env)
			add_text(&at_end, " (env");
		hull_sexpr_child(&doc, &found, &pair);
		for (more = hull_sexpr_next(&doc, &pair, &pair); more;
		     more = hull_sexpr_next(&doc, &pair, &pair)) {
			hull_sexpr_form key;
			hull_sexpr_form value;
			hull_sexpr_form mine;
			char            text[256];
			size_t          before = to->length;

			hull_sexpr_child(&doc, &pair, &key);
			hull_sexpr_next(&doc, &key, &value);
			if (!is_key(doc.data + key.content, key.content_length) ||
			    key.content_length >= sizeof text) {
				fprintf(stderr, "%s: %s: its env %.*s is not a name hull.conf takes, left out\n",
				        file, name, (int)key.content_length,
				        (const char *)doc.data + key.content);
				continue;
			}
			memcpy(text, doc.data + key.content, key.content_length);
			text[key.content_length] = '\0';
			if (has_env && hull_spec_find(spec, &env, text, &mine))
				continue;  /* the machine's own */
			add_text(to, " (");
			add_text(to, text);
			add_text(to, " ");
			if (add_quoted(to, doc.data + value.content, value.content_length) != 0) {
				fprintf(stderr, "%s: %s: its env %s holds what a hull.conf string cannot, "
				        "left out\n", file, name, text);
				cut_text(to, before);
				continue;
			}
			add_text(to, ")");
			added = 1;
		}
		if (!has_env && !added)
			cut_text(&at_end, before_env);
		else if (!has_env)
			add_text(&at_end, ")");
	}
	/* And a stop as the image asks, SIGTERM being what one is anyway. */
	if (!hull_spec_find(spec, machine, "stop_signal", &found) &&
	    record_find(&doc, &runtime, "stop_signal", &found)) {
		hull_sexpr_form head;
		hull_sexpr_form signal_name;

		hull_sexpr_child(&doc, &found, &head);
		hull_sexpr_next(&doc, &head, &signal_name);
		if (!hull_sexpr_is(&doc, &signal_name, "SIGTERM") &&
		    !hull_sexpr_is(&doc, &signal_name, "TERM") &&
		    signal_name.content_length < 40u) {
			char text[64];

			snprintf(text, sizeof text, " (stop_signal \"%.*s\")",
			         (int)signal_name.content_length, (const char *)doc.data + signal_name.content);
			add_text(&at_end, text);
		}
	}
	if (record_find(&doc, &runtime, "user", &found)) {
		hull_sexpr_form head;
		hull_sexpr_form user;
		hull_sexpr_form mine;
		int             has_user = hull_spec_find(spec, machine, "user", &mine);

		hull_sexpr_child(&doc, &found, &head);
		hull_sexpr_next(&doc, &head, &user);
		/* As docker runs it, where the machine says nothing of its own. */
		if (!hull_sexpr_is(&doc, &user, "root") && !hull_sexpr_is(&doc, &user, "0") &&
		    !hull_sexpr_is(&doc, &user, "0:0") && !has_user) {
			char text[160];

			if (user.content_length < sizeof text - 20u) {
				snprintf(text, sizeof text, " (user \"%.*s\")", (int)user.content_length,
				         (const char *)doc.data + user.content);
				add_text(&at_end, text);
			}
		}
	}
	if (into_env.length > 0u &&
	    add_rewrite(rewrites, count, env.offset + env.extent - 1u, 0u, &into_env) != 0)
		return -1;
	free(into_env.data);
	if (at_end.length > 0u &&
	    add_rewrite(rewrites, count, machine->offset + machine->extent - 1u, 0u, &at_end) != 0)
		return -1;
	free(at_end.data);
	return 0;
}

static int rewrite_order(const void *a, const void *b)
{
	uint32_t at = ((const rewrite *)a)->at;
	uint32_t bt = ((const rewrite *)b)->at;

	return at < bt ? 1 : at > bt ? -1 : 0;
}

/*
 * A machine whose source is a bare ref the store holds runs that tree, as
 * J.1 says a bare string is an image ref in the store or a registry: its
 * source becomes a dir source on the tree's checkout, under an overlay
 * where it says no (mutable ...), so that every engine taking a dir source
 * runs it and none writes the checkout, and the tree's A.4 defaults fill
 * what the machine leaves out. It is written into the file's text, which
 * `data` holds in MAX_FILE bytes, before anything reads it as a spec, so
 * the registry, the matrix and the plan all judge what the engine is
 * given, and a plan shows it. A ref the store does not hold is left alone,
 * an image ref for docker. With `create` the checkout is made, as a run
 * needs; a plan only names it. Returns 0, or -1 having said why.
 */
static int open_store(hull_store *store, int create);

/* The tarball `given` names, relative to the file `file` where it is not
 * absolute, committed to the store as `tar/` and the first 32 hex digits
 * of its bytes' sha256, its arch the machine's or the host's; that ref
 * into `ref`. 0, or -1 having said why. */
static int tar_ref(const char *file, const hull_spec *spec, const hull_sexpr_form *machine,
                   const char *given, char *ref, size_t cap)
{
	char            path[4400];
	char            arch[64];
	char            why[4400] = "";
	uint8_t         chunk[65536];
	uint8_t         digest[32];
	char            hex[65];
	hull_sha256     h;
	hull_sexpr_form entry;
	hull_sexpr_form head;
	hull_sexpr_form value;
	hull_store      store;
	hull_commit     made;
	ssize_t         n;
	int             fd;
	int             status;

	if (given[0] == '/') {
		snprintf(path, sizeof path, "%s", given);
	} else {
		const char *slash = strrchr(file, '/');

		snprintf(path, sizeof path, "%.*s%s", slash != NULL ? (int)(slash - file + 1) : 0, file,
		         given);
	}
	if ((fd = open(path, O_RDONLY | O_CLOEXEC)) < 0) {
		fprintf(stderr, "%s: tar:%s: %s\n", file, given, strerror(errno));
		return -1;
	}
	hull_sha256_init(&h);
	while ((n = read(fd, chunk, sizeof chunk)) > 0)
		hull_sha256_update(&h, chunk, (size_t)n);
	hull_sha256_final(&h, digest);
	hull_sha256_hex(digest, hex);
	snprintf(ref, cap, "tar/%.32s", hex);
	snprintf(arch, sizeof arch, "%s", host_arch());
	if (hull_spec_find(spec, machine, "arch", &entry) && hull_sexpr_child(&spec->doc, &entry, &head) &&
	    hull_sexpr_next(&spec->doc, &head, &value) && value.content_length < sizeof arch &&
	    !hull_sexpr_is(&spec->doc, &value, "host")) {
		memcpy(arch, spec->doc.data + value.content, value.content_length);
		arch[value.content_length] = '\0';
	}
	if (n < 0 || lseek(fd, 0, SEEK_SET) != 0 || (status = open_store(&store, 1)) != 0) {
		fprintf(stderr, "%s: tar:%s: it cannot be read again for the store\n", file, given);
		close(fd);
		return -1;
	}
	{
		hull_commit_extra extra = { 0 };
		char              provenance[200];

		snprintf(provenance, sizeof provenance, "(10:provenance(7:builder%zu:%s))",
		         strlen(HULL_BUILDER), HULL_BUILDER);
		extra.provenance = (const uint8_t *)provenance;
		extra.provenance_length = strlen(provenance);
		status = hull_store_commit_tar(&store, fd, arch, ref, NULL, &extra, &made, why,
		                               sizeof why);
	}
	hull_store_close(&store);
	close(fd);
	if (status != 0) {
		fprintf(stderr, "%s: tar:%s: %s\n", file, given, why);
		return -1;
	}
	return 0;
}

/* The pack `given` names, relative to the file `file` where it is not
 * absolute, read into the store and named `pack/` and its manifest hash's
 * first 32 hex digits; that ref into `ref`. 0, or -1 having said why. */
static int pack_ref(const char *file, const char *given, char *ref, size_t cap)
{
	char       path[4400];
	char       hex[65];
	char       why[4400] = "";
	hull_store store;
	int        fd;
	int        status;

	if (given[0] == '/') {
		snprintf(path, sizeof path, "%s", given);
	} else {
		const char *slash = strrchr(file, '/');

		snprintf(path, sizeof path, "%.*s%s", slash != NULL ? (int)(slash - file + 1) : 0, file,
		         given);
	}
	if ((fd = open(path, O_RDONLY | O_CLOEXEC)) < 0) {
		fprintf(stderr, "%s: pack:%s: %s\n", file, given, strerror(errno));
		return -1;
	}
	if (open_store(&store, 1) != 0) {
		close(fd);
		return -1;
	}
	status = hull_store_unpack(&store, fd, NULL, hex, why, sizeof why);
	close(fd);
	if (status == 0) {
		char from[80];

		snprintf(ref, cap, "pack/%.32s", hex);
		snprintf(from, sizeof from, "sha256:%s", hex);
		status = hull_store_tag(&store, from, ref, hex, why, sizeof why);
	}
	hull_store_close(&store);
	if (status != 0) {
		fprintf(stderr, "%s: pack:%s: %s\n", file, given, why);
		return -1;
	}
	return 0;
}

/* Whether a stored source on this machine is mounted as a composefs image
 * rather than checked out (section 3.2): where every backend it may run
 * on is the qemu engine's, vm or emu, whose guest mounts the image as
 * root, and it names nothing a composefs root refuses -- layers, an
 * upper, an export, the image's own kernel. The backends are the
 * command's where it names any, else the machine's own. */
static int composefs_machine(const hull_spec *spec, const hull_sexpr_form *machine,
                             const char *const *backends, unsigned backend_count)
{
	static const char *const refused[] = { "layers", "upper", "export", NULL };
	hull_sexpr_form          entry;
	hull_sexpr_form          head;
	hull_sexpr_form          value;
	const char              *given[1];
	char                     own[32] = "";

	for (unsigned i = 0u; refused[i] != NULL; i++)
		if (hull_spec_find(spec, machine, refused[i], &entry))
			return 0;
	if (hull_spec_find(spec, machine, "tuning", &entry)) {
		hull_sexpr_form vm;
		hull_sexpr_form item;

		hull_sexpr_child(&spec->doc, &entry, &item);
		for (int more = hull_sexpr_next(&spec->doc, &item, &item); more;
		     more = hull_sexpr_next(&spec->doc, &item, &item))
			if (hull_sexpr_child(&spec->doc, &item, &vm) && hull_sexpr_is(&spec->doc, &vm, "vm") &&
			    memmem(spec->doc.data + item.offset, item.extent, "image", 5u) != NULL)
				return 0;
	}
	if (backend_count == 0u) {
		if (!hull_spec_find(spec, machine, "backend", &entry) ||
		    !hull_sexpr_child(&spec->doc, &entry, &head) ||
		    !hull_sexpr_next(&spec->doc, &head, &value) || value.content_length >= sizeof own)
			return 0;
		memcpy(own, spec->doc.data + value.content, value.content_length);
		given[0] = own;
		backends = given;
		backend_count = 1u;
	}
	for (unsigned i = 0u; i < backend_count; i++)
		if (strcmp(backends[i], "vm") != 0 && strcmp(backends[i], "emu") != 0 &&
		    strcmp(backends[i], "qemu") != 0)
			return 0;
	return 1;
}

/* Whether the machine asks `(security (verity true))`. */
static int wants_verity(const hull_spec *spec, const hull_sexpr_form *machine)
{
	hull_sexpr_form security;
	hull_sexpr_form item;
	hull_sexpr_form head;
	hull_sexpr_form value;

	if (!hull_spec_find(spec, machine, "security", &security) ||
	    !hull_sexpr_child(&spec->doc, &security, &item))
		return 0;
	for (int more = hull_sexpr_next(&spec->doc, &item, &item); more;
	     more = hull_sexpr_next(&spec->doc, &item, &item))
		if (hull_sexpr_child(&spec->doc, &item, &head) && hull_sexpr_is(&spec->doc, &head, "verity") &&
		    hull_sexpr_next(&spec->doc, &head, &value) && hull_sexpr_is(&spec->doc, &value, "true"))
			return 1;
	return 0;
}

/* A verity machine's objects disk (section 3.2): the objects of its tree
 * as an ext4 filesystem with the verity feature, beside its image as
 * HEX.objects.ext4, where the guest enables fs-verity on each and mounts
 * them as the overlay's data layer with verity=require -- no filesystem
 * of the host's can hold verity without root. Made once, by mke2fs as
 * the user, from links to the objects in the store's tmp/; sized for the
 * bytes, the Merkle trees the guest adds, about a 127th, and a block or
 * three a file. */
static int verity_disk(hull_store *store, const char *name, const char *image, char *why,
                       size_t cap)
{
	const char *mke2fs = access("/usr/sbin/mke2fs", X_OK) == 0 ? "/usr/sbin/mke2fs" :
	                     access("/sbin/mke2fs", X_OK) == 0 ? "/sbin/mke2fs" : NULL;
	char        disk[4200];
	char        partial[4300];
	char        staging[4200];
	char        size[32];
	char        inodes[32];
	uint64_t    count;
	uint64_t    bytes;
	size_t      stem = strlen(image);
	const char *mounts_slash;
	int         status;

	if (stem < 8u || strcmp(image + stem - 8u, ".2.erofs") != 0 ||
	    (mounts_slash = strrchr(image, '/')) == NULL || mounts_slash - image < 7)
		return snprintf(why, cap, "the source is not a composefs image of the store's"), -1;
	snprintf(disk, sizeof disk, "%.*s.objects.ext4", (int)(stem - 8u), image);
	if (access(disk, R_OK) == 0)
		return 0;
	if (mke2fs == NULL)
		return snprintf(why, cap, "(security (verity true)) puts the tree's objects on an "
		                "ext4 disk, made by mke2fs, which is not installed (e2fsprogs)"), -1;
	/* tmp/ beside mounts/, which gc empties of what a run left. */
	snprintf(staging, sizeof staging, "%.*s/tmp/verity-%ld", (int)(mounts_slash - image - 7),
	         image, (long)getpid());
	if (hull_store_composefs_objects(store, name, staging, &count, &bytes, why, cap) != 0)
		return -1;
	snprintf(size, sizeof size, "%lluk",
	         (unsigned long long)((bytes + bytes / 64u + count * 3u * 4096u) / 1024u + 16384u));
	snprintf(inodes, sizeof inodes, "%llu", (unsigned long long)(count + 512u));
	snprintf(partial, sizeof partial, "%s.%ld", disk, (long)getpid());
	{
		const char *const argv[] = { "env", "E2FSPROGS_FAKE_TIME=1", mke2fs, "-q", "-F", "-t",
		                             "ext4", "-b", "4096", "-O", "verity,^has_journal", "-N",
		                             inodes, "-d", staging, partial, size, NULL };

		status = hull_hostnet_run(argv, why, cap);
	}
	{
		const char *leaf = strrchr(staging, '/');
		char        tmp[4200];
		int         fd;

		snprintf(tmp, sizeof tmp, "%.*s", (int)(leaf - staging), staging);
		if ((fd = open(tmp, O_RDONLY | O_DIRECTORY | O_CLOEXEC)) >= 0) {
			hull_store_remove_tree(fd, leaf + 1);
			close(fd);
		}
	}
	if (status != 0 || rename(partial, disk) != 0) {
		if (status == 0)
			snprintf(why, cap, "%s: %s", disk, strerror(errno));
		unlink(partial);
		return -1;
	}
	return 0;
}

/* A medium's tar: or pack: collection (J.4), one file a disc can travel
 * as: committed to the store as a machine's would be, and given to the
 * engine as the dir: of its checkout, so every engine that takes a
 * collection takes these. 0, or -1 having said why. */
static int media_sources(const char *file, const hull_spec *spec, const hull_sexpr_form *machine,
                         int create, rewrite *rewrites, unsigned *count)
{
	hull_sexpr_form media;
	hull_sexpr_form item;
	int             more;

	if (!hull_spec_find(spec, machine, "media", &media))
		return 0;
	hull_sexpr_child(&spec->doc, &media, &item);
	for (more = hull_sexpr_next(&spec->doc, &item, &item); more;
	     more = hull_sexpr_next(&spec->doc, &item, &item)) {
		hull_sexpr_form entry;
		hull_sexpr_form head;
		hull_sexpr_form value;
		hull_store      store;
		text_buf        text = { 0 };
		char            given[300];
		char            ref[300];
		char            path[4400];
		char            why[4400] = "";
		uint64_t        skipped = 0u;
		int             failed;

		if (!hull_spec_find(spec, &item, "source", &entry) ||
		    !hull_sexpr_child(&spec->doc, &entry, &head) ||
		    !hull_sexpr_next(&spec->doc, &head, &value) || value.content_length >= sizeof given)
			continue;
		memcpy(given, spec->doc.data + value.content, value.content_length);
		given[value.content_length] = '\0';
		if (strncmp(given, "tar:", 4u) == 0) {
			if (tar_ref(file, spec, machine, given + 4, ref, sizeof ref) != 0)
				return -1;
		} else if (strncmp(given, "pack:", 5u) == 0) {
			if (pack_ref(file, given + 5, ref, sizeof ref) != 0)
				return -1;
		} else {
			continue;
		}
		if (hull_store_open(&store, 0, why, sizeof why) != 0) {
			fprintf(stderr, "%s: %s: the store: %s\n", file, given, why);
			return -1;
		}
		failed = hull_store_checkout(&store, ref, create, path, sizeof path, &skipped, why,
		                             sizeof why);
		hull_store_close(&store);
		if (failed != 0 || strpbrk(path, "\"\\\n") != NULL) {
			fprintf(stderr, "%s: %s: %s\n", file, given,
			        why[0] != '\0' ? why : "its checkout cannot be named in a source");
			return -1;
		}
		add_text(&text, "dir:");
		add_text(&text, path);
		if (add_rewrite(rewrites, count, value.content, value.content_length, &text) != 0) {
			fprintf(stderr, "%s: too many sources to name\n", file);
			return -1;
		}
	}
	return 0;
}

static int store_sources(const char *file, uint8_t *data, size_t *length, int create,
                         const char *const *backends, unsigned backend_count)
{
	static rewrite  rewrites[MAX_REWRITES];
	static hull_sexpr_form machines[HULL_NAMES_MEMBERS];
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
		for (more = hull_sexpr_next(&spec.doc, &entry, &entry); more && machine_count < HULL_NAMES_MEMBERS;
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
		uint8_t                 *runtime = NULL;
		size_t                   runtime_length = 0u;
		text_buf                 source = { 0 };
		int                      has_mutable;
		int                      cfs;
		int                      schemed = 0;

		if (media_sources(file, &spec, &machines[i], create, rewrites, &count) != 0) {
			result = -1;
			break;
		}
		if (!hull_spec_find(&spec, &machines[i], "source", &entry) ||
		    !hull_sexpr_child(&spec.doc, &entry, &head) ||
		    !hull_sexpr_next(&spec.doc, &head, &value) || value.content_length >= sizeof name)
			continue;
		memcpy(name, spec.doc.data + value.content, value.content_length);
		name[value.content_length] = '\0';
		/* A tar: source, a tree as a tarball -- an export, a docker
		 * export, a release -- committed to the store as `tar/HASH`, its
		 * content's, owners and all, and run as the stored tree it then
		 * is, on every engine. Committed on its first plan or run, a
		 * commit of the same bytes again being the same ref. */
		if (strncmp(name, "tar:", 4u) == 0 && name[4] != '\0') {
			if (tar_ref(file, &spec, &machines[i], name + 4, name, sizeof name) != 0) {
				result = -1;
				break;
			}
		}
		/* A pack: source (section 3.13), read into the store as
		 * pack/HASH, its manifest's, each object checked, and run as the
		 * stored tree it then is, as a tar: source is. */
		if (strncmp(name, "pack:", 5u) == 0 && name[5] != '\0') {
			if (pack_ref(file, name + 5, name, sizeof name) != 0) {
				result = -1;
				break;
			}
		}
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
		cfs = composefs_machine(&spec, &machines[i], backends, backend_count);
		if (cfs ? hull_store_composefs_image(&store, name, create, path, sizeof path, why,
		                                     sizeof why) != 0 ||
		          (create && wants_verity(&spec, &machines[i]) &&
		           verity_disk(&store, name, path, why, sizeof why) != 0) :
		          create && hull_store_checkout(&store, name, 1, path, sizeof path, &skipped, why,
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
		add_text(&source, cfs ? "\"composefs:" : "\"dir:");
		add_text(&source, path);
		add_text(&source, "\")");
		if (!has_mutable)
			add_text(&source, " (mutable overlay)");
		/* A tree of a foreign arch is run as one: the machine takes its
		 * manifest's arch where it names none, and naming another is
		 * refused, a tree's programs being of the arch they were built
		 * for whatever the machine says. */
		{
			char            tree_arch[64];
			char            asked[64] = "";
			hull_sexpr_form arch_entry;
			hull_sexpr_form arch_head;
			hull_sexpr_form arch_value;

			if (hull_store_arch(&store, name, tree_arch, why, sizeof why) != 0) {
				fprintf(stderr, "%s: %s: %s\n", file, name, why);
				result = -1;
				break;
			}
			if (strspn(tree_arch, "abcdefghijklmnopqrstuvwxyz0123456789_") != strlen(tree_arch)) {
				fprintf(stderr, "%s: %s: its manifest's arch is not a name\n", file, name);
				result = -1;
				break;
			}
			if (hull_spec_find(&spec, &machines[i], "arch", &arch_entry) &&
			    hull_sexpr_child(&spec.doc, &arch_entry, &arch_head) &&
			    hull_sexpr_next(&spec.doc, &arch_head, &arch_value) &&
			    arch_value.content_length < sizeof asked) {
				memcpy(asked, spec.doc.data + arch_value.content, arch_value.content_length);
				asked[arch_value.content_length] = '\0';
				if (strcmp(asked, "host") == 0)
					snprintf(asked, sizeof asked, "%s", host_arch());
			}
			if (strcmp(tree_arch, "none") != 0 && asked[0] != '\0' &&
			    strcmp(asked, tree_arch) != 0) {
				fprintf(stderr, "%s: %s is a tree of %s, and the machine says (arch %s)\n",
				        file, name, tree_arch, asked);
				result = -1;
				break;
			}
			if (strcmp(tree_arch, "none") != 0 && asked[0] == '\0' &&
			    strcmp(tree_arch, host_arch()) != 0) {
				add_text(&source, " (arch ");
				add_text(&source, tree_arch);
				add_text(&source, ")");
			}
		}
		/* A stored image's non-root owners, which the checkout flattened to
		 * the user's, given back when the machine starts: its manifest
		 * holds them, hull_store_owners lists each as uid, gid and path, and
		 * the engine gives them in the overlay -- chroot's namespace one,
		 * qemu's host-side, docker's import's tar since 2026-10-31. A plan,
		 * which makes no checkout, names none. The list is a sibling of the
		 * checkout, written only on a real run and only where the manifest
		 * gives an explicit owner. */
		/* A composefs image holds its owners itself. */
		if (create && !cfs) {
			{
				char owners_file[4500];
				int  list;

				snprintf(owners_file, sizeof owners_file, "%s.owners", path);
				list = open(owners_file, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
				if (list >= 0) {
					uint64_t owned = 0u;
					int      ok    = hull_store_owners(&store, name, list, &owned, why,
					                                   sizeof why) == 0;

					close(list);
					if (ok && owned > 0u) {
						add_text(&source, " (owners \"");
						add_text(&source, owners_file);
						add_text(&source, "\")");
					} else {
						unlink(owners_file);
					}
				}
			}
		}
		if (add_rewrite(rewrites, &count, value.offset, entry.offset + entry.extent - value.offset,
		                &source) != 0) {
			fprintf(stderr, "%s: too many machines on stored trees\n", file);
			result = -1;
			break;
		}
		if (hull_store_runtime(&store, name, &runtime, &runtime_length, why, sizeof why) != 0) {
			fprintf(stderr, "%s: %s: %s\n", file, name, why);
			result = -1;
			break;
		}
		if (runtime != NULL &&
		    runtime_defaults(file, name, &spec, &machines[i], runtime, runtime_length, rewrites,
		                     &count) != 0)
			result = -1;
		free(runtime);
	}
	if (opened == 0)
		hull_store_close(&store);
	/* Last first, so that each rewrite's offset is still the file's. */
	qsort(rewrites, count, sizeof rewrites[0], rewrite_order);
	for (unsigned i = 0u; i < count; i++) {
		size_t added = strlen(rewrites[i].text);
		size_t tail  = *length - rewrites[i].at - rewrites[i].cut;

		if (result == 0 && *length - rewrites[i].cut + added > MAX_FILE) {
			fprintf(stderr, "%s: larger than %u bytes once its stored sources are named\n",
			        file, MAX_FILE);
			result = -1;
		}
		if (result == 0) {
			memmove(data + rewrites[i].at + added, data + rewrites[i].at + rewrites[i].cut, tail);
			memcpy(data + rewrites[i].at, rewrites[i].text, added);
			*length = *length - rewrites[i].cut + added;
		}
		free(rewrites[i].text);
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
	memset(&r, 0, sizeof r);
	r.options  = o;
	r.spec     = spec;
	r.machine  = shown;
	r.choice   = &choice;
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
	/* The hash a pipeline accepts, printed as the token it passes. */
	if (o->root_hash) {
		char hex[72];

		if (hull_matrix_root_hash(&plan.resolved, &plan.resolved.machine,
		                          choice.engine->caps->engine_name, choice.class_name, hex) != 0) {
			fprintf(stderr, "hull: out of memory\n");
			return 2;
		}
		printf("%s: %s on %s (%s): --accept root:%s\n", o->file, shown,
		       choice.engine->caps->engine_name, choice.class_name, hex);
	}
	{
		const char *field = NULL;
		const char *rootless = rootless_honours(registry, &plan, &field);

		if (rootless != NULL) {
			printf("%s: %s on %s (%s): root mode is not offered for %s: %s honours it "
			       "without root\n", o->file, shown, choice.engine->caps->engine_name,
			       choice.class_name, field, rootless);
			r.rejected++;
			plan.root_count = 0u;
		}
	}
	if (r.rejected != 0u)
		printf("%s: %s on %s (%s): rejected, %u field%s\n", o->file, shown,
		       choice.engine->caps->engine_name, choice.class_name, r.rejected,
		       r.rejected == 1u ? "" : "s");
	else
		printf("%s: %s on %s (%s): ok%s\n", o->file, shown, choice.engine->caps->engine_name,
		       choice.class_name, plan.root_count != 0u ? ", ROOT" : "");
	if (root_changed(&r))
		return 77;
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

	data = spec_text(o->file, &length);
	if (data == NULL)
		return 2;
	if (store_sources(o->file, data, &length, 0, o->backends, o->backend_count) != 0) {
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

/* Why no runtime directory was chosen, where XDG_RUNTIME_DIR named none. */
static char runtime_why[512];

/* $XDG_RUNTIME_DIR/hull/<leaf>, or 0 if the runtime directory is unset. */
static int runtime_path(char *out, size_t cap, const char *leaf)
{
	const char *runtime = getenv("XDG_RUNTIME_DIR");

	if (runtime == NULL || runtime[0] != '/')
		return 0;
	return (size_t)snprintf(out, cap, "%s/hull/%s", runtime, leaf) < cap;
}

/* `-H` or HULL_HOST: the core this frontend speaks to is that host's. */
static hull_dial_target remote;
static int         remote_given;

static int dial_local(void);

/* The local core, or with -H the far one through ssh. */
static int dial_core(void)
{
	return remote_given ? hull_dial_host(&remote) : dial_local();
}

static int dial_local(void)
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
 * once, so it is never this process's zombie. Its output goes to core.log
 * beside the socket, which the core opens, truncated, once it holds the
 * lock: opened here, a core that then gave way truncated the live core's
 * log, and what that core had said was gone. */
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
			int null = open("/dev/null", O_RDWR);

			setenv("HULL_CORE_LOG", log, 1);
			setsid();
			if (null >= 0) {
				dup2(null, 0);
				dup2(null, 1);
				dup2(null, 2);
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
	char           exe[64];
	int            given_way = 0;
	int            plain = 0;   /* the hello of a core from before builds were said */
	hull_msg       hello;
	hull_sexpr_doc body;
	int            fd;

	hull_wire_exe(exe, sizeof exe);
again:
	memset(&hello, 0, sizeof hello);
	fd = dial_core();

	start = start || (!remote_given && kept_machines());
	if (fd < 0 && start && !remote_given && getenv("XDG_RUNTIME_DIR") != NULL) {
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
			fprintf(stderr, "hull: XDG_RUNTIME_DIR is not set, and no runtime directory would "
			        "do (%s), so there is no core\n", runtime_why);
		else if (start)
			fprintf(stderr, "hull: the core did not start; see $XDG_RUNTIME_DIR/hull/core.log\n");
		return -1;
	}
	if (hull_wire_open(wire, fd, HULL_WIRE_FROM_CORE) != 0) {
		close(fd);
		return -1;
	}
	if (plain) {
		hull_msg_raw(&hello, "(5:hello(2:ch1:0)(5:proto1:1))");
	} else {
		hull_msg_raw(&hello, "(5:hello(2:ch1:0)(6:commit");
		hull_msg_text(&hello, HULL_COMMIT);
		hull_msg_raw(&hello, ")(3:exe");
		hull_msg_text(&hello, exe);
		hull_msg_raw(&hello, ")(5:proto1:1))");
	}
	if (hull_wire_send(fd, &hello) != 0 || hull_wire_read(wire, &body) != HULL_WIRE_OK ||
	    !hull_msg_is(&body, "hello")) {
		/* A core from before 2026-10-31 refuses the build's entries, as
		 * its description has none: said hello to again as it was, and
		 * warned of below. */
		if (!plain && !remote_given) {
			hull_msg_free(&hello);
			hull_wire_close(wire);
			plain = 1;
			goto again;
		}
		fprintf(stderr, "hull: the core did not answer hello\n");
		hull_msg_free(&hello);
		hull_wire_close(wire);
		return -1;
	}
	hull_msg_free(&hello);
	/* A core left running from an older build serves its old engine, where
	 * this frontend plans against the new one, so a feature this build adds
	 * is planned and then silently does nothing -- which cost a day's
	 * misdiagnosis once. The core says which build it is, and where it is
	 * another and idle, holding no machine, it goes and says so: then this
	 * starts its own build's core, once. Where it holds machines it stays,
	 * and this warns, as its machines would go with it. */
	{
		hull_sexpr_form top;
		char           *core_commit = NULL;
		char           *core_exe = NULL;
		char           *replaced = NULL;

		if (hull_sexpr_first(&body, &top) &&
		    (core_commit = hull_access_value(&body, &top, "core_version")) != NULL) {
			core_exe = hull_access_value(&body, &top, "exe");
			replaced = hull_access_value(&body, &top, "replaced");
			if (replaced != NULL && strcmp(replaced, "true") == 0 && !given_way) {
				free(core_commit);
				free(core_exe);
				free(replaced);
				hull_wire_close(wire);
				given_way = 1;
				start = 1;
				/* Gone once its socket is: it unlinks that first. */
				for (int tries = 0; tries < 100; tries++) {
					struct timespec pause = { 0, 20000000L };
					int             old = dial_core();

					if (old < 0)
						break;
					close(old);
					nanosleep(&pause, NULL);
				}
				goto again;
			}
			if (!hull_wire_same_build(core_commit, core_exe != NULL ? core_exe : "", HULL_COMMIT,
			                          exe))
				fprintf(stderr,
				        "hull: warning: the running core is another build than this: %s%s%s; a "
				        "machine planned here runs on the core's engine, so a newer feature may "
				        "do nothing until the core is restarted -- it holds machines, or is from "
				        "before a core gave way, so it stays; it exits on its own once they are "
				        "gone and it has idled, or end its `hull core` process, and the next "
				        "command starts this build\n",
				        core_commit,
				        strcmp(core_commit, HULL_COMMIT) == 0 ? ", built again since, as " : " against ",
				        HULL_COMMIT);
		}
		free(core_commit);
		free(core_exe);
		free(replaced);
	}
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
			send_request(wire, "(4:stop", ch + 1u, stop_id, "(10:timeout_ms2:-1)");
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
			return refused_status(reason);
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

/* Appendix D's names, drawn over the whole file, which a run of one of
 * its machines needs as much as a run of all: each sibling on a segment
 * hull addresses is named to it, whether or not this run starts it. 0,
 * else 125 having said why. */
static int draw_names(options *o, const hull_spec *spec, hull_names *names)
{
	static hull_sexpr_form machines[HULL_NAMES_MEMBERS];
	static char            id_text[HULL_NAMES_MEMBERS][64];
	const char            *ids[HULL_NAMES_MEMBERS];
	unsigned               count = 0u;

	if (spec->has_machine) {
		machines[count] = spec->machine;
		if (!machine_id(spec, &machines[count], NULL, o->base, id_text[count], sizeof id_text[0]))
			id_text[count][0] = '\0';
		ids[count] = id_text[count];
		count++;
	} else if (spec->has_machines) {
		hull_sexpr_form entry;

		hull_sexpr_child(&spec->doc, &spec->machines, &entry);
		while (hull_sexpr_next(&spec->doc, &entry, &entry) && count < HULL_NAMES_MEMBERS) {
			hull_sexpr_form key;
			char            name[64];
			size_t          length;

			hull_sexpr_child(&spec->doc, &entry, &key);
			if (hull_sexpr_value(&spec->doc, &key, (uint8_t *)name, sizeof name - 1u,
			                     &length) != HULL_SEXPR_OK)
				continue;
			name[length] = '\0';
			machines[count] = entry;
			if (!machine_id(spec, &entry, name, o->base, id_text[count], sizeof id_text[0]))
				id_text[count][0] = '\0';
			ids[count] = id_text[count];
			count++;
		}
	}
	if (hull_names_draw(names, spec, machines, ids, count) != 0) {
		fprintf(stderr, "%s: %s\n", o->file, names->reason);
		return 125;
	}
	return 0;
}

/* `plan` with the plan's `(names ...)` for machine `id` from `names`
 * added: "names" sorts after "machine" and before what the core adds, so
 * it goes last. 0, or -1 where the plan has no room for it. */
/* `(screen (display D) (xauthority F)?)` at the plan's end, from this
 * process's DISPLAY and XAUTHORITY; nothing where DISPLAY is not set. */
static int add_screen(hull_plan *plan)
{
	const char *display = getenv("DISPLAY");
	const char *authority = getenv("XAUTHORITY");
	char        entry[4600];
	int         n;

	if (display == NULL || display[0] == '\0')
		return 0;
	if (strlen(display) > 200u || (authority != NULL && strlen(authority) > 4096u))
		return -1;
	n = snprintf(entry, sizeof entry, "(6:screen(7:display%zu:%s)", strlen(display), display);
	if (authority != NULL && authority[0] != '\0')
		n += snprintf(entry + n, sizeof entry - (size_t)n, "(10:xauthority%zu:%s)",
		              strlen(authority), authority);
	n += snprintf(entry + n, sizeof entry - (size_t)n, "))");
	if (plan->canonical_length == 0u ||
	    plan->canonical_length - 1u + (size_t)n > sizeof plan->canonical)
		return -1;
	memcpy(plan->canonical + plan->canonical_length - 1u, entry, (size_t)n);
	plan->canonical_length += (uint32_t)(n - 1);
	return 0;
}

/* `(under ID)`, at the plan's end, before any (wait N). */
static int add_under(hull_plan *plan, const char *id)
{
	char   entry[96];
	size_t n;

	if (strlen(id) > 40u)
		return -1;
	n = (size_t)snprintf(entry, sizeof entry, "(5:under%zu:%s))", strlen(id), id);
	if (plan->canonical_length == 0u ||
	    plan->canonical_length - 1u + n > sizeof plan->canonical)
		return -1;
	memcpy(plan->canonical + plan->canonical_length - 1u, entry, n);
	plan->canonical_length += (uint32_t)(n - 1u);
	return 0;
}

/* `(wait N)`, seconds, last in the plan, which no key sorts after. */
static int add_wait(hull_plan *plan, long long seconds)
{
	char   entry[40];
	size_t n;

	snprintf(entry, sizeof entry, "%lld", seconds);
	n = (size_t)snprintf(entry, sizeof entry, "(4:wait%zu:%lld))", strlen(entry), seconds);
	if (plan->canonical_length == 0u ||
	    plan->canonical_length - 1u + n > sizeof plan->canonical)
		return -1;
	memcpy(plan->canonical + plan->canonical_length - 1u, entry, n);
	plan->canonical_length += (uint32_t)(n - 1u);
	return 0;
}

static int add_names(hull_plan *plan, const hull_names *names, const char *id)
{
	unsigned member;
	long     n;

	for (member = 0u; member < HULL_NAMES_MEMBERS && strcmp(names->ids[member], id) != 0; member++)
		;
	if (member == HULL_NAMES_MEMBERS || plan->canonical_length == 0u)
		return 0;
	n = hull_names_entry(names, member, plan->canonical + plan->canonical_length - 1u,
	                     sizeof plan->canonical - plan->canonical_length);
	if (n < 0)
		return -1;
	if (n > 0) {
		plan->canonical_length += (uint32_t)n;
		plan->canonical[plan->canonical_length - 1u] = ')';
	}
	return 0;
}

/* Whether `machine` names `segment` in its (segments ...). */
static int on_segment(const hull_sexpr_doc *doc, const hull_sexpr_form *machine,
                      const char *segment)
{
	hull_sexpr_form segments;
	hull_sexpr_form item;
	int             on = 0;

	if (!hull_access_find(doc, machine, "segments", &segments))
		return 0;
	hull_sexpr_child(doc, &segments, &item);
	while (!on && hull_sexpr_next(doc, &item, &item)) {
		char *name = hull_access_segment_name(doc, &item);

		on = name != NULL && strcmp(name, segment) == 0;
		free(name);
	}
	return on;
}

/* A notice on stderr for each link the spec impairs or tampers with, as
 * for what a plan emulates: the core applies it from the machine's first
 * frame, which nothing in the machine's own output shows, and only
 * `hull segment` counts what it does (Appendix D). */
static void link_notices(const options *o, const char *shown, const uint8_t *plan, size_t length)
{
	static const char *const which[2] = { "impair", "tamper" };
	static const char *const done[2]  = { "impaired", "tampered with" };
	hull_sexpr_doc           doc;
	hull_sexpr_form          machine;

	if (!hull_access_machine(&doc, plan, length, &machine))
		return;
	for (int kind = 0; kind < 2; kind++) {
		hull_sexpr_form group;
		hull_sexpr_form link;

		if (!hull_access_find(&doc, &machine, which[kind], &group))
			continue;
		hull_sexpr_child(&doc, &group, &link);
		while (hull_sexpr_next(&doc, &link, &link)) {
			hull_sexpr_form name;
			hull_sexpr_form way;
			char            segment[64];
			size_t          size;
			int             out = 0;
			int             in  = 0;

			if (link.kind != HULL_SEXPR_LIST || !hull_sexpr_child(&doc, &link, &name) ||
			    hull_sexpr_value(&doc, &name, (uint8_t *)segment, sizeof segment - 1u,
			                     &size) != HULL_SEXPR_OK)
				continue;
			segment[size] = '\0';
			for (way = name; hull_sexpr_next(&doc, &way, &way);) {
				hull_sexpr_form head;

				if (way.kind == HULL_SEXPR_LIST && hull_sexpr_child(&doc, &way, &head)) {
					in  |= hull_sexpr_is(&doc, &head, "in");
					out |= hull_sexpr_is(&doc, &head, "out");
				}
			}
			/* A link of a segment the machine is not on, the core refuses. */
			if ((in || out) && on_segment(&doc, &machine, segment))
				fprintf(stderr,
				        "%s: %s: segment %s %s %s from the first frame; hull segment %s "
				        "counts it\n",
				        o->file, shown, segment, done[kind],
				        in && out ? "both ways" : out ? "on the way out" : "on the way in",
				        segment);
		}
	}
}

/* One machine planned, and its run's request built on channel `ch`: the
 * plan, the names of its addressed segments, and the secrets it asks
 * for, which may prompt. 0, else 125 having said why; nothing is started. */
static int prepare_run(options *o, const hull_registry *registry, hull_spec *spec,
                       const hull_sexpr_form *machine, const char *name,
                       const hull_names *names, uint32_t ch, hull_msg *request, char *id,
                       size_t id_cap)
{
	static hull_plan plan;
	hull_choice      choice;
	const char      *reason = NULL;
	const char      *shown  = name != NULL ? name : "machine";
	report           r;
	hull_plan_status planned;

	if (!machine_id(spec, machine, name, o->base, id, id_cap)) {
		fprintf(stderr, "%s: name the machine with (hostname ...)\n", o->file);
		return 125;
	}
	if (hull_registry_choose(registry, spec, machine,
	                         o->backend_count != 0u ? o->backends[0] : NULL, &choice,
	                         &reason) != HULL_REGISTRY_OK) {
		fprintf(stderr, "%s: %s: %s\n", o->file, shown, reason);
		return 125;
	}
	memset(&r, 0, sizeof r);
	r.options  = o;
	r.spec     = spec;
	r.machine  = shown;
	r.choice   = &choice;
	planned = hull_plan_build(&plan, spec, machine, name, o->base, &choice,
	                          registry->host_arch, o->tokens, print_resolution, &r);
	if (planned == HULL_PLAN_REJECTED && root_changed(&r))
		return 77;
	if (planned == HULL_PLAN_OK) {
		const char *field = NULL;
		const char *rootless = rootless_honours(registry, &plan, &field);

		if (rootless != NULL) {
			fprintf(stderr, "%s: %s on %s (%s): not run: root mode is not offered for %s: %s "
			        "honours it without root\n", o->file, shown,
			        choice.engine->caps->engine_name, choice.class_name, field, rootless);
			return 125;
		}
	}
	if (planned != HULL_PLAN_OK) {
		fprintf(stderr, "%s: %s on %s (%s): not run: %s\n", o->file, shown,
		        choice.engine->caps->engine_name, choice.class_name,
		        planned == HULL_PLAN_REJECTED ? "the plan is rejected" : "it could not be planned");
		return 125;
	}
	{
		const hull_engine_ops *ops = choice.engine->ops;

		if (ops->create == NULL || ops->start == NULL || ops->stop == NULL ||
		    ops->destroy == NULL) {
			fprintf(stderr, "%s: %s: the %s engine cannot run a machine yet\n", o->file,
			        shown, choice.engine->caps->engine_name);
			return 125;
		}
	}

	if (add_names(&plan, names, id) != 0) {
		fprintf(stderr, "%s: %s: not run: its segments' names do not fit in its plan\n", o->file,
		        shown);
		return 125;
	}
	/* (display window): this run's own X display, and its authority,
	 * as `(screen ...)`, which sorts after root and before under, the
	 * core's being whoever started it. None where DISPLAY is not set,
	 * which the engine then says. */
	if (memmem(plan.canonical, plan.canonical_length, "(7:display6:window)", 19u) != NULL &&
	    add_screen(&plan) != 0) {
		fprintf(stderr, "%s: %s: not run: its display does not fit in its plan\n", o->file,
		        shown);
		return 125;
	}
	/* A slot of a lease's, `(under ID)`, which sorts after root and
	 * before wait (section 4.12). */
	if (o->lease != NULL && add_under(&plan, o->lease) != 0) {
		fprintf(stderr, "%s: %s: not run: its lease does not fit in its plan\n", o->file, shown);
		return 125;
	}
	/* A slot of its pool waited for so long, or not at all, where the
	 * core would otherwise wait for one for ever (section 4.12): last,
	 * as no key sorts after it. */
	if (o->wait >= 0 && add_wait(&plan, o->wait) != 0) {
		fprintf(stderr, "%s: %s: not run: its wait does not fit in its plan\n", o->file, shown);
		return 125;
	}
	link_notices(o, shown, plan.canonical, plan.canonical_length);

	/* Before the core is reached, and before SIGINT would stop a machine:
	 * a command asked for a secret may prompt, and ^C there ends this. */
	hull_msg_raw(request, "(3:run");
	hull_msg_channel(request, ch);
	hull_msg_raw(request, "(7:machine");
	hull_msg_text(request, id);
	hull_msg_raw(request, ")");
	hull_msg_bytes(request, plan.canonical, plan.canonical_length);
	if (add_secrets(request, plan.canonical, plan.canonical_length) != 0) {
		fprintf(stderr, "%s: %s: not run: a secret could not be had\n", o->file, shown);
		return 125;
	}
	hull_msg_raw(request, ")");
	return 0;
}

/* --- run, a whole file --------------------------------------------------- */

#define LINE_CAP 4096u

/* One machine of a whole file's run: its channel is 1 + 2 * its place,
 * its stop's the one after, and its readiness is asked on PROBE_CHANNEL +
 * 2 * its place. */
typedef struct member {
	char      id[64];
	char      name[64];
	int       ended;
	int       status;
	int       sent;          /* its run asked of the core */
	int       up;            /* started, and ready where it says how */
	int       ready;         /* it says how, with (ready ...) */
	int       probing;       /* its (ready ...) is being asked */
	unsigned  port;          /* (ready (port N)): a dial, not a command */
	long long interval_ms;
	long long timeout_ms;
	long long next_ms;       /* when it is asked next, once started */
	long long until_ms;      /* when it is given up on, once started */
	char      said[160];     /* what the command running now said, its end */
	char      last[256];     /* why it was not ready, the last time asked */
	size_t    held[2];       /* a line not yet ended, per stream */
	uint8_t   line[2][LINE_CAP];
} member;

#define PROBE_CHANNEL (1u + 2u * HULL_NAMES_MEMBERS)

/* What a member wrote, a line at a time, each behind its name: so that
 * two machines writing at once do not split each other's lines. A line
 * longer than LINE_CAP goes in pieces, each prefixed. */
static void member_write(member *m, int width, int stream, const uint8_t *data, size_t n,
                         int flush)
{
	FILE *to = stream == 2 ? stderr : stdout;

	for (size_t i = 0u; i <= n; i++) {
		int  end_of_data = i == n;
		int  newline     = !end_of_data && data[i] == '\n';

		if (!end_of_data && !newline)
			m->line[stream - 1][m->held[stream - 1]++] = data[i];
		if (newline || m->held[stream - 1] == LINE_CAP ||
		    (end_of_data && flush && m->held[stream - 1] != 0u)) {
			fprintf(to, "%-*s | %.*s\n", width, m->name, (int)m->held[stream - 1],
			        (const char *)m->line[stream - 1]);
			m->held[stream - 1] = 0u;
		}
	}
	fflush(to);
}

static long long seconds_of(const char *text);

static long long monotonic_ms(void)
{
	struct timespec now;

	clock_gettime(CLOCK_MONOTONIC, &now);
	return (long long)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

/* A machine's (ready ...), read before the file's text is let go: the
 * exec its command is, or the dial its port is, sent each time it is
 * asked, and how often and for how long. 1s and 5m where it does not say:
 * a machine is asked again soon, and one that boots a whole system has
 * time to. */
static int ready_of(const hull_spec *spec, const hull_sexpr_form *entry, member *m,
                    unsigned place, hull_msg *probe)
{
	hull_sexpr_form ready;
	hull_sexpr_form command;
	hull_sexpr_form word;
	hull_sexpr_form found;
	char            text[4096];
	size_t          length;

	m->interval_ms = 1000;
	m->timeout_ms  = 300000;
	if (!hull_spec_find(spec, entry, "ready", &ready))
		return 0;
	m->ready = 1;
	if (hull_spec_find(spec, &ready, "interval", &found) &&
	    hull_sexpr_child(&spec->doc, &found, &word) && hull_sexpr_next(&spec->doc, &word, &word) &&
	    hull_sexpr_value(&spec->doc, &word, (uint8_t *)text, sizeof text - 1u, &length) ==
	    HULL_SEXPR_OK) {
		text[length] = '\0';
		m->interval_ms = seconds_of(text) * 1000;
	}
	if (hull_spec_find(spec, &ready, "timeout", &found) &&
	    hull_sexpr_child(&spec->doc, &found, &word) && hull_sexpr_next(&spec->doc, &word, &word) &&
	    hull_sexpr_value(&spec->doc, &word, (uint8_t *)text, sizeof text - 1u, &length) ==
	    HULL_SEXPR_OK) {
		text[length] = '\0';
		m->timeout_ms = seconds_of(text) * 1000;
	}
	if (hull_spec_find(spec, &ready, "port", &found) &&
	    hull_sexpr_child(&spec->doc, &found, &word) && hull_sexpr_next(&spec->doc, &word, &word) &&
	    hull_sexpr_value(&spec->doc, &word, (uint8_t *)text, sizeof text - 1u, &length) ==
	    HULL_SEXPR_OK) {
		text[length] = '\0';
		m->port = (unsigned)strtoul(text, NULL, 10);
		hull_msg_raw(probe, "(4:dial");
		hull_msg_channel(probe, PROBE_CHANNEL + 2u * place);
		hull_msg_raw(probe, "(7:machine");
		hull_msg_text(probe, m->id);
		hull_msg_raw(probe, ")(4:port");
		hull_msg_number(probe, (long long)m->port);
		hull_msg_raw(probe, "))");
		return probe->failed ? -1 : 0;
	}
	hull_spec_find(spec, &ready, "command", &command);
	hull_msg_raw(probe, "(4:exec(4:argv");
	hull_sexpr_child(&spec->doc, &command, &word);
	while (hull_sexpr_next(&spec->doc, &word, &word)) {
		if (hull_sexpr_value(&spec->doc, &word, (uint8_t *)text, sizeof text - 1u, &length) !=
		    HULL_SEXPR_OK) {
			fprintf(stderr, "hull: %s: a word of its ready command is too long\n", m->name);
			return -1;
		}
		hull_msg_atom(probe, (const uint8_t *)text, length);
	}
	hull_msg_raw(probe, ")");
	hull_msg_channel(probe, PROBE_CHANNEL + 2u * place);
	hull_msg_raw(probe, "(7:machine");
	hull_msg_text(probe, m->id);
	hull_msg_raw(probe, ")(3:tty5:false))");
	return probe->failed ? -1 : 0;
}

/* Each machine not yet started whose (after ...) are all up, started;
 * -1 when a run could not be sent. */
static int start_ready(hull_wire *wire, member *members, hull_msg *requests, unsigned count,
                       uint8_t (*waits)[HULL_NAMES_MEMBERS / 8u])
{
	for (unsigned i = 0u; i < count; i++) {
		int held = 0;

		if (members[i].sent || members[i].ended)
			continue;
		for (unsigned j = 0u; j < count && !held; j++)
			held = (waits[i][j / 8u] >> (j % 8u) & 1u) && !members[j].up;
		if (held)
			continue;
		members[i].sent = 1;
		if (requests[i].failed || hull_wire_send(wire->fd, &requests[i]) != 0) {
			fprintf(stderr, "hull: the run of %s could not be sent\n", members[i].name);
			return -1;
		}
		hull_msg_free(&requests[i]);
	}
	return 0;
}

/* A machine is up: what waited on it alone may start. */
static int now_up(hull_wire *wire, member *members, hull_msg *requests, unsigned count,
                  uint8_t (*waits)[HULL_NAMES_MEMBERS / 8u], member *m)
{
	m->up = 1;
	return start_ready(wire, members, requests, count, waits);
}

/* What a command asking a machine's readiness said, its last bytes, so
 * that a machine not ready can say why. */
static void probe_said(member *m, const uint8_t *data, size_t n)
{
	size_t have = strlen(m->said);
	size_t room = sizeof m->said - 1u;

	if (n >= room) {
		data += n - room;
		n = room;
		have = 0u;
	} else if (have + n > room) {
		memmove(m->said, m->said + (have + n - room), room - n);
		have = room - n;
	}
	memcpy(m->said + have, data, n);
	m->said[have + n] = '\0';
}

/* Every machine of a (machines ...) file: each planned before any starts,
 * so one the file cannot run starts none; all run on one connection, a
 * channel each, their output a line at a time behind their names; and the
 * first to fail, or SIGINT or SIGTERM, stops the rest. A machine starts
 * when those it is (after ...) are up: started, and where one says
 * (ready ...), its command exited 0 in it. The run ends when every machine
 * has: 0 when each ended 0, else the status of the first that did not. */
static int run_all(options *o, const hull_registry *registry, hull_spec *spec, uint8_t *data)
{
	static hull_names names_drawn;
	const hull_names *names = &names_drawn;
	static member   members[HULL_NAMES_MEMBERS];
	static hull_msg requests[HULL_NAMES_MEMBERS];
	static hull_msg probes[HULL_NAMES_MEMBERS];
	static hull_sexpr_form forms[HULL_NAMES_MEMBERS];
	static uint8_t  waits[HULL_NAMES_MEMBERS][HULL_NAMES_MEMBERS / 8u];
	hull_sexpr_form entry;
	hull_wire       wire;
	struct sigaction action;
	unsigned        count = 0u;
	unsigned        left;
	int             width = 0;
	int             status = 0;
	int             stopping = 0;

	memset(requests, 0, sizeof requests);
	memset(probes, 0, sizeof probes);
	memset(members, 0, sizeof members);
	memset(waits, 0, sizeof waits);
	if (draw_names(o, spec, &names_drawn) != 0) {
		free(data);
		return 125;
	}
	hull_sexpr_child(&spec->doc, &spec->machines, &entry);
	while (hull_sexpr_next(&spec->doc, &entry, &entry)) {
		hull_sexpr_form key;
		size_t          length;
		member         *m;

		if (count == sizeof members / sizeof members[0]) {
			fprintf(stderr, "%s: a run takes at most %u machines\n", o->file, count);
			status = 125;
			break;
		}
		m = &members[count];
		hull_sexpr_child(&spec->doc, &entry, &key);
		if (hull_sexpr_value(&spec->doc, &key, (uint8_t *)m->name, sizeof m->name - 1u,
		                     &length) != HULL_SEXPR_OK) {
			status = 125;
			break;
		}
		m->name[length] = '\0';
		if (prepare_run(o, registry, spec, &entry, m->name, names, 1u + 2u * count,
		                &requests[count], m->id, sizeof m->id) != 0 ||
		    ready_of(spec, &entry, m, count, &probes[count]) != 0) {
			count++;
			status = 125;
			break;
		}
		forms[count] = entry;
		if ((int)length > width)
			width = (int)length;
		count++;
	}
	/* (after ...) as places, the spec check having held each to a
	 * sibling and the whole to an order. */
	for (unsigned i = 0u; i < count && status == 0; i++) {
		hull_sexpr_form after;
		hull_sexpr_form name;

		if (!hull_spec_find(spec, &forms[i], "after", &after))
			continue;
		hull_sexpr_child(&spec->doc, &after, &name);
		while (hull_sexpr_next(&spec->doc, &name, &name))
			for (unsigned j = 0u; j < count; j++)
				if (hull_sexpr_is(&spec->doc, &name, members[j].name))
					waits[i][j / 8u] |= (uint8_t)(1u << (j % 8u));
	}
	free(data);
	if (status != 0) {
		for (unsigned i = 0u; i < count; i++) {
			hull_msg_free(&requests[i]);
			hull_msg_free(&probes[i]);
		}
		return status;
	}

	memset(&action, 0, sizeof action);
	action.sa_handler = interrupt;
	sigaction(SIGINT, &action, NULL);
	sigaction(SIGTERM, &action, NULL);
	if (connect_core(&wire, 1) != 0 ||
	    (start_ready(&wire, members, requests, count, waits) != 0 &&
	     (hull_wire_close(&wire), 1))) {
		for (unsigned i = 0u; i < count; i++) {
			hull_msg_free(&requests[i]);
			hull_msg_free(&probes[i]);
		}
		return 125;
	}

	for (left = count; left > 0u;) {
		hull_sexpr_doc   body;
		hull_sexpr_form  value;
		long long        channel = -1;
		long long        now;
		int              wait_ms = -1;
		hull_wire_status read;
		member          *m;
		int              probe;
		int              ended = -1;

		if ((interrupted || status != 0) && !stopping) {
			stopping = 1;
			for (unsigned i = 0u; i < count; i++) {
				if (members[i].ended)
					continue;
				if (members[i].sent) {
					send_request(&wire, "(4:stop", 2u + 2u * i, members[i].id,
					             "(10:timeout_ms2:-1)");
				} else {
					members[i].ended = 1;
					left--;
				}
			}
			if (left == 0u)
				break;
		}

		/* Each machine started and not yet ready asked again when its
		 * interval is up, and given up on at its timeout. */
		now = monotonic_ms();
		for (unsigned i = 0u; i < count && !stopping && status == 0; i++) {
			m = &members[i];
			if (!m->ready || m->up || m->ended || m->until_ms == 0)
				continue;
			if (now >= m->until_ms) {
				fprintf(stderr, "hull: %s: not ready after %llds%s%s\n", m->name,
				        m->timeout_ms / 1000, m->last[0] != '\0' ? ": " : "", m->last);
				status = 125;
				break;
			}
			if (!m->probing && now >= m->next_ms) {
				hull_msg eof = { 0 };

				/* Its stdin ended at once: a command asking readiness
				 * reads nothing. A dial has none to end. */
				hull_msg_raw(&eof, "(3:eof");
				hull_msg_channel(&eof, PROBE_CHANNEL + 2u * i);
				hull_msg_raw(&eof, ")");
				m->probing = 1;
				m->said[0] = '\0';
				hull_wire_send(wire.fd, &probes[i]);
				if (m->port == 0u)
					hull_wire_send(wire.fd, &eof);
				hull_msg_free(&eof);
			}
		}
		/* Given up on just now: the top stops the rest. */
		if (status != 0 && !stopping)
			continue;
		read = hull_wire_next(&wire, &body);
		if (read == HULL_WIRE_WAIT) {
			struct pollfd ready = { wire.fd, POLLIN, 0 };

			now = monotonic_ms();
			for (unsigned i = 0u; i < count && !stopping; i++) {
				long long due;

				m = &members[i];
				if (!m->ready || m->up || m->ended || m->until_ms == 0)
					continue;
				due = m->until_ms;
				if (!m->probing && m->next_ms < due)
					due = m->next_ms;
				due -= now;
				if (due < 0)
					due = 0;
				if (wait_ms < 0 || due < wait_ms)
					wait_ms = (int)due;
			}
			if (poll(&ready, 1u, wait_ms) <= 0)
				continue;
			read = hull_wire_fill(&wire);
			if (read == HULL_WIRE_OK || read == HULL_WIRE_WAIT)
				continue;
		}
		if (read != HULL_WIRE_OK) {
			fprintf(stderr, "hull: the core went away\n");
			hull_wire_close(&wire);
			for (unsigned i = 0u; i < count; i++) {
				hull_msg_free(&requests[i]);
				hull_msg_free(&probes[i]);
			}
			return 125;
		}
		if (!hull_msg_find(&body, "ch", &value) || !hull_msg_long(&body, &value, &channel) ||
		    channel < 1 || channel % 2 != 1)
			continue;
		if ((unsigned long long)channel < 1u + 2u * count) {
			probe = 0;
			m = &members[(channel - 1) / 2];
		} else if ((unsigned long long)channel >= PROBE_CHANNEL &&
		           (unsigned long long)channel < PROBE_CHANNEL + 2u * count) {
			probe = 1;
			m = &members[(channel - PROBE_CHANNEL) / 2];
		} else {
			continue;
		}
		if (m->ended)
			continue;

		if (probe) {
			if (hull_msg_is(&body, "started") && m->port != 0u) {
				hull_msg reset = { 0 };

				/* Taken: the machine is up, and the connection, which
				 * has served its purpose, is ended by a signal, which on
				 * a dial resets it; its exit frees the channel. */
				hull_msg_raw(&reset, "(6:signal");
				hull_msg_channel(&reset, (uint32_t)channel);
				hull_msg_raw(&reset, "(6:signal4:TERM))");
				hull_wire_send(wire.fd, &reset);
				hull_msg_free(&reset);
				if (!stopping && !m->up &&
				    now_up(&wire, members, requests, count, waits, m) != 0)
					status = 125;
			} else if (hull_msg_is(&body, "data")) {
				hull_sexpr_form buf;
				hull_msg        credit = { 0 };

				if (!hull_msg_find(&body, "buf", &buf))
					continue;
				probe_said(m, body.data + buf.content, buf.content_length);
				/* Taken as soon as it comes, so a command that says much
				 * is not held back. */
				hull_msg_raw(&credit, "(6:credit(5:bytes");
				hull_msg_number(&credit, (long long)buf.content_length);
				hull_msg_raw(&credit, ")");
				hull_msg_channel(&credit, (uint32_t)channel);
				hull_msg_raw(&credit, ")");
				hull_wire_send(wire.fd, &credit);
				hull_msg_free(&credit);
			} else if (hull_msg_is(&body, "exited") || hull_msg_is(&body, "error")) {
				long long code = 0;
				long long sig  = 0;
				char      reason[200] = "an error";
				size_t    n;

				m->probing = 0;
				m->next_ms = monotonic_ms() + m->interval_ms;
				/* A dial taken, now ended: up already. */
				if (m->up)
					continue;
				if (hull_msg_is(&body, "error")) {
					if (hull_msg_find(&body, "reason", &value))
						hull_msg_string(&body, &value, reason, sizeof reason);
					if (m->port != 0u)
						snprintf(m->last, sizeof m->last, "its ready %s", reason);
					else
						snprintf(m->last, sizeof m->last,
						         "its ready command could not run: %s", reason);
					continue;
				}
				if (hull_msg_find(&body, "code", &value))
					hull_msg_long(&body, &value, &code);
				if (hull_msg_find(&body, "signal", &value))
					hull_msg_long(&body, &value, &sig);
				if (code == 0 && sig == 0) {
					if (!stopping &&
					    now_up(&wire, members, requests, count, waits, m) != 0)
						status = 125;
					continue;
				}
				for (n = strlen(m->said); n > 0u && (m->said[n - 1u] == '\n' ||
				                                     m->said[n - 1u] == '\r'); n--)
					m->said[n - 1u] = '\0';
				if (sig != 0)
					snprintf(m->last, sizeof m->last, "its ready command ended on signal %lld",
					         sig);
				else
					snprintf(m->last, sizeof m->last, "its ready command exited %lld%s%s",
					         code, m->said[0] != '\0' ? ", saying: " : "",
					         strrchr(m->said, '\n') != NULL ? strrchr(m->said, '\n') + 1 :
					         m->said);
			}
			continue;
		}

		if (hull_msg_is(&body, "started")) {
			if (m->ready) {
				m->until_ms = monotonic_ms() + m->timeout_ms;
				m->next_ms  = 0;
			} else if (!stopping && now_up(&wire, members, requests, count, waits, m) != 0) {
				status = 125;
			}
		} else if (hull_msg_is(&body, "data")) {
			hull_sexpr_form buf;
			hull_sexpr_form fd;

			if (hull_msg_find(&body, "buf", &buf) && hull_msg_find(&body, "fd", &fd))
				member_write(m, width, hull_sexpr_is(&body, &fd, "2") ? 2 : 1,
				             body.data + buf.content, buf.content_length, 0);
		} else if (hull_msg_is(&body, "lost")) {
			long long bytes = 0;

			if (hull_msg_find(&body, "bytes", &value))
				hull_msg_long(&body, &value, &bytes);
			fprintf(stderr, "hull: %s: %lld bytes of output lost: this reader fell behind\n",
			        m->name, bytes);
		} else if (hull_msg_is(&body, "exited")) {
			long long code = 0;
			long long sig  = 0;

			if (hull_msg_find(&body, "code", &value))
				hull_msg_long(&body, &value, &code);
			if (hull_msg_find(&body, "signal", &value))
				hull_msg_long(&body, &value, &sig);
			ended = sig != 0 ? 128 + (int)sig : (int)code;
		} else if (hull_msg_is(&body, "error")) {
			char reason[512] = "an error";

			if (hull_msg_find(&body, "reason", &value))
				hull_msg_string(&body, &value, reason, sizeof reason);
			fprintf(stderr, "hull: %s: %s\n", m->name, reason);
			ended = refused_status(reason);
		} else if (hull_msg_is(&body, "eof") || hull_msg_is(&body, "done")) {
			ended = 0;
		}
		if (ended >= 0) {
			member_write(m, width, 1, NULL, 0u, 1);
			member_write(m, width, 2, NULL, 0u, 1);
			m->ended  = 1;
			m->status = ended;
			left--;
			/* A machine the rest stopped is not the failure the run
			 * reports: the first that ended other than 0 is. */
			if (ended != 0 && status == 0 && !stopping)
				status = ended;
			/* One that ended before it was up can never be: what
			 * waits on it would wait for ever. */
			if (!m->up && status == 0 && !stopping)
				for (unsigned i = 0u; i < count && status == 0; i++)
					if (!members[i].sent &&
					    (waits[i][(unsigned)(m - members) / 8u] >>
					     ((unsigned)(m - members) % 8u) & 1u)) {
						fprintf(stderr, "hull: %s: not started: %s ended before it was "
						        "ready\n", members[i].name, m->name);
						status = 125;
					}
		}
	}
	hull_wire_close(&wire);
	for (unsigned i = 0u; i < count; i++) {
		hull_msg_free(&requests[i]);
		hull_msg_free(&probes[i]);
	}
	if (status == 0 && interrupted)
		for (unsigned i = 0u; i < count && status == 0; i++)
			status = members[i].status;
	return status;
}

/* `hull systemd [NAME]`: a systemd user service unit for a machine, on
 * stdout. It runs `hull run` at boot; `systemctl --user enable` and
 * `loginctl enable-linger` start it, as section 9 says. A plain .service,
 * not a podman-style quadlet, which would want a generator installed; this
 * needs nothing but systemd. Restart= is the spec's (restart ...), journald
 * takes the output by default, and socket activation (section 9, optional)
 * is not emitted. The binary and the file are named by absolute path, a
 * boot having neither this PATH nor this working directory. */
static int systemd_unit(options *o, const char *name)
{
	hull_spec       spec;
	hull_spec_error error;
	hull_sexpr_form machine;
	uint8_t        *data;
	size_t          length;
	char            id[64];
	char            self[PATH_MAX];
	char            conf[PATH_MAX];
	char            dir[PATH_MAX];
	char            restart[32] = "no";
	char           *slash;
	ssize_t         n;

	data = spec_text(o->file, &length);
	if (data == NULL)
		return 125;
	/* No store_sources: the unit's own `hull run` resolves a bare ref; here
	 * only the machine and its restart are read. */
	if (hull_spec_open(&spec, data, length, &error) != HULL_SPEC_OK) {
		fprintf(stderr, "%s:%u:%u: %s%s%s\n", o->file, error.line, error.column,
		        error.path, error.path[0] != '\0' ? ": " : "", error.reason);
		free(data);
		return 125;
	}
	if (spec.has_machine && name == NULL) {
		machine = spec.machine;
	} else if (spec.has_machines && name != NULL &&
	           hull_spec_find(&spec, &spec.machines, name, &machine)) {
		;
	} else {
		fprintf(stderr, "%s: %s\n", o->file, spec.has_machines ?
		        (name == NULL ? "name the machine: a (machines ...) file holds several" :
		         "no such machine in (machines ...)") :
		        name != NULL ? "a single machine takes no name" : "there is no machine");
		free(data);
		return 125;
	}
	if (!machine_id(&spec, &machine, name, o->base, id, sizeof id)) {
		fprintf(stderr, "%s: name the machine with (hostname ...)\n", o->file);
		free(data);
		return 125;
	}
	/* (restart no|on-failure|always), hull's own (section 4.4): the spec's
	 * values are systemd's Restart= spelling, so they pass through. Read
	 * with the spec accessor, not the engine's, since a readable symbol is
	 * not the atom hull_access_* wants; the default is no. */
	{
		hull_sexpr_form rentry;
		hull_sexpr_form rhead;
		hull_sexpr_form rvalue;
		size_t          rl;

		if (hull_spec_find(&spec, &machine, "restart", &rentry) &&
		    hull_sexpr_child(&spec.doc, &rentry, &rhead) &&
		    hull_sexpr_next(&spec.doc, &rhead, &rvalue) &&
		    hull_sexpr_value(&spec.doc, &rvalue, (uint8_t *)restart, sizeof restart - 1u, &rl) ==
		        HULL_SEXPR_OK)
			restart[rl] = '\0';
	}
	free(data);

	n = readlink("/proc/self/exe", self, sizeof self - 1u);
	if (n <= 0 || realpath(o->file, conf) == NULL) {
		fprintf(stderr, "hull: %s: %s\n", o->file, strerror(errno));
		return 1;
	}
	self[n] = '\0';
	snprintf(dir, sizeof dir, "%s", conf);
	slash = strrchr(dir, '/');
	if (slash != NULL)
		slash[slash == dir ? 1 : 0] = '\0';

	printf("[Unit]\n");
	printf("Description=hull machine %s\n", id);
	printf("After=network.target\n\n");
	printf("[Service]\n");
	printf("WorkingDirectory=%s\n", dir);
	if (name != NULL)
		printf("ExecStart=%s run -f %s %s\n", self, conf, id);
	else
		printf("ExecStart=%s run -f %s\n", self, conf);
	printf("Restart=%s\n", restart);
	printf("\n[Install]\nWantedBy=default.target\n");
	return 0;
}

static int run(options *o, const hull_registry *registry, const char *name)
{
	hull_spec            spec;
	hull_spec_error      error;
	hull_sexpr_form      machine;
	hull_wire            wire;
	hull_msg             request = { 0 };
	struct sigaction     action;
	static hull_names    names;
	uint8_t             *data;
	size_t               length;
	char                 id[64];
	int                  status;

	data = spec_text(o->file, &length);
	if (data == NULL)
		return 125;
	if (store_sources(o->file, data, &length, 1, o->backends, o->backend_count) != 0) {
		free(data);
		return 125;
	}
	if (hull_spec_open(&spec, data, length, &error) != HULL_SPEC_OK) {
		fprintf(stderr, "%s:%u:%u: %s%s%s\n", o->file, error.line, error.column,
		        error.path, error.path[0] != '\0' ? ": " : "", error.reason);
		return 125;
	}
	if (spec.has_machines && name == NULL)
		return run_all(o, registry, &spec, data);
	if (spec.has_machine && name == NULL) {
		machine = spec.machine;
	} else if (spec.has_machines && hull_spec_find(&spec, &spec.machines, name, &machine)) {
		;
	} else {
		fprintf(stderr, "%s: %s\n", o->file, spec.has_machines ?
		        "no such machine in (machines ...)" :
		        name != NULL ? "a single machine takes no name" : "there is no machine to run");
		return 125;
	}
	status = draw_names(o, &spec, &names);
	if (status == 0)
		status = prepare_run(o, registry, &spec, &machine, name, &names, 1u, &request, id,
		                     sizeof id);
	free(data);
	if (status != 0) {
		hull_msg_free(&request);
		return status;
	}

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

/* --- hull build ----------------------------------------------------------- */

static int open_store(hull_store *store, int create);
static void print_change(void *context, char change, const char *path, const char *what);

/* A build's step, section 3.6's and Appendix E's: what its cache key is
 * made of, and what it ran on. */
typedef struct step_input {
	uint8_t digest[32];
	char    manifest[65];  /* a directory's, committed, or "" for a file */
	char    path[4096];    /* on the host, as the file means it */
} step_input;

#define MAX_STEP_INPUTS 64u

static void hex_of_bytes(const uint8_t *bytes, size_t n, char *out)
{
	for (size_t i = 0u; i < n; i++)
		snprintf(out + 2u * i, 3, "%02x", bytes[i]);
}

static int bytes_of_hex(const char *hex, uint8_t *out, size_t n)
{
	for (size_t i = 0u; i < n; i++) {
		unsigned value;

		if (sscanf(hex + 2u * i, "%2x", &value) != 1)
			return -1;
		out[i] = (uint8_t)value;
	}
	return 0;
}

/* A string value of `(key "value")` in a record, into `out`, or 0. */
static int record_text(const hull_spec *spec, const hull_sexpr_form *record, const char *key,
                       char *out, size_t cap)
{
	hull_sexpr_form entry;
	hull_sexpr_form head;
	hull_sexpr_form value;
	size_t          length;

	if (!hull_spec_find(spec, record, key, &entry) || !hull_sexpr_child(&spec->doc, &entry, &head) ||
	    !hull_sexpr_next(&spec->doc, &head, &value) ||
	    hull_sexpr_value(&spec->doc, &value, (uint8_t *)out, cap - 1u, &length) != HULL_SEXPR_OK)
		return 0;
	out[length] = '\0';
	return 1;
}

/* `path`, absolute in a tree, opened beneath `root` as the tree's own
 * symlinks resolve, its directories made where missing when `make`: the
 * parent's descriptor, its last part into `*leaf`; -2 for a path with a
 * `.` or `..` part, or -1 with errno, as where a symlink leads to a
 * directory the tree has not. `copy` holds the path while `*leaf` points
 * into it. */
static int in_tree(int root, const char *path, char *copy, size_t cap, const char **leaf, int make)
{
	struct open_how how = { .flags = O_PATH | O_DIRECTORY | O_CLOEXEC,
	                        .resolve = RESOLVE_IN_ROOT | RESOLVE_NO_MAGICLINKS };
	char           *slash;
	int             fd;

	while (*path == '/')
		path++;
	if ((size_t)snprintf(copy, cap, "%s", path) >= cap || copy[0] == '\0')
		return -2;
	for (char *part = copy;; ) {
		char *end = strchr(part, '/');
		size_t n = end != NULL ? (size_t)(end - part) : strlen(part);

		if (n == 0u || (n == 1u && part[0] == '.') || (n == 2u && part[0] == '.' && part[1] == '.'))
			return -2;
		if (end == NULL)
			break;
		part = end + 1;
	}
	if ((slash = strrchr(copy, '/')) == NULL) {
		*leaf = copy;
		return (int)syscall(SYS_openat2, root, ".", &how, sizeof how);
	}
	*slash = '\0';
	*leaf = slash + 1;
	fd = (int)syscall(SYS_openat2, root, copy, &how, sizeof how);
	if (fd < 0 && errno == ENOENT && make) {
		for (char *at = copy + 1;; at++) {
			char saved;

			if (*at != '/' && *at != '\0')
				continue;
			saved = *at;
			*at = '\0';
			{
				char *last = strrchr(copy, '/');
				int   above;

				if (last != NULL) {
					*last = '\0';
					above = (int)syscall(SYS_openat2, root, copy, &how, sizeof how);
					*last = '/';
				} else {
					above = (int)syscall(SYS_openat2, root, ".", &how, sizeof how);
				}
				if (above >= 0) {
					mkdirat(above, last != NULL ? last + 1 : copy, 0755);
					close(above);
				}
			}
			*at = saved;
			if (saved == '\0')
				break;
		}
		fd = (int)syscall(SYS_openat2, root, copy, &how, sizeof how);
	}
	return fd;
}

/* An input's digest: a directory's manifest, committed with no ref, so
 * that a copy of it has its objects in the store; a file's sha256 over its
 * mode and bytes. */
static int input_of(hull_store *store, const char *path, step_input *in, char *why, size_t cap)
{
	struct stat st;

	snprintf(in->path, sizeof in->path, "%s", path);
	in->manifest[0] = '\0';
	if (stat(path, &st) != 0) {
		snprintf(why, cap, "%s: %s", path, strerror(errno));
		return -1;
	}
	if (S_ISDIR(st.st_mode)) {
		hull_commit made;

		if (hull_store_commit(store, path, host_arch(), NULL, NULL, &made, why, cap) != 0)
			return -1;
		snprintf(in->manifest, sizeof in->manifest, "%s", made.manifest);
		return bytes_of_hex(made.manifest, in->digest, 32u);
	}
	if (S_ISREG(st.st_mode)) {
		hull_sha256 h;
		uint8_t     chunk[65536];
		char        head[32];
		ssize_t     n;
		int         fd = open(path, O_RDONLY | O_CLOEXEC);

		if (fd < 0) {
			snprintf(why, cap, "%s: %s", path, strerror(errno));
			return -1;
		}
		hull_sha256_init(&h);
		n = snprintf(head, sizeof head, "file %o\n", (unsigned)(st.st_mode & 07777));
		hull_sha256_update(&h, head, (size_t)n);
		while ((n = read(fd, chunk, sizeof chunk)) > 0)
			hull_sha256_update(&h, chunk, (size_t)n);
		close(fd);
		hull_sha256_final(&h, in->digest);
		return n < 0 ? -1 : 0;
	}
	snprintf(why, cap, "%s: neither a file nor a directory", path);
	return -1;
}

/* A file into the tree at `to`, its mode kept, replacing what is there. */
static int copy_file(int root, const char *from, const char *to, char *why, size_t cap)
{
	char        copy[4096];
	const char *leaf;
	int         dir = in_tree(root, to, copy, sizeof copy, &leaf, 1);
	struct stat st;
	uint8_t     chunk[65536];
	ssize_t     n;
	int         in;
	int         out;

	if (dir < 0) {
		snprintf(why, cap, "%s: %s", to, dir == -2 ? "not a path the tree can hold" :
		         strerror(errno));
		return -1;
	}
	if ((in = open(from, O_RDONLY | O_CLOEXEC)) < 0 || fstat(in, &st) != 0) {
		snprintf(why, cap, "%s: %s", from, strerror(errno));
		close(dir);
		if (in >= 0)
			close(in);
		return -1;
	}
	unlinkat(dir, leaf, 0);
	out = openat(dir, leaf, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
	close(dir);
	if (out < 0) {
		snprintf(why, cap, "%s: %s", to, strerror(errno));
		close(in);
		return -1;
	}
	while ((n = read(in, chunk, sizeof chunk)) > 0)
		if (write(out, chunk, (size_t)n) != n) {
			n = -1;
			break;
		}
	close(in);
	if (n < 0 || fchmod(out, st.st_mode & 07777) != 0 || close(out) != 0) {
		snprintf(why, cap, "%s: %s", to, strerror(errno));
		return -1;
	}
	return 0;
}

/* A directory's committed tree into the tree at `to`, which must be absent
 * or empty there. */
static int copy_tree(hull_store *store, int root, const step_input *in, const char *to, char *why,
                     size_t cap)
{
	char        copy[4096];
	char        target[4200];
	char        name[80];
	const char *leaf;
	uint64_t    skipped = 0u;
	int         dir = in_tree(root, to, copy, sizeof copy, &leaf, 1);
	int         result;

	if (dir < 0) {
		snprintf(why, cap, "%s: %s", to, dir == -2 ? "not a path the tree can hold" :
		         strerror(errno));
		return -1;
	}
	snprintf(target, sizeof target, "/proc/self/fd/%d/%s", dir, leaf);
	snprintf(name, sizeof name, "sha256:%s", in->manifest);
	result = hull_store_materialize(store, name, target, &skipped, why, cap);
	/* The directory itself has the mode its source has, as its contents
	 * have theirs from the manifest. */
	if (result == 0) {
		struct stat st;

		if (stat(in->path, &st) != 0 || chmod(target, st.st_mode & 07777) != 0) {
			snprintf(why, cap, "%s: %s", to, strerror(errno));
			result = -1;
		}
	}
	close(dir);
	return result;
}

/* Whether `(key true)` is in `record`. */
static int record_true(const hull_spec *spec, const hull_sexpr_form *record, const char *key)
{
	char text[8];

	return record_text(spec, record, key, text, sizeof text) && strcmp(text, "true") == 0;
}

/* A tar's contents at `to`, a new or empty directory of the tree, by the
 * layer reader: what the user cannot make, a device or an owner, is said
 * and left out, and `..` and links out of the tree refused, which a host
 * tar would not hold to. Into one that holds files is refused: merging
 * would write through whatever links the tree already has. */
static int unpack_tar(int root, const char *from, const char *to, char *why, size_t cap)
{
	char         target[4300];
	char         copy[4096];
	const char  *leaf = "";
	uint64_t     skipped = 0u;
	hull_unpack *u;
	int          dir = -1;
	int          fd = open(from, O_RDONLY | O_CLOEXEC);
	int          result = -1;

	if (fd < 0) {
		snprintf(why, cap, "%s: %s", from, strerror(errno));
		return -1;
	}
	if (strcmp(to, "/") == 0) {
		snprintf(target, sizeof target, "/proc/self/fd/%d", root);
	} else {
		if ((dir = in_tree(root, to, copy, sizeof copy, &leaf, 1)) < 0) {
			snprintf(why, cap, "%s: not a path the tree can hold", to);
			close(fd);
			return -1;
		}
		if (mkdirat(dir, leaf, 0755) != 0 && errno != EEXIST) {
			snprintf(why, cap, "%s: %s", to, strerror(errno));
			goto done;
		}
		snprintf(target, sizeof target, "/proc/self/fd/%d/%s", dir, leaf);
	}
	{
		DIR           *there = opendir(target);
		struct dirent *entry;
		int            empty = there != NULL;

		while (there != NULL && (entry = readdir(there)) != NULL)
			if (strcmp(entry->d_name, ".") != 0 && strcmp(entry->d_name, "..") != 0)
				empty = 0;
		if (there != NULL)
			closedir(there);
		if (!empty) {
			snprintf(why, cap, "%s: a tar is unpacked into a new or empty directory, and this "
			         "holds files", to);
			goto done;
		}
	}
	if ((u = hull_unpack_begin(target, why, cap)) == NULL)
		goto done;
	result = hull_unpack_layer(u, fd, &skipped, why, cap);
	if (hull_unpack_end(u, result == 0 ? why : (char[64]){ 0 }, result == 0 ? cap : 64u) != 0)
		result = -1;
done:
	if (dir >= 0)
		close(dir);
	close(fd);
	return result;
}

/* Whether `name` matches `pattern`, Go's filepath.Match as .dockerignore
 * reads it: `*` and `?` within a component, `**` across them. */
static int ignore_match(const char *pattern, const char *name)
{
	for (;;) {
		if (pattern[0] == '*' && pattern[1] == '*') {
			pattern += 2;
			if (*pattern == '/')
				pattern++;
			for (;; name++) {
				if (ignore_match(pattern, name))
					return 1;
				if (*name == '\0')
					return 0;
			}
		}
		if (*pattern == '*') {
			for (pattern++;; name++) {
				if (ignore_match(pattern, name))
					return 1;
				if (*name == '\0' || *name == '/')
					return 0;
			}
		}
		if (*pattern == '\0')
			return *name == '\0' || *name == '/';  /* a directory's contents go with it */
		if (*name == '\0' || (*pattern != '?' && *pattern != *name) ||
		    (*pattern == '?' && *name == '/'))
			return 0;
		pattern++;
		name++;
	}
}

/* Whether the patterns leave out `path`, relative to the context: the last
 * to match says, `!` keeping what an earlier one left out. */
static int ignored(char *const *patterns, unsigned count, const char *path)
{
	int out = 0;

	for (unsigned i = 0u; i < count; i++) {
		const char *p = patterns[i];
		int         keep = *p == '!';

		p += keep;
		while (*p == '/' || (p[0] == '.' && p[1] == '/'))
			p += *p == '/' ? 1 : 2;
		char   clean[1024];
		size_t n = strlen(p);

		while (n > 1u && p[n - 1u] == '/')
			n--;
		snprintf(clean, sizeof clean, "%.*s", (int)n, p);
		if (ignore_match(clean, path))
			out = !keep;
	}
	return out;
}

static char *const *exclude_patterns;
static unsigned     exclude_count;
static const char  *exclude_prefix;     /* the copy's source, relative to the context */
static size_t       exclude_skip;       /* the tree walk's length before the path */

/* One entry of a tree just copied, deepest first, gone if it is ignored; a
 * directory a `!` kept something in stays. */
static int exclude_entry(const char *path, const struct stat *st, int kind, struct FTW *at)
{
	char relative[4300];

	(void)st;
	(void)at;
	if (strlen(path) <= exclude_skip)
		return 0;
	snprintf(relative, sizeof relative, "%s%s", exclude_prefix, path + exclude_skip + 1u);
	if (ignored(exclude_patterns, exclude_count, relative)) {
		if (kind == FTW_DP)
			rmdir(path);
		else
			unlink(path);
	}
	return 0;
}

/* (exclude ...) on the tree just copied to `to`. */
static void exclude_from(int root, const char *from, const char *to, char *const *patterns,
                         unsigned count)
{
	char        target[4400];
	char        prefix[4200] = "";
	size_t      n = strlen(from);

	while (from[0] == '.' && from[1] == '/')
		from += 2;
	if (strcmp(from, ".") != 0 && n > 0u)
		snprintf(prefix, sizeof prefix, "%s/", from);
	snprintf(target, sizeof target, "/proc/self/fd/%d%s", root, to);
	n = strlen(target);
	while (n > 1u && target[n - 1u] == '/')
		target[--n] = '\0';
	exclude_patterns = patterns;
	exclude_count    = count;
	exclude_prefix   = prefix;
	exclude_skip     = n;
	nftw(target, exclude_entry, 16, FTW_DEPTH | FTW_PHYS);
}

/* "7d" and its kind as seconds, or 0. */
static long long seconds_of(const char *text)
{
	char     *end;
	long long n = strtoll(text, &end, 10);

	switch (*end) {
	case 's': return n;
	case 'm': return n * 60;
	case 'h': return n * 3600;
	case 'd': return n * 86400;
	case 'w': return n * 604800;
	default:  return 0;
	}
}

/* The env a step runs with: the parent's runtime's, each name the step's
 * own (env ...) gives replaced by its value, then PATH where neither gives
 * one, and SOURCE_DATE_EPOCH, for toolchains that honour it. */
static void step_env(text_buf *t, const uint8_t *runtime, size_t runtime_length,
                     const hull_spec *spec, const hull_sexpr_form *step)
{
	hull_sexpr_doc  doc;
	hull_sexpr_form top;
	hull_sexpr_form env;
	hull_sexpr_form own;
	int             has_own = hull_spec_find(spec, step, "env", &own);
	int             has_path = 0;

	add_text(t, " (env");
	if (has_own) {
		hull_sexpr_form pair;
		int             more;

		hull_sexpr_child(&spec->doc, &own, &pair);
		for (more = hull_sexpr_next(&spec->doc, &pair, &pair); more;
		     more = hull_sexpr_next(&spec->doc, &pair, &pair)) {
			hull_sexpr_form key;
			hull_sexpr_form value;
			char            text[8192];
			size_t          n;
			size_t          before = t->length;

			hull_sexpr_child(&spec->doc, &pair, &key);
			hull_sexpr_next(&spec->doc, &key, &value);
			if (hull_sexpr_value(&spec->doc, &value, (uint8_t *)text, sizeof text - 1u, &n) !=
			    HULL_SEXPR_OK)
				continue;
			has_path |= hull_sexpr_is(&spec->doc, &key, "PATH");
			add_text(t, " (");
			add_bytes(t, spec->doc.data + key.content, key.content_length);
			add_text(t, " ");
			if (add_quoted(t, (const uint8_t *)text, n) != 0)
				cut_text(t, before);
			else
				add_text(t, ")");
		}
	}
	if (runtime != NULL &&
	    hull_sexpr_open(&doc, runtime, runtime_length, HULL_SEXPR_CANONICAL, NULL) == HULL_SEXPR_OK &&
	    hull_sexpr_first(&doc, &top) && record_find(&doc, &top, "env", &env)) {
		hull_sexpr_form pair;
		int             more;

		hull_sexpr_child(&doc, &env, &pair);
		for (more = hull_sexpr_next(&doc, &pair, &pair); more;
		     more = hull_sexpr_next(&doc, &pair, &pair)) {
			hull_sexpr_form key;
			hull_sexpr_form value;
			size_t          before = t->length;

			hull_sexpr_form mine;
			char            name[256];

			hull_sexpr_child(&doc, &pair, &key);
			hull_sexpr_next(&doc, &key, &value);
			if (!is_key(doc.data + key.content, key.content_length) ||
			    key.content_length >= sizeof name ||
			    (key.content_length == 17u &&
			     memcmp(doc.data + key.content, "SOURCE_DATE_EPOCH", 17u) == 0))
				continue;
			memcpy(name, doc.data + key.content, key.content_length);
			name[key.content_length] = '\0';
			if (has_own && hull_spec_find(spec, &own, name, &mine))
				continue;  /* the step's own */
			has_path |= key.content_length == 4u && memcmp(doc.data + key.content, "PATH", 4u) == 0;
			add_text(t, " (");
			add_bytes(t, doc.data + key.content, key.content_length);
			add_text(t, " ");
			if (add_quoted(t, doc.data + value.content, value.content_length) != 0)
				cut_text(t, before);
			else
				add_text(t, ")");
		}
	}
	if (!has_path)
		add_text(t, " (PATH \"/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin\")");
	add_text(t, " (SOURCE_DATE_EPOCH \"0\"))");
}

/* One canonical atom of `n` bytes. */
static void add_atom(text_buf *t, const void *bytes, size_t n)
{
	char length[24];

	snprintf(length, sizeof length, "%zu:", n);
	add_text(t, length);
	add_bytes(t, bytes, n);
}

typedef struct named_text {
	char *name;
	char *value;
} named_text;

static int named_order(const void *a, const void *b)
{
	return strcmp(((const named_text *)a)->name, ((const named_text *)b)->name);
}

/* Each string of the spec's `(key "a" ...)` into `items`, as names. */
static void spec_words(const hull_spec *spec, const hull_sexpr_form *record, const char *key,
                       named_text *items, size_t *count, size_t cap)
{
	hull_sexpr_form entry;
	hull_sexpr_form item;
	int             more;

	if (!hull_spec_find(spec, record, key, &entry))
		return;
	hull_sexpr_child(&spec->doc, &entry, &item);
	for (more = hull_sexpr_next(&spec->doc, &item, &item); more && *count < cap;
	     more = hull_sexpr_next(&spec->doc, &item, &item)) {
		char   text[8192];
		size_t n;

		if (hull_sexpr_value(&spec->doc, &item, (uint8_t *)text, sizeof text - 1u, &n) !=
		    HULL_SEXPR_OK)
			continue;
		text[n] = '\0';
		items[*count].name = strdup(text);
		items[*count].value = NULL;
		if (items[*count].name != NULL)
			(*count)++;
	}
}

/* Each atom of a canonical `(key a ...)` into `items`, as names. */
static void canonical_words(const hull_sexpr_doc *doc, const hull_sexpr_form *record,
                            const char *key, named_text *items, size_t *count, size_t cap)
{
	hull_sexpr_form list;
	hull_sexpr_form item;
	int             more;

	if (record == NULL || !record_find(doc, record, key, &list))
		return;
	hull_sexpr_child(doc, &list, &item);
	for (more = hull_sexpr_next(doc, &item, &item); more && *count < cap;
	     more = hull_sexpr_next(doc, &item, &item)) {
		items[*count].name = strndup((const char *)doc->data + item.content, item.content_length);
		items[*count].value = NULL;
		if (items[*count].name != NULL)
			(*count)++;
	}
}

/*
 * J.6's (runtime ...) laid over the base's, as A.4's canonical form: the
 * command, the entrypoint, the stop signal, the user and the workdir the
 * build's where it gives them, an empty (cmd) clearing the base's, as an
 * ENTRYPOINT does a CMD; the env merged by name, the build's winning; the
 * ports and the volumes joined. The workdir `/` where neither gives one.
 */
static int build_runtime(const hull_spec *spec, const hull_sexpr_form *own, const uint8_t *base,
                         size_t base_length, text_buf *out)
{
	static named_text items[1024];
	hull_sexpr_doc    doc;
	hull_sexpr_form   top;
	hull_sexpr_form   found;
	hull_sexpr_form  *from = NULL;
	static const char *const lists[] = { "cmd", "entrypoint" };
	static const char *const opens[] = { "(3:cmd", "(10:entrypoint" };
	static const char *const ones[] = { "stop_signal", "user", "workdir" };
	static const char *const one_opens[] = { "(11:stop_signal", "(4:user", "(7:workdir" };
	size_t            count;

	if (base != NULL && hull_sexpr_open(&doc, base, base_length, HULL_SEXPR_CANONICAL, NULL) ==
	                        HULL_SEXPR_OK && hull_sexpr_first(&doc, &top))
		from = &top;
	add_text(out, "(7:runtime");
	for (unsigned k = 0u; k < 2u; k++) {
		count = 0u;
		if (hull_spec_find(spec, own, lists[k], &found))
			spec_words(spec, own, lists[k], items, &count, 1024u);
		else
			canonical_words(&doc, from, lists[k], items, &count, 1024u);
		if (count > 0u) {
			add_text(out, opens[k]);
			for (size_t i = 0u; i < count; i++)
				add_atom(out, items[i].name, strlen(items[i].name));
			add_text(out, ")");
		}
		for (size_t i = 0u; i < count; i++)
			free(items[i].name);
	}
	/* env: the base's pairs, then the build's over them by name. */
	count = 0u;
	if (from != NULL && record_find(&doc, from, "env", &found)) {
		hull_sexpr_form pair;
		int             more;

		hull_sexpr_child(&doc, &found, &pair);
		for (more = hull_sexpr_next(&doc, &pair, &pair); more && count < 1024u;
		     more = hull_sexpr_next(&doc, &pair, &pair)) {
			hull_sexpr_form key;
			hull_sexpr_form value;

			hull_sexpr_child(&doc, &pair, &key);
			hull_sexpr_next(&doc, &key, &value);
			items[count].name = strndup((const char *)doc.data + key.content, key.content_length);
			items[count].value = strndup((const char *)doc.data + value.content,
			                             value.content_length);
			count++;
		}
	}
	if (hull_spec_find(spec, own, "env", &found)) {
		hull_sexpr_form pair;
		int             more;

		hull_sexpr_child(&spec->doc, &found, &pair);
		for (more = hull_sexpr_next(&spec->doc, &pair, &pair); more && count < 1024u;
		     more = hull_sexpr_next(&spec->doc, &pair, &pair)) {
			hull_sexpr_form key;
			hull_sexpr_form value;
			char            text[8192];
			size_t          n;
			char           *name;

			hull_sexpr_child(&spec->doc, &pair, &key);
			hull_sexpr_next(&spec->doc, &key, &value);
			if (hull_sexpr_value(&spec->doc, &value, (uint8_t *)text, sizeof text - 1u, &n) !=
			    HULL_SEXPR_OK)
				continue;
			name = strndup((const char *)spec->doc.data + key.content, key.content_length);
			for (size_t i = 0u; name != NULL && i < count; i++)
				if (strcmp(items[i].name, name) == 0) {
					free(items[i].name);
					free(items[i].value);
					items[i] = items[--count];
					break;
				}
			items[count].name = name;
			items[count].value = strndup(text, n);
			count++;
		}
	}
	qsort(items, count, sizeof items[0], named_order);
	if (count > 0u) {
		add_text(out, "(3:env");
		for (size_t i = 0u; i < count; i++) {
			add_text(out, "(");
			add_atom(out, items[i].name, items[i].name != NULL ? strlen(items[i].name) : 0u);
			add_atom(out, items[i].value, items[i].value != NULL ? strlen(items[i].value) : 0u);
			add_text(out, ")");
			free(items[i].name);
			free(items[i].value);
		}
		add_text(out, ")");
	}
	/* ports, then after the stop signal and user, volumes: joined. */
	for (unsigned k = 0u; k < 3u; k++) {
		static const char *const joined[] = { "ports", NULL, "volumes" };
		static const char *const joined_opens[] = { "(5:ports", NULL, "(7:volumes" };

		if (k == 1u) {
			for (unsigned j = 0u; j < 2u; j++) {
				char text[8192];

				if (record_text(spec, own, ones[j], text, sizeof text)) {
					add_text(out, one_opens[j]);
					add_atom(out, text, strlen(text));
					add_text(out, ")");
				} else if (from != NULL && record_find(&doc, from, ones[j], &found)) {
					add_bytes(out, doc.data + found.offset, found.extent);
				}
			}
			continue;
		}
		count = 0u;
		canonical_words(&doc, from, joined[k], items, &count, 1024u);
		spec_words(spec, own, joined[k], items, &count, 1024u);
		qsort(items, count, sizeof items[0], named_order);
		if (count > 0u) {
			add_text(out, joined_opens[k]);
			for (size_t i = 0u; i < count; i++)
				if (i == 0u || strcmp(items[i].name, items[i - 1u].name) != 0)
					add_atom(out, items[i].name, strlen(items[i].name));
			add_text(out, ")");
		}
		for (size_t i = 0u; i < count; i++)
			free(items[i].name);
	}
	{
		char text[8192];

		add_text(out, one_opens[2]);
		if (record_text(spec, own, "workdir", text, sizeof text))
			add_atom(out, text, strlen(text));
		else if (from != NULL && record_find(&doc, from, "workdir", &found)) {
			hull_sexpr_form head;
			hull_sexpr_form value;

			hull_sexpr_child(&doc, &found, &head);
			hull_sexpr_next(&doc, &head, &value);
			add_atom(out, doc.data + value.content, value.content_length);
		} else {
			add_atom(out, "/", 1u);
		}
		add_text(out, "))");
	}
	return out->failed ? -1 : 0;
}

/*
 * `hull build`: section 3.6's steps over the base, each committed, each
 * cached by hash(parent, step, inputs). A step's tree is its parent
 * materialized under the store's tmp/, its copies put in, its run line
 * run as a machine on it, chroot's or the engine the step names, an
 * overlay given the parent's owners and exported, its network none unless
 * it says (network true), and the tree committed, /tmp and its discard
 * paths dropped, with the parent's runtime and A.5's build
 * provenance. The last step's manifest is tagged `ref`.
 */
#define MAX_BUILD_STEPS 256u

static int build_file(options *o, const hull_registry *registry, const char *ref, int check);

/* The stages of the build in hand (J.6): each built first, as a build of
 * its own, its tree tagged under this prefix, and a stage a step copies
 * from materialized once, in the store's tmp. */
#define MAX_STAGE_TREES 16u
static char     stage_prefix[64];
static char     stage_name[MAX_STAGE_TREES][256];
static char     stage_dir[MAX_STAGE_TREES][4400];
static unsigned stage_count;

/* The tree of stage `name`, its root into `root`. 0, or -1 with `why`. */
static int stage_tree(hull_store *store, const char *name, char *root, size_t cap, char *why,
                      size_t why_cap)
{
	char     ref[400];
	uint64_t skipped = 0u;

	for (unsigned i = 0u; i < stage_count; i++)
		if (strcmp(stage_name[i], name) == 0) {
			snprintf(root, cap, "%s/root", stage_dir[i]);
			return 0;
		}
	if (stage_count == MAX_STAGE_TREES || stage_prefix[0] == '\0' || strlen(name) >= 256u) {
		snprintf(why, why_cap, "stage %.200s: there is no such stage, or too many are copied from",
		         name);
		return -1;
	}
	snprintf(ref, sizeof ref, "%s%s", stage_prefix, name);
	snprintf(stage_dir[stage_count], sizeof stage_dir[0], "%s/tmp/stage-XXXXXX", store->root);
	if (mkdtemp(stage_dir[stage_count]) == NULL) {
		snprintf(why, why_cap, "%.4000s: %s", stage_dir[stage_count], strerror(errno));
		return -1;
	}
	snprintf(root, cap, "%s/root", stage_dir[stage_count]);
	snprintf(stage_name[stage_count], sizeof stage_name[0], "%s", name);
	stage_count++;
	return hull_store_materialize(store, ref, root, &skipped, why, why_cap);
}

static void stage_trees_gone(void)
{
	for (unsigned i = 0u; i < stage_count; i++)
		hull_store_remove_tree(AT_FDCWD, stage_dir[i]);
	stage_count = 0u;
}

/* Each of the file's stages built, in order, as `(build ...)`s of their
 * own, by a descriptor holding each form's text: their steps cached as any
 * build's are. 0, or 1 where one failed. */
static int build_stages(options *o, const hull_registry *registry, hull_spec *spec,
                        const uint8_t *data, size_t length)
{
	hull_sexpr_form stages;
	hull_sexpr_form stage;
	hull_sha256     h;
	uint8_t         digest[32];
	char            hex[65];
	int             more;

	if (!hull_spec_find(spec, &spec->build, "stages", &stages))
		return 0;
	hull_sha256_init(&h);
	hull_sha256_update(&h, data, length);
	hull_sha256_final(&h, digest);
	hex_of_bytes(digest, 16u, hex);
	snprintf(stage_prefix, sizeof stage_prefix, "build/stage/%.32s/", hex);
	hull_sexpr_child(&spec->doc, &stages, &stage);
	for (more = hull_sexpr_next(&spec->doc, &stage, &stage); more;
	     more = hull_sexpr_next(&spec->doc, &stage, &stage)) {
		static const char *const keys[] = { "base", "steps", "runtime" };
		char                     name[256] = "";
		char                     ref[400];
		char                     path[64];
		options                  sub = *o;
		int                      fd = memfd_create("hull-stage", MFD_CLOEXEC);
		int                      status;

		record_text(spec, &stage, "name", name, sizeof name);
		snprintf(ref, sizeof ref, "%s%s", stage_prefix, name);
		if (fd < 0 || write(fd, "(build ", 7u) != 7) {
			fprintf(stderr, "hull: build: stage %s: %s\n", name, strerror(errno));
			return 1;
		}
		for (unsigned i = 0u; i < 3u; i++) {
			hull_sexpr_form found;

			if (hull_spec_find(spec, &stage, keys[i], &found) &&
			    (write(fd, spec->doc.data + found.offset, found.extent) != (ssize_t)found.extent ||
			     write(fd, "\n", 1u) != 1)) {
				close(fd);
				return 1;
			}
		}
		if (write(fd, ")\n", 2u) != 2) {
			close(fd);
			return 1;
		}
		snprintf(path, sizeof path, "/proc/self/fd/%d", fd);
		sub.file = path;
		fprintf(stderr, "hull: build: stage %s\n", name);
		status = build_file(&sub, registry, ref, 0);
		close(fd);
		if (status != 0)
			return status;
	}
	return 0;
}

static int build_file(options *o, const hull_registry *registry, const char *ref, int check)
{
	static char     first_pass[MAX_BUILD_STEPS][65];
	char            base_hex[65];
	char            build_arch[64];
	unsigned        index = 0u;
	int             pass = 0;
	static uint8_t  canonical[HULL_PLAN_TEXT];
	static uint8_t  work[3u * HULL_PLAN_TEXT];
	hull_spec       spec;
	hull_spec_error error;
	hull_store      store;
	hull_sexpr_form steps;
	hull_sexpr_form step;
	uint8_t        *data;
	size_t          length;
	char            base[4096];
	char            parent[65];
	char            why[4400] = "";
	int             held = -1;
	int             result = 1;
	int             more;
	int             status_of_empty;

	data = read_file(o->file, &length);
	if (data == NULL)
		return 1;
	if (hull_spec_open(&spec, data, length, &error) != HULL_SPEC_OK) {
		fprintf(stderr, "%s:%u:%u: %s%s%s\n", o->file, error.line, error.column, error.path,
		        error.path[0] != '\0' ? ": " : "", error.reason);
		free(data);
		return 1;
	}
	if (!spec.has_build) {
		fprintf(stderr, "%s: there is no (build ...) to build\n", o->file);
		free(data);
		return 1;
	}
	/* Before the store is held: a stage holds it for itself. */
	if (build_stages(o, registry, &spec, data, length) != 0) {
		free(data);
		return 1;
	}
	if (open_store(&store, 1) != 0) {
		free(data);
		return 1;
	}
	/* Held from the base to the tag, so that gc, which waits for it,
	 * cannot take a manifest no ref names yet. */
	if ((held = hull_store_hold(&store, why, sizeof why)) < 0)
		goto done;
	record_text(&spec, &spec.build, "base", base, sizeof base);
	if (strncmp(base, "dir:", 4u) == 0) {
		step_input in;
		char       absolute[8200];

		snprintf(absolute, sizeof absolute, "%s%s%s", base[4] == '/' ? "" : o->base,
		         base[4] == '/' ? "" : "/", base + 4);
		if (input_of(&store, absolute, &in, why, sizeof why) != 0 || in.manifest[0] == '\0') {
			if (in.manifest[0] == '\0' && why[0] == '\0')
				snprintf(why, sizeof why, "%.4000s: a dir base is a directory", absolute);
			goto done;
		}
		snprintf(parent, sizeof parent, "%s", in.manifest);
	} else if (strcmp(base, "scratch") == 0) {
		/* Nothing: an empty directory's tree, as a Dockerfile's scratch. */
		step_input in;
		char       empty[4400];

		snprintf(empty, sizeof empty, "%s/tmp/scratch-XXXXXX", store.root);
		if (mkdtemp(empty) == NULL) {
			snprintf(why, sizeof why, "%.4000s: %s", empty, strerror(errno));
			goto done;
		}
		chmod(empty, 0755);
		status_of_empty = input_of(&store, empty, &in, why, sizeof why);
		rmdir(empty);
		if (status_of_empty != 0)
			goto done;
		snprintf(parent, sizeof parent, "%s", in.manifest);
	} else if (strncmp(base, "stage:", 6u) == 0) {
		char named[400];

		snprintf(named, sizeof named, "%s%.300s", stage_prefix, base + 6);
		if (stage_prefix[0] == '\0' ||
		    hull_store_resolve(&store, named, parent, why, sizeof why) != 0) {
			snprintf(why, sizeof why, "%.300s: there is no such stage before this one", base);
			goto done;
		}
	} else if (hull_store_resolve(&store, base, parent, why, sizeof why) != 0) {
		hull_commit made;

		why[0] = '\0';
		fprintf(stderr, "hull: build: %s is not in the store; pulling it\n", base);
		if (hull_pull(&store, base, base, host_arch(), HULL_BUILDER, &made, why,
		              sizeof why) != 0)
			goto done;
		snprintf(parent, sizeof parent, "%s", made.manifest);
	}
	fprintf(stderr, "hull: build: base %s is sha256:%s\n", base, parent);
	snprintf(base_hex, sizeof base_hex, "%s", parent);
	/* The base's arch is the build's: each step runs as it, translated on
	 * chroot where it is foreign (section 4.4), and each tree is stored as
	 * it, a step's programs being the base's. */
	{
		char name[80];

		snprintf(name, sizeof name, "sha256:%s", base_hex);
		if (hull_store_arch(&store, name, build_arch, why, sizeof why) != 0)
			goto done;
		if (strcmp(build_arch, "none") == 0)
			snprintf(build_arch, sizeof build_arch, "%s", host_arch());
		if (strspn(build_arch, "abcdefghijklmnopqrstuvwxyz0123456789_") != strlen(build_arch)) {
			snprintf(why, sizeof why, "the base's manifest's arch is not a name");
			goto done;
		}
	}

	/* With --check, Appendix E's test of a build: every step run twice
	 * from the base, the cache read by neither, and each step's two
	 * manifests the same. */
next_pass:
	if (check)
		fprintf(stderr, "hull: build: pass %d of 2\n", pass + 1);
	snprintf(parent, sizeof parent, "%s", base_hex);
	index = 0u;
	more = hull_spec_find(&spec, &spec.build, "steps", &steps) &&
	       hull_sexpr_child(&spec.doc, &steps, &step);
	for (more = more && hull_sexpr_next(&spec.doc, &step, &step); more;
	     more = hull_sexpr_next(&spec.doc, &step, &step), index++) {
		static step_input inputs[MAX_STEP_INPUTS];
		hull_sexpr_form   entry;
		hull_sexpr_form   item;
		char              name[256] = "";
		char              run_line[8192];
		char              backend[64] = "";
		char              owners_path[4600];
		char              tar_path[4600];
		int               exported;
		static char       drops[64][4096];
		const char       *drop[66];
		unsigned          dropping = 0u;
		char              key_hex[65];
		char              cache[96];
		char              scratch[4400];
		char              root_path[4500];
		hull_sha256       h;
		uint8_t           key[32];
		uint32_t          canonical_length = 0u;
		unsigned          input_count = 0u;
		int               network;
		int               has_run;
		int               ok;
		char              step_user[128];

		record_text(&spec, &step, "name", name, sizeof name);
		has_run = record_text(&spec, &step, "run", run_line, sizeof run_line);
		record_text(&spec, &step, "backend", backend, sizeof backend);
		/* Exported where a machine runs: a step of copies alone gives no
		 * owner, and its tree is the directory as it stands. A step with a
		 * run line, on any engine, is an overlay whose tree is taken as a
		 * tar, owners and all -- on docker an import given the parent's
		 * owners and docker export, the user's uid it ran as being the
		 * commit's root as the namespace's root is on chroot. */
		exported = has_run;
		network = hull_spec_find(&spec, &step, "network", &entry) &&
		          hull_sexpr_child(&spec.doc, &entry, &item) &&
		          hull_sexpr_next(&spec.doc, &item, &item) &&
		          hull_sexpr_is(&spec.doc, &item, "true");
		if (hull_sexpr_to_canonical(&spec.doc, &step, canonical, sizeof canonical,
		                            &canonical_length, work, sizeof work) != HULL_SEXPR_OK) {
			snprintf(why, sizeof why, "step %s could not be put in canonical form", name);
			goto done;
		}
		/* The inputs: what (inputs ...) names, then each copy's from. */
		if (hull_spec_find(&spec, &step, "inputs", &entry)) {
			int go;

			hull_sexpr_child(&spec.doc, &entry, &item);
			for (go = hull_sexpr_next(&spec.doc, &item, &item); go;
			     go = hull_sexpr_next(&spec.doc, &item, &item)) {
				char   text[4096];
				char   absolute[8200];
				size_t n;

				if (input_count == MAX_STEP_INPUTS ||
				    hull_sexpr_value(&spec.doc, &item, (uint8_t *)text, sizeof text - 1u, &n) !=
				        HULL_SEXPR_OK) {
					snprintf(why, sizeof why, "step %s: too many inputs, or one too long", name);
					goto done;
				}
				text[n] = '\0';
				snprintf(absolute, sizeof absolute, "%s%s%s", text[0] == '/' ? "" : o->base,
				         text[0] == '/' ? "" : "/", text);
				if (input_of(&store, absolute, &inputs[input_count++], why, sizeof why) != 0)
					goto done;
			}
		}
		if (hull_spec_find(&spec, &step, "copy", &entry)) {
			int go;

			hull_sexpr_child(&spec.doc, &entry, &item);
			for (go = hull_sexpr_next(&spec.doc, &item, &item); go;
			     go = hull_sexpr_next(&spec.doc, &item, &item)) {
				char from[4096];
				char stage[256];
				char absolute[8200];

				if (input_count == MAX_STEP_INPUTS ||
				    !record_text(&spec, &item, "from", from, sizeof from)) {
					snprintf(why, sizeof why, "step %s: too many copies", name);
					goto done;
				}
				/* From a stage's tree, which `from` must not leave. */
				if (record_text(&spec, &item, "stage", stage, sizeof stage)) {
					char tree[4500];

					if (strstr(from, "..") != NULL) {
						snprintf(why, sizeof why, "step %s: a stage's path holds ..", name);
						goto done;
					}
					if (stage_tree(&store, stage, tree, sizeof tree, why, sizeof why) != 0)
						goto done;
					snprintf(absolute, sizeof absolute, "%s/%.3600s", tree, from);
				} else {
					snprintf(absolute, sizeof absolute, "%s%s%s", from[0] == '/' ? "" : o->base,
					         from[0] == '/' ? "" : "/", from);
				}
				if (input_of(&store, absolute, &inputs[input_count++], why, sizeof why) != 0)
					goto done;
			}
		}
		/* The key: the parent, the step as it is written, the inputs as
		 * they are, and with a network, the refresh period it falls in. */
		hull_sha256_init(&h);
		hull_sha256_update(&h, "hull build step 1\n", 18u);
		hull_sha256_update(&h, parent, 64u);
		hull_sha256_update(&h, canonical, canonical_length);
		for (unsigned i = 0u; i < input_count; i++)
			hull_sha256_update(&h, inputs[i].digest, 32u);
		if (network) {
			char      refresh[64] = "7d";
			char      bucket[40];
			long long period;
			int       n;

			record_text(&spec, &step, "refresh", refresh, sizeof refresh);
			period = seconds_of(refresh);
			n = snprintf(bucket, sizeof bucket, "network %lld",
			             period > 0 ? (long long)time(NULL) / period : 0LL);
			hull_sha256_update(&h, bucket, (size_t)n);
		}
		hull_sha256_final(&h, key);
		hex_of_bytes(key, 32u, key_hex);
		snprintf(cache, sizeof cache, "build/cache/%s", key_hex);
		if (index == MAX_BUILD_STEPS) {
			snprintf(why, sizeof why, "more than %u steps", MAX_BUILD_STEPS);
			goto done;
		}
		if (!check && hull_store_resolve(&store, cache, parent, why, sizeof why) == 0) {
			fprintf(stderr, "hull: build: step %s: cached, sha256:%s\n", name, parent);
			continue;
		}
		why[0] = '\0';
		fprintf(stderr, "hull: build: step %s\n", name);

		/* Its tree: the parent, as a directory the step writes. */
		snprintf(scratch, sizeof scratch, "%s/tmp/build-XXXXXX", store.root);
		if (mkdtemp(scratch) == NULL) {
			snprintf(why, sizeof why, "%.4000s: %s", scratch, strerror(errno));
			goto done;
		}
		/* Not setgid, though the store's directory may be: what is made in
		 * it takes the user's own group, which a machine's namespace maps,
		 * and not one overlayfs could not copy up, EOVERFLOW. */
		if (chmod(scratch, 0700) != 0) {
			snprintf(why, sizeof why, "%.4000s: %s", scratch, strerror(errno));
			goto done;
		}
		snprintf(root_path, sizeof root_path, "%s/root", scratch);
		{
			char        named[80];
			uint64_t    skipped = 0u;
			uint8_t    *runtime = NULL;
			size_t      runtime_length = 0u;
			hull_commit made;
			int         root = -1;
			unsigned    copied = 0u;

			snprintf(named, sizeof named, "sha256:%s", parent);
			ok = hull_store_materialize(&store, named, root_path, &skipped, why, sizeof why) == 0 &&
			     hull_store_runtime(&store, named, &runtime, &runtime_length, why, sizeof why) == 0 &&
			     (root = open(root_path, O_RDONLY | O_DIRECTORY | O_CLOEXEC)) >= 0;
			/* Exported, the step's machine is an overlay of that copy, the
			 * parent's owners given back in it and its tree exported, so an
			 * owner it gives, a package's own user's, is kept with no file
			 * of another owner on the user's disk (section 7). */
			snprintf(owners_path, sizeof owners_path, "%s/owners.list", scratch);
			snprintf(tar_path, sizeof tar_path, "%s/tree.tar", scratch);
			if (ok && exported) {
				int      list = open(owners_path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
				uint64_t owned = 0u;

				ok = list >= 0 &&
				     hull_store_owners(&store, named, list, &owned, why, sizeof why) == 0;
				if (list >= 0 && close(list) != 0)
					ok = 0;
			}
			/* The copies, each its committed tree or its file, at its to. */
			if (ok && hull_spec_find(&spec, &step, "copy", &entry)) {
				int go;
				unsigned first = input_count;

				hull_sexpr_child(&spec.doc, &entry, &item);
				for (go = hull_sexpr_next(&spec.doc, &item, &item); go;
				     go = hull_sexpr_next(&spec.doc, &item, &item))
					copied++;
				first = input_count - copied;
				hull_sexpr_child(&spec.doc, &entry, &item);
				for (go = hull_sexpr_next(&spec.doc, &item, &item); go && ok;
				     go = hull_sexpr_next(&spec.doc, &item, &item), first++) {
					char to[4096];

					record_text(&spec, &item, "to", to, sizeof to);
					if (record_true(&spec, &item, "unpack")) {
						ok = unpack_tar(root, inputs[first].path, to, why, sizeof why) == 0;
						continue;
					}
					/* A file to a path ending in a slash goes into it. */
					if (inputs[first].manifest[0] == '\0' && to[strlen(to) - 1u] == '/') {
						const char *leaf = strrchr(inputs[first].path, '/');

						if (leaf != NULL && strlen(to) + strlen(leaf) < sizeof to)
							strcat(to, leaf + 1);
					}
					ok = inputs[first].manifest[0] != '\0' ?
					     copy_tree(&store, root, &inputs[first], to, why, sizeof why) == 0 :
					     copy_file(root, inputs[first].path, to, why, sizeof why) == 0;
					if (ok && inputs[first].manifest[0] != '\0') {
						char           *patterns[512];
						unsigned        patterns_count = 0u;
						hull_sexpr_form list;
						char            from[4096];

						if (hull_spec_find(&spec, &item, "exclude", &list) &&
						    record_text(&spec, &item, "from", from, sizeof from)) {
							hull_sexpr_form word;

							hull_sexpr_child(&spec.doc, &list, &word);
							while (hull_sexpr_next(&spec.doc, &word, &word) && patterns_count < 512u) {
								char   text[1024];
								size_t got;

								/* A file's string, escapes and all, no canonical atom. */
								if (hull_sexpr_value(&spec.doc, &word, (uint8_t *)text,
								                     sizeof text - 1u, &got) != HULL_SEXPR_OK)
									continue;
								text[got] = '\0';
								if ((patterns[patterns_count] = strdup(text)) != NULL)
									patterns_count++;
							}
							exclude_from(root, from, to, patterns, patterns_count);
							for (unsigned p = 0u; p < patterns_count; p++)
								free(patterns[p]);
						}
					}
				}
			}
			/* The run line, in a chroot machine on the tree, or the
			 * engine the step names. */
			if (ok && has_run) {
				text_buf conf = { 0 };
				options  sub = { .wait = -1 };
				char     conf_path[4600];
				char     workdir[4096] = "/";
				FILE    *f;
				int      status;

				if (runtime != NULL) {
					hull_sexpr_doc  doc;
					hull_sexpr_form top;
					hull_sexpr_form found;

					if (hull_sexpr_open(&doc, runtime, runtime_length, HULL_SEXPR_CANONICAL,
					                    NULL) == HULL_SEXPR_OK && hull_sexpr_first(&doc, &top) &&
					    record_find(&doc, &top, "workdir", &found)) {
						hull_sexpr_form head;
						hull_sexpr_form value;

						hull_sexpr_child(&doc, &found, &head);
						hull_sexpr_next(&doc, &head, &value);
						if (value.content_length < sizeof workdir) {
							memcpy(workdir, doc.data + value.content, value.content_length);
							workdir[value.content_length] = '\0';
						}
					}
				}
				/* The step's own, made in the tree where it is missing, as
				 * docker makes a WORKDIR. */
				record_text(&spec, &step, "workdir", workdir, sizeof workdir);
				if (strcmp(workdir, "/") != 0) {
					char        copy[4096];
					const char *leaf;
					int         above = in_tree(root, workdir, copy, sizeof copy, &leaf, 1);

					if (above < 0 || (mkdirat(above, leaf, 0755) != 0 && errno != EEXIST)) {
						snprintf(why, sizeof why, "step %s: its workdir %s: %s", name, workdir,
						         above == -2 ? "not a path the tree can hold" : strerror(errno));
						ok = 0;
					}
					if (above >= 0)
						close(above);
				}
				add_text(&conf, "(machine (source ");
				add_text(&conf, "\"dir:");
				add_text(&conf, root_path);
				add_text(&conf, "\") (mutable overlay) (owners ");
				add_quoted(&conf, (const uint8_t *)owners_path, strlen(owners_path));
				add_text(&conf, ") (export ");
				add_quoted(&conf, (const uint8_t *)tar_path, strlen(tar_path));
				add_text(&conf, ") (backend ");
				add_text(&conf, backend[0] == '\0' || strcmp(backend, "ns") == 0 ? "chroot" :
				                backend);
				add_text(&conf, ") (hostname \"build-");
				add_bytes(&conf, key_hex, 12u);
				add_text(&conf, "\")");
				if (record_text(&spec, &step, "user", step_user, sizeof step_user)) {
					add_text(&conf, " (user ");
					add_quoted(&conf, (const uint8_t *)step_user, strlen(step_user));
					add_text(&conf, ")");
				}
				if (strcmp(build_arch, host_arch()) != 0) {
					add_text(&conf, " (arch ");
					add_text(&conf, build_arch);
					add_text(&conf, ")");
				}
				/* (network true) is the host's on chroot, and on a vm its
				 * own; otherwise none. */
				if (!network)
					add_text(&conf, " (network none)");
				else if (backend[0] == '\0' || strcmp(backend, "ns") == 0 ||
				         strcmp(backend, "chroot") == 0)
					add_text(&conf, " (network host)");
				add_text(&conf, " (workdir ");
				add_quoted(&conf, (const uint8_t *)workdir, strlen(workdir));
				add_text(&conf, ") (command \"/bin/sh\" \"-c\" ");
				if (add_quoted(&conf, (const uint8_t *)run_line, strlen(run_line)) != 0) {
					snprintf(why, sizeof why, "step %s: its run line holds what a string cannot",
					         name);
					ok = 0;
				}
				add_text(&conf, ")");
				step_env(&conf, runtime, runtime_length, &spec, &step);
				add_text(&conf, ")\n");
				snprintf(conf_path, sizeof conf_path, "%s/step.conf", scratch);
				if (ok && (conf.failed || strpbrk(root_path, "\"\\") != NULL ||
				           (f = fopen(conf_path, "w")) == NULL ||
				           fwrite(conf.data, 1u, conf.length, f) != conf.length || fclose(f) != 0)) {
					snprintf(why, sizeof why, "step %s: its machine could not be written", name);
					ok = 0;
				}
				free(conf.data);
				if (ok) {
					sub.file = conf_path;
					sub.stream = stderr;
					sub.base = scratch;
					status = run(&sub, registry, NULL);
					if (status != 0) {
						snprintf(why, sizeof why, "step %s exited %d", name, status);
						ok = 0;
					}
				}
			}
			/* What a step leaves that is not the tree's: /tmp's contents,
			 * and what it says to discard; removed from a tree written in
			 * place, or left out of an exported one's commit. */
			if (exported) {
				dropping = 0u;
				drop[dropping++] = "/tmp/";
				if (ok && hull_spec_find(&spec, &step, "discard", &entry)) {
					int go;

					hull_sexpr_child(&spec.doc, &entry, &item);
					for (go = hull_sexpr_next(&spec.doc, &item, &item); go && dropping < 64u;
					     go = hull_sexpr_next(&spec.doc, &item, &item)) {
						size_t n;

						if (hull_sexpr_value(&spec.doc, &item, (uint8_t *)drops[dropping],
						                     sizeof drops[0] - 1u, &n) != HULL_SEXPR_OK)
							continue;
						drops[dropping][n] = '\0';
						drop[dropping] = drops[dropping];
						dropping++;
					}
				}
				drop[dropping] = NULL;
			}
			if (ok && !exported) {
				char        copy[4096];
				const char *leaf;
				int         tmp = in_tree(root, "/tmp/x", copy, sizeof copy, &leaf, 0);

				if (tmp >= 0) {
					DIR           *list = fdopendir(openat(tmp, ".", O_RDONLY | O_DIRECTORY |
					                                            O_CLOEXEC));
					struct dirent *gone;

					while (list != NULL && (gone = readdir(list)) != NULL)
						if (strcmp(gone->d_name, ".") != 0 && strcmp(gone->d_name, "..") != 0)
							hull_store_remove_tree(dirfd(list), gone->d_name);
					if (list != NULL)
						closedir(list);
					close(tmp);
				}
				if (hull_spec_find(&spec, &step, "discard", &entry)) {
					int go;

					hull_sexpr_child(&spec.doc, &entry, &item);
					for (go = hull_sexpr_next(&spec.doc, &item, &item); go;
					     go = hull_sexpr_next(&spec.doc, &item, &item)) {
						char   text[4096];
						size_t n;
						int    dir;

						if (hull_sexpr_value(&spec.doc, &item, (uint8_t *)text, sizeof text - 1u,
						                     &n) != HULL_SEXPR_OK)
							continue;
						text[n] = '\0';
						if ((dir = in_tree(root, text, copy, sizeof copy, &leaf, 0)) >= 0) {
							hull_store_remove_tree(dir, leaf);
							close(dir);
						}
					}
				}
			}
			if (root >= 0)
				close(root);
			/* Committed with its parent's runtime and A.5's provenance:
			 * the inputs, the parent and the step. */
			if (ok) {
				text_buf          provenance = { 0 };
				hull_commit_extra extra = { 0 };
				uint8_t           raw[32];
				char              atom[64];

				add_text(&provenance, "(10:provenance(5:build(6:inputs");
				for (unsigned i = 0u; i < input_count; i++) {
					add_text(&provenance, "32:");
					add_bytes(&provenance, inputs[i].digest, 32u);
				}
				add_text(&provenance, ")(6:parent32:");
				bytes_of_hex(parent, raw, 32u);
				add_bytes(&provenance, raw, 32u);
				add_text(&provenance, ")(4:step");
				add_bytes(&provenance, canonical, canonical_length);
				snprintf(atom, sizeof atom, "))(7:builder%zu:%s))", strlen(HULL_BUILDER),
				         HULL_BUILDER);
				add_text(&provenance, atom);
				extra.provenance = (const uint8_t *)provenance.data;
				extra.provenance_length = provenance.length;
				extra.runtime = runtime;
				extra.runtime_length = runtime_length;
				if (!provenance.failed && exported) {
					int tar = open(tar_path, O_RDONLY | O_CLOEXEC);

					ok = tar >= 0 && hull_store_commit_tar(&store, tar, build_arch, cache, drop,
					                                       &extra, &made, why, sizeof why) == 0;
					if (tar < 0)
						snprintf(why, sizeof why, "step %s: its tree was not exported: %s", name,
						         strerror(errno));
					if (tar >= 0)
						close(tar);
				} else {
					ok = !provenance.failed &&
					     hull_store_commit(&store, root_path, build_arch, cache, &extra, &made,
					                       why, sizeof why) == 0;
				}
				if (ok) {
					snprintf(parent, sizeof parent, "%s", made.manifest);
					fprintf(stderr, "hull: build: step %s: sha256:%s\n", name, parent);
				}
				if (ok && check && pass == 0)
					snprintf(first_pass[index], sizeof first_pass[index], "%s", parent);
				if (ok && check && pass == 1 && strcmp(first_pass[index], parent) != 0) {
					char then[80];
					char now[80];

					printf("step %s is not reproducible: sha256:%s, then sha256:%s%s\n", name,
					       first_pass[index], parent,
					       network ? "; it has a network, which Appendix E does not expect to "
					                 "reproduce" : "");
					snprintf(then, sizeof then, "sha256:%.64s", first_pass[index]);
					snprintf(now, sizeof now, "sha256:%.64s", parent);
					hull_store_diff(&store, then, now, print_change, NULL, why, sizeof why);
					snprintf(why, sizeof why, "the build is not reproducible");
					ok = 0;
				}
				free(provenance.data);
			}
			free(runtime);
			hull_store_remove_tree(AT_FDCWD, scratch);
			if (!ok) {
				if (why[0] == '\0')
					snprintf(why, sizeof why, "step %s could not be built", name);
				goto done;
			}
		}
	}
	if (check && pass++ == 0)
		goto next_pass;
	/* The build's own runtime over the tree's: a manifest of the same
	 * entries and provenance, its runtime the two laid together. */
	{
		hull_sexpr_form own;

		if (hull_spec_find(&spec, &spec.build, "runtime", &own)) {
			text_buf runtime = { 0 };
			uint8_t *carried = NULL;
			size_t   carried_length = 0u;
			char     named[80];
			char     hex[65];
			int      laid;

			snprintf(named, sizeof named, "sha256:%s", parent);
			laid = hull_store_runtime(&store, named, &carried, &carried_length, why,
			                          sizeof why) == 0 &&
			       build_runtime(&spec, &own, carried, carried_length, &runtime) == 0 &&
			       hull_store_with_runtime(&store, named, (const uint8_t *)runtime.data,
			                               runtime.length, hex, why, sizeof why) == 0;
			free(carried);
			free(runtime.data);
			if (!laid) {
				if (why[0] == '\0')
					snprintf(why, sizeof why, "the build's runtime could not be laid over its tree");
				goto done;
			}
			snprintf(parent, sizeof parent, "%s", hex);
			fprintf(stderr, "hull: build: its runtime: sha256:%s\n", parent);
		}
	}
	if (check)
		printf("reproducible: every step the same manifest twice\n");
	{
		char named[80];
		char hex[65];

		snprintf(named, sizeof named, "sha256:%s", parent);
		if (hull_store_tag(&store, named, ref, hex, why, sizeof why) != 0)
			goto done;
		printf("%s sha256:%s\n", ref, hex);
		result = 0;
	}
done:
	if (result != 0)
		fprintf(stderr, "hull: build: %s\n", why[0] != '\0' ? why : "it could not be built");
	stage_trees_gone();
	hull_store_release(held);
	hull_store_close(&store);
	free(data);
	return result;
}

/* A compose file's build: services, each its Dockerfile imported as
 * `hull import` prints it and built with its context as the file's
 * directory, under the ref the service runs from. Cached as any build is,
 * so a second `hull up` costs a look. Its output is stderr's: stdout is the
 * machines'. 0, or 1. */
static int compose_builds(const options *o, const hull_registry *registry)
{
	const hull_compose_build *list;
	unsigned                  count;
	char                     *text = hull_import_compose(o->file, NULL);
	int                       status = 0;

	if (text == NULL)
		return 1;
	free(text);
	count = hull_compose_builds(&list);
	for (unsigned i = 0u; i < count && status == 0; i++) {
		options sub = *o;
		char    context[8200];
		char    dockerfile[8600];
		char    path[64];
		int     out = dup(1);
		int     fd = memfd_create("hull-compose", MFD_CLOEXEC);

		snprintf(context, sizeof context, "%s%s%s", list[i].context[0] == '/' ? "" : o->base,
		         list[i].context[0] == '/' ? "" : "/", list[i].context);
		snprintf(dockerfile, sizeof dockerfile, "%s/%s", context, list[i].dockerfile);
		if (fd < 0 || out < 0) {
			perror("hull: up");
			return 1;
		}
		fprintf(stderr, "hull: up: building %s from %s\n", list[i].ref, dockerfile);
		fflush(stdout);
		dup2(fd, 1);
		status = hull_import_build(dockerfile, list[i].args, list[i].arg_count,
		                           list[i].target[0] != '\0' ? list[i].target : NULL);
		fflush(stdout);
		dup2(2, 1);
		if (status == 0) {
			snprintf(path, sizeof path, "/proc/self/fd/%d", fd);
			sub.file = path;
			sub.base = realpath(context, NULL);
			status = sub.base == NULL ? 1 : build_file(&sub, registry, list[i].ref, 0);
			free(sub.base);
		}
		fflush(stdout);
		dup2(out, 1);
		close(out);
		close(fd);
	}
	return status != 0;
}

/* A Vagrantfile's shell provisioners, each machine's a build on its box,
 * as compose_builds() builds a service: so a second `hull up` costs a look
 * where Vagrant would run them once and keep the machine. 0, or 1. */
static int vagrant_builds(const options *o, const hull_registry *registry)
{
	const hull_vagrant_build *list;
	unsigned                  count;
	char                     *text = hull_vagrant_form(o->file, NULL);
	int                       status = 0;

	if (text == NULL)
		return 1;
	free(text);
	count = hull_vagrant_builds(&list);
	for (unsigned i = 0u; i < count && status == 0; i++) {
		options sub = *o;
		char    path[64];
		int     out = dup(1);
		int     fd = memfd_create("hull-vagrant", MFD_CLOEXEC);
		size_t  length = list[i].form != NULL ? strlen(list[i].form) : 0u;

		if (fd < 0 || out < 0 || length == 0u || write(fd, list[i].form, length) != (ssize_t)length) {
			perror("hull: up");
			return 1;
		}
		fprintf(stderr, "hull: up: provisioning %s\n", list[i].ref);
		snprintf(path, sizeof path, "/proc/self/fd/%d", fd);
		sub.file = path;
		fflush(stdout);
		dup2(2, 1);
		status = build_file(&sub, registry, list[i].ref, 0);
		fflush(stdout);
		dup2(out, 1);
		close(out);
		close(fd);
	}
	return status != 0;
}

/* --- the other commands -------------------------------------------------- */

/* Whether a core answers, without starting one; or would, holding the
 * machines a core that went kept for it. */
/* The engine of the machine list_machines() was last asked for. */
static char listed_engine[64];

/* `hull ps`; or with `want`, nothing printed, and that machine's arch
 * into `arch`, "" where it says none, the host's: 0 where it is listed. */
static int list_machines(const char *want, char *arch, size_t cap)
{
	hull_wire      wire;
	hull_sexpr_doc body;
	hull_sexpr_form message;
	hull_sexpr_form item;
	int            more;
	int            status = connect_core(&wire, 0);

	if (status == 1) {
		if (want != NULL)
			return 1;
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
	if (want == NULL)
		printf("%-24s %-15s %-10s %-6s %s\n", "ID", "ADDRESS", "ENGINE", "CLASS", "STATE");
	hull_sexpr_first(&body, &message);
	hull_sexpr_child(&body, &message, &item);
	for (more = hull_sexpr_next(&body, &item, &item); more;
	     more = hull_sexpr_next(&body, &item, &item)) {
		const char *keys[] = { "id", "address", "engine", "class", "state", "root", "arch" };
		char        values[7][64] = { "", "", "", "", "", "", "" };
		hull_sexpr_form head;

		hull_sexpr_child(&body, &item, &head);
		if (!hull_sexpr_is(&body, &head, "machine"))
			continue;
		for (unsigned k = 0u; k < 7u; k++) {
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
		if (want != NULL) {
			if (strcmp(values[0], want) != 0)
				continue;
			snprintf(arch, cap, "%s", values[6]);
			snprintf(listed_engine, sizeof listed_engine, "%s", values[2]);
			hull_wire_close(&wire);
			return 0;
		}
		/* Root mode's machine says so, after its state (section 7). */
		printf("%-24s %-15s %-10s %-6s %s%s\n", values[0], values[1], values[2], values[3],
		       values[4], strcmp(values[5], "true") == 0 ? " ROOT" : "");
	}
	hull_wire_close(&wire);
	return want != NULL ? 1 : 0;
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

/* `hull commit`: a directory, or with `tar` a tar of engine/tree.c's shape
 * from the file `dir` names, `-` for stdin, its owners kept. */
static int exec_in(const char *id, char **argv, int argc, int tty, unsigned port);
static int commit_from(const char *dir, const char *ref, const char *arch, int fd, int quiet);

/* The running machine `id`'s root as a tar, from an exec of (tree true)
 * in a child whose output is the pipe `*fd` reads: the child's pid, or -1
 * said. */
static pid_t tree_of(const char *id, int *fd)
{
	int   ends[2];
	pid_t child;

	if (pipe2(ends, O_CLOEXEC) != 0 || (child = fork()) < 0) {
		fprintf(stderr, "hull: commit: %s\n", strerror(errno));
		return -1;
	}
	if (child == 0) {
		int none = open("/dev/null", O_RDONLY);

		if (none < 0 || dup2(none, 0) < 0 || dup2(ends[1], 1) < 0)
			_exit(125);
		_exit(exec_in(id, NULL, 0, 0, 0u));
	}
	close(ends[1]);
	*fd = ends[0];
	return child;
}

/* `hull commit DIR REF`, `--tar FILE REF`, or a running machine's root,
 * `hull commit NAME REF` where NAME is no directory: what it is now,
 * written by the machine's engine as (export PATH) writes it at its end,
 * its volumes, /proc, /sys and /dev out. A tar its writer did not finish
 * is refused, so no ref is made of part of one. */
static int commit(const char *dir, const char *ref, const char *arch, int tar)
{
	struct stat is;
	pid_t       child = -1;
	int         ended = 0;

	if (!tar && stat(dir, &is) != 0 && errno == ENOENT && strchr(dir, '/') == NULL) {
		char own[64] = "";
		int  fd;
		int  status;

		/* Its arch where --arch says none: a machine of a foreign one,
		 * its programs that arch's, is stored as it, as a pull is. */
		if (arch == NULL && list_machines(dir, own, sizeof own) == 0 && own[0] != '\0')
			arch = own;
		if (arch == NULL)
			arch = host_arch();
		if ((child = tree_of(dir, &fd)) < 0)
			return 1;
		status = commit_from(dir, ref, arch, fd, 1);
		close(fd);
		while (waitpid(child, &ended, 0) < 0 && errno == EINTR)
			;
		/* The exec's own refusal, said by it, is the reason: what the
		 * store then says of an empty or cut tar is only its echo. */
		if (!WIFEXITED(ended) || WEXITSTATUS(ended) != 0) {
			if (status == 0)
				fprintf(stderr, "hull: commit: the machine's tree did not end as a tar "
				        "should\n");
			return 1;
		}
		if (status != 0)
			return commit_from(NULL, NULL, NULL, -1, 2);
		return 0;
	}
	if (arch == NULL)
		arch = host_arch();
	if (tar) {
		int fd = strcmp(dir, "-") == 0 ? 0 : open(dir, O_RDONLY | O_CLOEXEC);

		if (fd < 0) {
			fprintf(stderr, "hull: commit: %s: %s\n", dir, strerror(errno));
			return 1;
		}
		ended = commit_from(NULL, ref, arch, fd, 0);
		if (fd > 0)
			close(fd);
		return ended;
	}
	return commit_from(dir, ref, arch, -1, 0);
}

/* The commit itself: of the directory `dir` where `fd` is -1, else of the
 * tar `fd` reads. `quiet` 1 keeps a failure's reason for a call with
 * `quiet` 2 to say, once its caller knows it is the reason. */
static int commit_from(const char *dir, const char *ref, const char *arch, int fd, int quiet)
{
	static char why[4400];
	hull_store  store;
	hull_commit made;
	int         status;

	if (quiet == 2) {
		fprintf(stderr, "hull: commit: %s\n", why);
		return 1;
	}
	why[0] = '\0';
	if ((status = open_store(&store, 1)) != 0)
		return status;
	{
		char              provenance[200];
		hull_commit_extra extra = { 0 };

		/* A.5's builder, alone: a commit has no build and no registry. */
		snprintf(provenance, sizeof provenance, "(10:provenance(7:builder%zu:%s))",
		         strlen(HULL_BUILDER), HULL_BUILDER);
		extra.provenance = (const uint8_t *)provenance;
		extra.provenance_length = strlen(provenance);
		status = fd >= 0 ? hull_store_commit_tar(&store, fd, arch, ref, NULL, &extra, &made, why,
		                                         sizeof why) :
		                   hull_store_commit(&store, dir, arch, ref, &extra, &made, why, sizeof why);
	}
	hull_store_close(&store);
	if (status != 0) {
		if (!quiet)
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

/* --- pools and leases (section 4.12) ----------------------------------- */

/* `m` asked of the core on channel 1, and its answer into `body`, what
 * it says on fd 2 meanwhile -- a wait -- written to stderr: 0, 1 having
 * said the core's refusal, with its status in `*status`, or 2. */
static int ask_core(hull_wire *wire, hull_msg *m, hull_sexpr_doc *body, int *status, int start)
{
	int connected = connect_core(wire, start);

	if (connected != 0) {
		hull_msg_free(m);
		if (connected == 1)
			fprintf(stderr, "hull: no core is running, so no lease is held\n");
		*status = connected == 1 ? 1 : 2;
		return 1;
	}
	hull_wire_send(wire->fd, m);
	hull_msg_free(m);
	for (;;) {
		hull_sexpr_form value;

		if (hull_wire_read(wire, body) != HULL_WIRE_OK) {
			fprintf(stderr, "hull: the core went away\n");
			*status = 2;
			return 1;
		}
		if (hull_msg_is(body, "data") && hull_msg_find(body, "buf", &value)) {
			write_all(2, body->data + value.content, value.content_length);
			continue;
		}
		if (hull_msg_is(body, "error")) {
			char reason[512] = "an error";

			if (hull_msg_find(body, "reason", &value))
				hull_msg_string(body, &value, reason, sizeof reason);
			fprintf(stderr, "hull: %s\n", reason);
			*status = refused_status(reason);
			return 1;
		}
		return 0;
	}
}

/* `hull reserve POOL [--count N] [--wait D|--no-wait]`: a lease's id,
 * alone on stdout, so that `lease=$(hull reserve ci)` takes it. */
static int reserve(int argc, char **argv)
{
	hull_wire       wire;
	hull_sexpr_doc  body;
	hull_sexpr_form value;
	hull_msg        m = { 0 };
	const char     *pool = NULL;
	long long       count = 1;
	long long       wait = -1;
	int             status = 0;
	char            id[64] = "";

	for (int i = 2; i < argc; i++) {
		if (strcmp(argv[i], "--count") == 0 && i + 1 < argc)
			count = strtoll(argv[++i], NULL, 10);
		else if (strcmp(argv[i], "--wait") == 0 && i + 1 < argc)
			wait = seconds_of(argv[++i]) > 0 ? seconds_of(argv[i]) : -2;
		else if (strcmp(argv[i], "--no-wait") == 0)
			wait = 0;
		else if (argv[i][0] != '-' && pool == NULL)
			pool = argv[i];
		else
			pool = NULL, i = argc;
	}
	if (pool == NULL || count < 1 || wait == -2) {
		usage(stderr);
		return 2;
	}
	hull_msg_raw(&m, "(5:lease");
	hull_msg_channel(&m, 1u);
	hull_msg_raw(&m, "(5:count");
	hull_msg_number(&m, count);
	hull_msg_raw(&m, ")(4:pool");
	hull_msg_text(&m, pool);
	hull_msg_raw(&m, ")");
	if (wait >= 0) {
		hull_msg_raw(&m, "(4:wait");
		hull_msg_number(&m, wait);
		hull_msg_raw(&m, ")");
	}
	hull_msg_raw(&m, ")");
	/* The core is started for it, as for a run: a lease is the core's. */
	if (ask_core(&wire, &m, &body, &status, 1) != 0)
		return status;
	if (!hull_msg_is(&body, "lease") || !hull_msg_find(&body, "id", &value) ||
	    !hull_msg_string(&body, &value, id, sizeof id)) {
		hull_wire_close(&wire);
		fprintf(stderr, "hull: the core's answer to a lease is not one\n");
		return 2;
	}
	hull_wire_close(&wire);
	printf("%s\n", id);
	return 0;
}

/* `hull release ID`: the lease ended, its machines stopped first. */
static int release(const char *id)
{
	hull_wire      wire;
	hull_sexpr_doc body;
	hull_msg       m = { 0 };
	int            status = 0;

	hull_msg_raw(&m, "(4:free");
	hull_msg_channel(&m, 1u);
	hull_msg_raw(&m, "(2:id");
	hull_msg_text(&m, id);
	hull_msg_raw(&m, "))");
	if (ask_core(&wire, &m, &body, &status, 1) != 0)
		return status;
	/* The answer is read before the wire it lies in is let go of. */
	status = hull_msg_is(&body, "done") ? 0 : 2;
	hull_wire_close(&wire);
	return status;
}

/* `hull lease ls` and `hull pool ls`: one line a record, its columns
 * fixed, a header first, as `hull ps` prints. */
static int list_of(int pools)
{
	static const char *const lease_keys[] = { "id", "pool", "count", "machines" };
	static const char *const pool_keys[]  = { "name", "capacity", "held", "waiting" };
	const char *const       *keys = pools ? pool_keys : lease_keys;
	hull_wire                wire;
	hull_sexpr_doc           body;
	hull_sexpr_form          message;
	hull_sexpr_form          item;
	hull_msg                 m = { 0 };
	int                      status = 0;

	hull_msg_raw(&m, pools ? "(5:pools" : "(6:leases");
	hull_msg_channel(&m, 1u);
	hull_msg_raw(&m, ")");
	if (pools)
		printf("%-24s %-9s %-6s %s\n", "POOL", "CAPACITY", "HELD", "WAITING");
	else
		printf("%-24s %-24s %-6s %s\n", "ID", "POOL", "COUNT", "MACHINES");
	/* The core is started to say: a lease outlives the core that made
	 * it, kept in hull/leases/, and the next one reads it back. */
	if (ask_core(&wire, &m, &body, &status, 1) != 0)
		return status;
	hull_sexpr_first(&body, &message);
	hull_sexpr_child(&body, &message, &item);
	while (hull_sexpr_next(&body, &item, &item)) {
		char            values[4][64] = { "", "", "", "" };
		hull_sexpr_form head;

		if (item.kind != HULL_SEXPR_LIST || !hull_sexpr_child(&body, &item, &head) ||
		    !hull_sexpr_is(&body, &head, pools ? "pool" : "lease"))
			continue;
		for (unsigned k = 0u; k < 4u; k++) {
			hull_sexpr_form value;

			if (hull_access_find(&body, &item, keys[k], &value)) {
				hull_sexpr_form name;
				hull_sexpr_form v;

				if (hull_sexpr_child(&body, &value, &name) && hull_sexpr_next(&body, &name, &v))
					hull_msg_string(&body, &v, values[k], sizeof values[k]);
			}
		}
		if (pools)
			printf("%-24s %-9s %-6s %s\n", values[0], values[1], values[2], values[3]);
		else
			printf("%-24s %-24s %-6s %s\n", values[0], values[1], values[2], values[3]);
	}
	hull_wire_close(&wire);
	return 0;
}

/* The stored tree SRC names: a directory committed first, its owners as a
 * commit takes them, or a stored ref as it is. 0, or -1 with `why`. */
static int tree_name(hull_store *store, const char *source, char *name, size_t cap, char *why,
                     size_t why_cap)
{
	struct stat is;

	if (stat(source, &is) == 0 && S_ISDIR(is.st_mode)) {
		hull_commit       made;
		hull_commit_extra extra = { 0 };
		char              provenance[200];

		snprintf(provenance, sizeof provenance, "(10:provenance(7:builder%zu:%s))",
		         strlen(HULL_BUILDER), HULL_BUILDER);
		extra.provenance = (const uint8_t *)provenance;
		extra.provenance_length = strlen(provenance);
		if (hull_store_commit(store, source, host_arch(), NULL, &extra, &made, why,
		                      why_cap) != 0)
			return -1;
		snprintf(name, cap, "sha256:%s", made.manifest);
	} else {
		snprintf(name, cap, "%s", source);
	}
	return 0;
}

/* The container format of a virtual machine's disk, by its magic, as
 * qemu-img names it; NULL for anything else. */
static const char *container_format(const char *image)
{
	unsigned char head[80] = { 0 };
	int           fd = open(image, O_RDONLY | O_CLOEXEC);
	ssize_t       got = fd >= 0 ? read(fd, head, sizeof head) : -1;

	if (fd >= 0)
		close(fd);
	if (got < 8)
		return NULL;
	if (memcmp(head, "QFI\xfb", 4u) == 0)
		return "qcow2";
	if (memcmp(head, "KDMV", 4u) == 0)
		return "vmdk";
	if (memcmp(head, "vhdxfile", 8u) == 0)
		return "vhdx";
	if (memcmp(head, "conectix", 8u) == 0)
		return "vpc";
	if (got >= 68 && memcmp(head + 64, "\x7f\x10\xda\xbe", 4u) == 0)
		return "vdi";
	return NULL;
}

/* `hull media import IMAGE DIR` (section 3.10), and `hull import IMAGE
 * DIR`, section 3.11's one algorithm: what it understood and what it did
 * not, the patch being the measure of that. */
/* An appliance, `.ova`: a tar of an .ovf and its disks. The disk taken
 * apart, the first where there are several, which is said. */
static int media_import(const char *image, const char *dir);
static int run_child(const char *const *argv);

static int ova_import(const char *image, const char *dir)
{
	char           work[4300];
	char           disk[4600] = "";
	DIR           *list;
	struct dirent *entry;
	unsigned       disks = 0u;
	int            status = 1;

	snprintf(work, sizeof work, "%s.ova-XXXXXX", dir);
	if (mkdtemp(work) == NULL) {
		perror(work);
		return 1;
	}
	{
		const char *const argv[] = { "tar", "-xf", image, "-C", work, "--no-same-owner", NULL };

		if (run_child(argv) != 0) {
			fprintf(stderr, "hull: media import: %s is not an appliance's tar\n", image);
			goto done;
		}
	}
	if ((list = opendir(work)) == NULL)
		goto done;
	while ((entry = readdir(list)) != NULL) {
		const char *dot = strrchr(entry->d_name, '.');

		if (dot == NULL || strcmp(dot, ".vmdk") != 0)
			continue;
		disks++;
		/* The disks of one appliance are numbered, so the least name. */
		if (disk[0] == '\0' || strcmp(strrchr(disk, '/') + 1, entry->d_name) > 0)
			snprintf(disk, sizeof disk, "%s/%s", work, entry->d_name);
	}
	closedir(list);
	if (disk[0] == '\0') {
		fprintf(stderr, "hull: media import: %s holds no disk\n", image);
		goto done;
	}
	if (disks > 1u)
		fprintf(stderr, "hull: media import: %s holds %u disks, and only %s is taken\n", image,
		        disks, strrchr(disk, '/') + 1);
	status = media_import(disk, dir);
done:
	hull_store_remove_tree(AT_FDCWD, work);
	return status;
}

static int media_import(const char *image, const char *dir)
{
	hull_media_report report;
	char              why[4400] = "";
	char              raw[4400] = "";
	const char       *format = container_format(image);
	size_t            length = strlen(image);
	int               status;

	if (length > 4u && strcmp(image + length - 4u, ".ova") == 0)
		return ova_import(image, dir);
	/* A virtual machine's disk is its raw disk in a container qemu-img
	 * reads: that is what is taken apart, and what export gives back, the
	 * container's own bytes being no part of the disk (section 3.13). */
	if (format != NULL) {
		pid_t pid;
		int   child = 1;

		snprintf(raw, sizeof raw, "%s.raw", dir);
		if ((pid = fork()) == 0) {
			execlp("qemu-img", "qemu-img", "convert", "-f", format, "-O", "raw", image, raw,
			       (char *)NULL);
			_exit(127);
		}
		if (pid < 0 || waitpid(pid, &child, 0) < 0 || !WIFEXITED(child) ||
		    WEXITSTATUS(child) != 0) {
			unlink(raw);
			fprintf(stderr, "hull: media import: qemu-img could not read %s as %s\n", image, format);
			return 1;
		}
		image = raw;
	}
	status = hull_media_import(image, dir, &report, why, sizeof why);
	if (raw[0] != '\0')
		unlink(raw);
	if (status != 0) {
		fprintf(stderr, "hull: media import: %s\n", why);
		return 1;
	}
	if (format != NULL)
		printf("%s: a %s disk, taken as its raw disk\n", dir, format);
	printf("%s: %llu file%s, %llu bytes, %llu of them patch\n", dir, report.files,
	       report.files == 1u ? "" : "s", report.bytes, report.patch_bytes);
	return 0;
}

/* A child's status, stdout on stderr: this process's own is the caller's. */
static int run_child(const char *const *argv)
{
	int   status = 1;
	pid_t pid = fork();

	if (pid == 0) {
		dup2(2, 1);
		execvp(argv[0], (char *const *)argv);
		_exit(127);
	}
	if (pid < 0 || waitpid(pid, &status, 0) < 0)
		return 1;
	return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
}

/* `hull vagrant box add NAME FILE`: a box is a tar around a virtual disk.
 * The disk taken apart, the partition with a /sbin/init is the machine's
 * root, committed as vagrant/NAME, which a Vagrantfile's box names. Its
 * files are the user's, and its owners and devices stay in the patch that
 * is dropped here, so what the image's own users own is root's. */
static int vagrant_box_add(const char *name, const char *file)
{
	char           self[4096];
	char           work[4200];
	char           disk[4500] = "";
	char           collection[4300];
	char           root[4500] = "";
	char           ref[300];
	hull_store     store;
	DIR           *dir;
	struct dirent *entry;
	ssize_t        n = readlink("/proc/self/exe", self, sizeof self - 1u);
	int            status = 1;

	if (n <= 0 || strpbrk(name, " \t\"") != NULL || open_store(&store, 1) != 0) {
		fprintf(stderr, "hull: vagrant: a box's name is one word\n");
		return 1;
	}
	self[n] = '\0';
	snprintf(work, sizeof work, "%s/tmp/box-XXXXXX", store.root);
	hull_store_close(&store);
	if (mkdtemp(work) == NULL) {
		perror(work);
		return 1;
	}
	{
		const char *const argv[] = { "tar", "-xf", file, "-C", work, "--no-same-owner", NULL };

		if (run_child(argv) != 0) {
			fprintf(stderr, "hull: vagrant: %s is not a box's tar\n", file);
			goto done;
		}
	}
	if ((dir = opendir(work)) == NULL)
		goto done;
	while ((entry = readdir(dir)) != NULL) {
		const char *dot = strrchr(entry->d_name, '.');

		if (dot != NULL && (strcmp(dot, ".vmdk") == 0 || strcmp(dot, ".img") == 0 ||
		                    strcmp(dot, ".qcow2") == 0 || strcmp(dot, ".vdi") == 0 ||
		                    strcmp(dot, ".vhd") == 0 || strcmp(dot, ".vhdx") == 0)) {
			snprintf(disk, sizeof disk, "%s/%s", work, entry->d_name);
			break;
		}
	}
	closedir(dir);
	if (disk[0] == '\0') {
		fprintf(stderr, "hull: vagrant: %s holds no disk\n", file);
		goto done;
	}
	snprintf(collection, sizeof collection, "%s/collection", work);
	{
		const char *const argv[] = { self, "media", "import", disk, collection, NULL };

		if (run_child(argv) != 0)
			goto done;
	}
	/* The disk's own tree, or the partition that is an operating system. */
	snprintf(root, sizeof root, "%s/files", collection);
	{
		char probe[4600];

		snprintf(probe, sizeof probe, "%s/sbin/init", root);
		if (access(probe, F_OK) != 0 && faccessat(AT_FDCWD, probe, F_OK, AT_SYMLINK_NOFOLLOW) != 0) {
			char found[4500] = "";

			for (unsigned i = 1u; i <= 16u && found[0] == '\0'; i++) {
				snprintf(probe, sizeof probe, "%s/files/p%u/sbin/init", collection, i);
				if (faccessat(AT_FDCWD, probe, F_OK, AT_SYMLINK_NOFOLLOW) == 0)
					snprintf(found, sizeof found, "%s/files/p%u", collection, i);
			}
			if (found[0] == '\0') {
				fprintf(stderr, "hull: vagrant: no partition of %s has a /sbin/init\n", file);
				goto done;
			}
			snprintf(root, sizeof root, "%s", found);
		}
	}
	snprintf(ref, sizeof ref, "vagrant/%s", name);
	{
		const char *const argv[] = { self, "commit", root, ref, NULL };

		status = run_child(argv);
	}
	if (status == 0)
		printf("%s\n", ref);
done:
	hull_store_remove_tree(AT_FDCWD, work);
	return status != 0;
}

/* `hull media change ID DRIVE [IMAGE]`: a running pc machine's floppy or CD
 * swapped, or with no image taken out, for a set of disks. It goes by exec,
 * the engine's own word as `console` is, so it is asked of a pc machine alone:
 * anything else would run it as a program. A collection is not taken, being
 * reassembled as a machine starts: `hull media export` makes it an image. */
static int media_change(const char *id, const char *drive, const char *image)
{
	char        arch[32];
	char        path[PATH_MAX];
	struct stat st;
	char       *words[3] = { "change", (char *)drive, path };

	if (list_machines(id, arch, sizeof arch) != 0) {
		fprintf(stderr, "hull: media change: no machine %s\n", id);
		return 125;
	}
	if (strcmp(listed_engine, "qemu-pc") != 0) {
		fprintf(stderr, "hull: media change: %s is on %s; a pc machine's drives are what change\n",
		        id, listed_engine);
		return 125;
	}
	if (image == NULL)
		return exec_in(id, words, 2, 0, 0u);
	if (realpath(image, path) == NULL || stat(path, &st) != 0) {
		fprintf(stderr, "hull: media change: %s: %s\n", image, strerror(errno));
		return 125;
	}
	if (!S_ISREG(st.st_mode)) {
		fprintf(stderr, "hull: media change: %s is not an image file; `hull media export` makes "
		        "one of a collection\n", image);
		return 125;
	}
	return exec_in(id, words, 3, 0, 0u);
}

/* `hull media verify DIR`: reassembled and held to the hash of what was
 * imported. */
static int media_verify(const char *dir)
{
	char why[4400] = "";
	char temporary[4300];
	int  fd;
	int  status;

	snprintf(temporary, sizeof temporary, "%s/.verify-XXXXXX", dir);
	if ((fd = mkostemp(temporary, O_CLOEXEC)) < 0) {
		fprintf(stderr, "hull: media: %s: %s\n", dir, strerror(errno));
		return 1;
	}
	unlink(temporary);
	/* A raw dump's own, where it was one; else the disc. */
	status = hull_media_write_raw(dir, fd, why, sizeof why);
	if (status == 1)
		status = hull_media_write(dir, fd, why, sizeof why);
	close(fd);
	if (status != 0) {
		fprintf(stderr, "hull: media: %s\n", why);
		return 1;
	}
	printf("the collection gives its disc, byte for byte\n");
	return 0;
}

/* `hull media export DIR OUT`: the same, kept. */
static int media_export(const char *dir, const char *out)
{
	char why[4400] = "";

	if (hull_media_export(dir, out, why, sizeof why) != 0) {
		fprintf(stderr, "hull: media: %s\n", why);
		return 1;
	}
	printf("%s\n", out);
	return 0;
}

/* `hull export --bootable SRC IMAGE [--init PATH]` (section 3.13): the
 * tree SRC is as a disk image that boots by itself, PATH its PID 1,
 * /sbin/init unless said. */
/* `host`: `--host TTY`, the hosted form (hull_export_hosted), whose agent
 * waits on that UART for an engine's core rather than booting the tree. */
static int export_bootable(const char *source, const char *image, const char *init,
                           const char *host)
{
	hull_store store;
	char       name[200];
	char       why[4400] = "";
	int        status;

	if ((status = open_store(&store, 1)) != 0)
		return status;
	if (tree_name(&store, source, name, sizeof name, why, sizeof why) != 0 ||
	    (host != NULL ? hull_export_hosted(&store, name, image, host, why, sizeof why) :
	                    hull_export_bootable(&store, name, image, init, why, sizeof why)) != 0) {
		hull_store_close(&store);
		fprintf(stderr, "hull: export: %s\n", why);
		return 1;
	}
	hull_store_close(&store);
	printf("%s\n", image);
	return 0;
}

/* The one value of `key` in the spec's machine, allocated, or NULL. */
static char *spec_value(hull_spec *spec, const hull_sexpr_form *machine, const char *key)
{
	hull_sexpr_form entry;
	hull_sexpr_form head;
	hull_sexpr_form value;

	if (!hull_spec_find(spec, machine, key, &entry) ||
	    !hull_sexpr_child(&spec->doc, &entry, &head) ||
	    !hull_sexpr_next(&spec->doc, &head, &value))
		return NULL;
	return strndup((const char *)spec->doc.data + value.content, value.content_length);
}

/* `hull export --bootable -f FILE [NAME] IMAGE`: the machine's own source
 * and init, from its spec -- a dir: source relative to the file, as a run
 * reads it, committed first; a tar: or pack: one committed as a run
 * commits it; a stored ref as it is. */
static int export_machine(const char *file, const char *name, const char *image)
{
	hull_spec       spec;
	hull_spec_error error;
	hull_sexpr_form machine;
	uint8_t        *data;
	size_t          length;
	char           *source;
	char           *init;
	char            path[4400];
	int             status;

	if ((data = spec_text(file, &length)) == NULL)
		return 125;
	if (store_sources(file, data, &length, 1, NULL, 0u) != 0) {
		free(data);
		return 125;
	}
	if (hull_spec_open(&spec, data, length, &error) != HULL_SPEC_OK) {
		fprintf(stderr, "%s:%u:%u: %s%s%s\n", file, error.line, error.column, error.path,
		        error.path[0] != '\0' ? ": " : "", error.reason);
		free(data);
		return 125;
	}
	if (spec.has_machine && name == NULL) {
		machine = spec.machine;
	} else if (!(spec.has_machines && name != NULL &&
	             hull_spec_find(&spec, &spec.machines, name, &machine))) {
		fprintf(stderr, "%s: %s\n", file, spec.has_machines ?
		        "no such machine in (machines ...)" :
		        name != NULL ? "a single machine takes no name" : "there is no machine to export");
		free(data);
		return 125;
	}
	source = spec_value(&spec, &machine, "source");
	init   = spec_value(&spec, &machine, "init");
	if (source == NULL) {
		fprintf(stderr, "%s: the machine names no source\n", file);
		status = 125;
	} else if (strncmp(source, "dir:", 4u) == 0 && source[4] != '/') {
		const char *slash = strrchr(file, '/');

		snprintf(path, sizeof path, "%.*s%s", slash != NULL ? (int)(slash - file + 1) : 0,
		         file, source + 4);
		status = export_bootable(path, image, init != NULL ? init : "/sbin/init", NULL);
	} else {
		status = export_bootable(strncmp(source, "dir:", 4u) == 0 ? source + 4 : source, image,
		                         init != NULL ? init : "/sbin/init", NULL);
	}
	free(source);
	free(init);
	free(data);
	return status;
}

/* `hull pack SRC FILE` (section 3.13): the tree SRC is, a directory --
 * committed first, its owners as a commit takes them -- or a stored ref,
 * as one file, its manifest's hash printed. */
static int pack(const char *source, const char *file)
{
	hull_store store;
	char       name[200];
	char       hex[65];
	char       why[4400] = "";
	int        status;
	int        fd;

	if ((status = open_store(&store, 1)) != 0)
		return status;
	if (tree_name(&store, source, name, sizeof name, why, sizeof why) != 0) {
		hull_store_close(&store);
		fprintf(stderr, "hull: pack: %s\n", why);
		return 1;
	}
	if ((fd = open(file, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644)) < 0) {
		hull_store_close(&store);
		fprintf(stderr, "hull: pack: %s: %s\n", file, strerror(errno));
		return 1;
	}
	status = hull_store_pack(&store, name, fd, hex, why, sizeof why);
	hull_store_close(&store);
	if (close(fd) != 0 && status == 0)
		snprintf(why, sizeof why, "%s: %s", file, strerror(errno)), status = -1;
	if (status != 0) {
		unlink(file);
		fprintf(stderr, "hull: pack: %s\n", why);
		return 1;
	}
	printf("%s sha256:%s\n", file, hex);
	return 0;
}

/* `hull unpack FILE DIR`: the pack into the store, as pack/ and its
 * manifest's hash's first 32 hex digits, every object and the manifest
 * checked, then materialized into DIR. */
static int unpack(const char *file, const char *dir)
{
	hull_store store;
	char       ref[64];
	char       hex[65];
	char       why[4400] = "";
	uint64_t   skipped = 0u;
	int        status;
	int        fd = strcmp(file, "-") == 0 ? 0 : open(file, O_RDONLY | O_CLOEXEC);

	if (fd < 0) {
		fprintf(stderr, "hull: unpack: %s: %s\n", file, strerror(errno));
		return 1;
	}
	if ((status = open_store(&store, 1)) != 0) {
		if (fd > 0)
			close(fd);
		return status;
	}
	status = hull_store_unpack(&store, fd, NULL, hex, why, sizeof why);
	if (fd > 0)
		close(fd);
	if (status == 0) {
		char given[80];

		snprintf(ref, sizeof ref, "pack/%.32s", hex);
		snprintf(given, sizeof given, "sha256:%s", hex);
		status = hull_store_tag(&store, given, ref, hex, why, sizeof why);
	}
	if (status == 0)
		status = hull_store_materialize(&store, ref, dir, &skipped, why, sizeof why);
	hull_store_close(&store);
	if (status != 0) {
		fprintf(stderr, "hull: unpack: %s\n", why);
		return 1;
	}
	printf("%s sha256:%s\n", ref, hex);
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

/* An image from its registry into the store, as `ref`, or as the image's
 * own name, so that a machine whose source is that name runs it. */
/* `arch` is the platform an index is narrowed to, the host's where NULL:
 * another's tree runs on chroot translated, taking its arch from the
 * manifest (section 4.4). */
static int pull(const char *name, const char *ref, const char *arch)
{
	hull_store  store;
	hull_commit made;
	char        why[4400] = "";
	int         status;

	if (arch == NULL)
		arch = host_arch();
	if (arch == NULL) {
		fprintf(stderr, "hull: pull: the host's arch could not be read\n");
		return 1;
	}
	if ((status = open_store(&store, 1)) != 0)
		return status;
	status = hull_pull(&store, name, ref, arch, HULL_BUILDER, &made, why, sizeof why);
	hull_store_close(&store);
	if (status != 0) {
		fprintf(stderr, "hull: pull: %s\n", why);
		return 1;
	}
	printf("%s sha256:%s\n", ref, made.manifest);
	fprintf(stderr, "hull: %llu entries, %llu objects, %llu new, %llu bytes%s\n",
	        (unsigned long long)made.entries, (unsigned long long)made.objects,
	        (unsigned long long)made.new_objects, (unsigned long long)made.total_size,
	        made.skipped != 0u ? "; some were left out, as said" : "");
	return 0;
}

/* `hull load FILE [REF]`: an image saved to a file, with no registry. */
static int load(const char *path, const char *ref)
{
	hull_store  store;
	hull_commit made;
	char        why[4400] = "";
	char        name[512];
	const char *arch = host_arch();
	int         status;

	if (arch == NULL) {
		fprintf(stderr, "hull: load: the host's arch could not be read\n");
		return 1;
	}
	if ((status = open_store(&store, 1)) != 0)
		return status;
	status = hull_load(&store, path, ref, arch, HULL_BUILDER, &made, name, sizeof name, why,
	                   sizeof why);
	hull_store_close(&store);
	if (status != 0) {
		fprintf(stderr, "hull: load: %s\n", why);
		return 1;
	}
	printf("%s sha256:%s\n", name, made.manifest);
	return 0;
}

/* A stored tree to its registry, as a single-layer image. */
static int push(const char *name, const char *image)
{
	hull_store store;
	char       digest[80];
	char       why[4400] = "";
	int        status;

	if ((status = open_store(&store, 0)) != 0) {
		if (status == 1)
			fprintf(stderr, "hull: push: there is no store, so no %s\n", name);
		return 1;
	}
	status = hull_push(&store, name, image, digest, why, sizeof why);
	hull_store_close(&store);
	if (status != 0) {
		fprintf(stderr, "hull: push: %s\n", why);
		return 1;
	}
	printf("%s %s\n", image, digest);
	return 0;
}

static int tag(const char *from, const char *ref)
{
	hull_store store;
	char       hex[65];
	char       why[4400] = "";
	int        status;

	if ((status = open_store(&store, 0)) != 0) {
		if (status == 1)
			fprintf(stderr, "hull: tag: there is no store, so no %s\n", from);
		return 1;
	}
	status = hull_store_tag(&store, from, ref, hex, why, sizeof why);
	hull_store_close(&store);
	if (status != 0) {
		fprintf(stderr, "hull: tag: %s\n", why);
		return 1;
	}
	printf("%s sha256:%s\n", ref, hex);
	return 0;
}

static int untag(const char *ref)
{
	hull_store store;
	char       why[4400] = "";
	int        status;

	if ((status = open_store(&store, 0)) != 0) {
		if (status == 1)
			fprintf(stderr, "hull: tag: there is no store, so no %s\n", ref);
		return 1;
	}
	status = hull_store_untag(&store, ref, why, sizeof why);
	hull_store_close(&store);
	if (status != 0) {
		fprintf(stderr, "hull: tag: %s\n", why);
		return 1;
	}
	return 0;
}

static int show_manifest(const char *name)
{
	hull_store store;
	char       why[4400] = "";
	int        status;

	if ((status = open_store(&store, 0)) != 0) {
		if (status == 1)
			fprintf(stderr, "hull: manifest: there is no store, so no %s\n", name);
		return 1;
	}
	status = hull_store_show(&store, name, stdout, why, sizeof why);
	hull_store_close(&store);
	if (status != 0) {
		fprintf(stderr, "hull: manifest: %s\n", why);
		return 1;
	}
	return 0;
}

/* `hull manifest composefs REF FILE`: the tree REF names as a composefs
 * EROFS image in FILE (section 3.2), made new, for an overlay over the
 * store's objects to serve. */
static int composefs_manifest(const char *name, const char *file)
{
	hull_store store;
	char       why[4400] = "";
	int        status;
	int        fd;

	if ((status = open_store(&store, 0)) != 0) {
		if (status == 1)
			fprintf(stderr, "hull: manifest: there is no store, so no %s\n", name);
		return 1;
	}
	if ((fd = open(file, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644)) < 0) {
		fprintf(stderr, "hull: manifest: %s: %s\n", file, strerror(errno));
		hull_store_close(&store);
		return 1;
	}
	status = hull_store_composefs(&store, name, fd, why, sizeof why);
	hull_store_close(&store);
	if (close(fd) != 0 && status == 0)
		snprintf(why, sizeof why, "%s: %s", file, strerror(errno)), status = -1;
	if (status != 0) {
		unlink(file);
		fprintf(stderr, "hull: manifest: %s\n", why);
		return 1;
	}
	return 0;
}

/* `hull manifest compile [REF]`: a readable manifest view on stdin, made
 * canonical and stored (its sha256 printed), tagged REF where one is given.
 * The view's way back: `hull manifest show X | edit | hull manifest compile
 * Y`. Compiling an unedited view reproduces X's bytes, so its name. */
static int compile_manifest(const char *ref)
{
	hull_store store;
	char       why[4400] = "";
	char       hex[65];
	uint8_t   *view = NULL;
	size_t     length = 0u;
	size_t     cap = 0u;
	int        status;

	for (;;) {
		ssize_t n;

		if (length == cap) {
			uint8_t *grown;

			if (cap >= (64u << 20)) {
				fprintf(stderr, "hull: manifest: the view on stdin is too large\n");
				free(view);
				return 2;
			}
			cap = cap == 0u ? 65536u : cap * 2u;
			if ((grown = realloc(view, cap)) == NULL) {
				free(view);
				fprintf(stderr, "hull: out of memory\n");
				return 2;
			}
			view = grown;
		}
		n = read(0, view + length, cap - length);
		if (n < 0) {
			fprintf(stderr, "hull: stdin: %s\n", strerror(errno));
			free(view);
			return 2;
		}
		if (n == 0)
			break;
		length += (size_t)n;
	}
	if (open_store(&store, 1) != 0) {
		fprintf(stderr, "hull: manifest: the store could not be opened\n");
		free(view);
		return 1;
	}
	status = hull_store_compile(&store, view, length, ref, hex, why, sizeof why);
	hull_store_close(&store);
	free(view);
	if (status != 0) {
		fprintf(stderr, "hull: %s\n", why);
		return 1;
	}
	printf("sha256:%s\n", hex);
	if (ref != NULL)
		printf("%s\n", ref);
	return 0;
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
		/* No argv is (tree true): the machine's root as a tar on the
		 * output, for commit_machine. */
		if (argv == NULL) {
			hull_msg_raw(&request, "(4:exec(4:tree4:true)");
		} else {
			hull_msg_raw(&request, "(4:exec(4:argv");
			for (int i = 0; i < argc; i++)
				hull_msg_text(&request, argv[i]);
			hull_msg_raw(&request, ")");
		}
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
				/* A connection the far side closed first is no error of
				 * ssh's: its last bytes crossing that close is how a login
				 * ends, and a ProxyCommand's stderr is the user's screen. */
				if (port == 0u || !started ||
				    strcmp(reason, "the command's input has closed") != 0)
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
/* The machine's hull-managed host public key, under the data home where
 * (ssh true) kept the persistent one; 1 and its path, or 0. Its presence is
 * how `hull ssh` knows a machine runs an sshd it can reach and verify. */
static int machine_host_pub(const char *id, char *out, size_t cap)
{
	const char *data = getenv("XDG_DATA_HOME");
	const char *home = getenv("HOME");
	int         n = -1;

	if (data != NULL && data[0] == '/')
		n = snprintf(out, cap, "%s/hull/machines/%s/host_key.pub", data, id);
	else if (home != NULL && home[0] == '/')
		n = snprintf(out, cap, "%s/.local/share/hull/machines/%s/host_key.pub", home, id);
	return n > 0 && (size_t)n < cap && access(out, R_OK) == 0;
}

/*
 * A real ssh into the machine's own sshd, as section 8.1 item 2 prefers
 * when one runs: the user's ssh client, over `hull proxy-ssh` to port 22,
 * logging in as root (the guest's own user). Where `pub` names the
 * machine's persistent host public key -- an sshd that (ssh true)
 * provisioned -- it is pinned in a known_hosts of its own, so ssh verifies
 * the machine with no prompt and no churn across runs. Where `pub` is NULL
 * -- an sshd the image runs of its own, with its own keys and authorized
 * keys -- ssh uses the user's own known_hosts as it would for any host.
 * Returns ssh's exit, or -1 where ssh cannot be started -- then the caller
 * falls back to the exec shell.
 */
static int ssh_client(const char *id, const char *pub)
{
	char    self[4096];
	char    known[64];
	char    proxy[4200];
	char    hosts[4300];
	char    target[80];
	char    line[8192];
	char   *argv[14];
	int     ai = 0;
	int     pinned = pub != NULL;
	pid_t   child;
	int     fd;
	int     status;
	ssize_t n = readlink("/proc/self/exe", self, sizeof self - 1u);

	if (n <= 0 || (size_t)n >= sizeof self - 1u || strpbrk(self, "\"%\\\n") != NULL)
		return -1;  /* no usable path for a ProxyCommand: the exec shell */
	self[n] = '\0';
	if (pinned) {
		FILE *pf = fopen(pub, "r");
		char *type;
		char *key;

		/* The host public key's type and data, without its comment. */
		if (pf == NULL || fgets(line, sizeof line, pf) == NULL) {
			if (pf != NULL)
				fclose(pf);
			return -1;
		}
		fclose(pf);
		type = strtok(line, " \t\n");
		key  = type != NULL ? strtok(NULL, " \t\n") : NULL;
		if (type == NULL || key == NULL)
			return -1;
		snprintf(known, sizeof known, "/tmp/hull-known.XXXXXX");
		if ((fd = mkstemp(known)) < 0)
			return -1;
		snprintf(line, sizeof line, "%s.hull %s %s\n", id, type, key);
		if (write(fd, line, strlen(line)) != (ssize_t)strlen(line)) {
			close(fd);
			unlink(known);
			return -1;
		}
		close(fd);
		snprintf(hosts, sizeof hosts, "UserKnownHostsFile=%s", known);
	}
	snprintf(proxy, sizeof proxy, "ProxyCommand=\"%s\" proxy-ssh %%h", self);
	/* The door on a pc machine is the host's sshd, run as the user, which
	 * logs in the user alone; a guest's own sshd is the guest's root. */
	{
		struct passwd *who = strcmp(listed_engine, "qemu-pc") == 0 ? getpwuid(getuid()) : NULL;

		snprintf(target, sizeof target, "%s@%s.hull", who != NULL ? who->pw_name : "root", id);
	}
	argv[ai++] = "ssh";
	argv[ai++] = "-o";
	argv[ai++] = proxy;
	if (pinned) {
		argv[ai++] = "-o";
		argv[ai++] = hosts;
		argv[ai++] = "-o";
		argv[ai++] = "GlobalKnownHostsFile=/dev/null";
		argv[ai++] = "-o";
		argv[ai++] = "StrictHostKeyChecking=yes";
	}
	argv[ai++] = target;
	argv[ai]   = NULL;
	child = fork();
	if (child == 0) {
		execvp("ssh", argv);
		_exit(127);
	}
	if (child < 0 || waitpid(child, &status, 0) != child) {
		if (pinned)
			unlink(known);
		return -1;
	}
	if (pinned)
		unlink(known);
	if (WIFEXITED(status) && WEXITSTATUS(status) == 127)
		return -1;  /* no ssh client: the exec shell */
	return WIFEXITED(status) ? WEXITSTATUS(status) : 125;
}

/*
 * Does something answer on the machine's port 22 -- an sshd the image runs
 * of its own, which (ssh true) did not provision? Dial it and see whether
 * the core connects rather than refusing, within a short window: a real
 * sshd sends its banner at once, so a silent service or none reads as no
 * sshd and falls to the exec shell. Used only when there is no persistent
 * host key to go by.
 */
static int machine_sshd_open(const char *id)
{
	hull_wire      wire;
	hull_sexpr_doc body;
	struct pollfd  p;
	int            open = 0;

	if (connect_core(&wire, 0) != 0)
		return 0;
	send_request(&wire, "(4:dial", 1u, id, "(4:port2:22)");
	p.fd     = wire.fd;
	p.events = POLLIN;
	if (poll(&p, 1u, 2000) == 1 && hull_wire_read(&wire, &body) == HULL_WIRE_OK &&
	    !hull_msg_is(&body, "error"))
		open = 1;
	hull_wire_close(&wire);
	return open;
}

static int ssh_into(const char *id)
{
	const char *term = getenv("TERM");
	char       *login[] = { "/bin/sh", "-l" };
	char       *termed[] = { "/bin/sh", "-c", "TERM=\"$1\" exec /bin/sh -l", "sh", NULL };
	char        pub[4300];
	int         rc;

	if (!isatty(0)) {
		fprintf(stderr, "hull: ssh needs a terminal on stdin\n");
		return 125;
	}
	/* Prefer the machine's own sshd where one runs (section 8.1 item 2),
	 * found by a port-22 probe: pinned to its persistent host key where
	 * (ssh true) gave it one, else left to the user's own known_hosts. A
	 * login shell over the agent otherwise, or where there is no ssh
	 * client. The key alone is not enough: it outlives the run that made
	 * it, and a later run of the machine without (ssh true) has no sshd,
	 * where `hull ssh` met a refused port 22 rather than its shell. */
	{
		char arch[64];

		list_machines(id, arch, sizeof arch);
	}
	if (machine_sshd_open(id)) {
		if ((rc = ssh_client(id, machine_host_pub(id, pub, sizeof pub) ? pub : NULL)) >= 0)
			return rc;
	}
	/* A pc machine runs nothing of hull's: its serial port is the shell. */
	if (strcmp(listed_engine, "qemu-pc") == 0) {
		char *console[] = { "console" };

		return exec_in(id, console, 1, 1, 0u);
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
static int read_stats(const char *id, long long values[3], int guest)
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
	send_request(&wire, "(5:stats", 1u, id, guest ? "(5:guest4:true)" : NULL);
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
static int stats_of(const char *id, long over, int guest)
{
	long long       first[3] = { 0, 0, 0 };
	long long       second[3] = { 0, 0, 0 };
	struct timespec before;
	struct timespec after;
	struct timespec pause = { over, 0 };
	double          elapsed;
	int             status;

	if ((status = read_stats(id, first, guest)) != 0)
		return status;
	if (over == 0) {
		printf("cpu     %lld.%06lld s\nio      %lld bytes\nmemory  %lld bytes\n",
		       first[0] / 1000000, first[0] % 1000000, first[1], first[2]);
		return 0;
	}
	clock_gettime(CLOCK_MONOTONIC, &before);
	while (nanosleep(&pause, &pause) != 0 && errno == EINTR)
		;
	if ((status = read_stats(id, second, guest)) != 0)
		return status;
	clock_gettime(CLOCK_MONOTONIC, &after);
	elapsed = (double)(after.tv_sec - before.tv_sec) +
	          (double)(after.tv_nsec - before.tv_nsec) / 1e9;
	printf("cpu     %.1f %%\nio      %.0f bytes/s\nmemory  %lld bytes\n",
	       (double)(second[0] - first[0]) / 1e4 / elapsed,
	       (double)(second[1] - first[1]) / elapsed, second[2]);
	return 0;
}

/* --- segments ------------------------------------------------------------ */

/* The atom `text`, as a canonical s-expression writes it. */
static void msg_entry(hull_msg *m, const char *key, const char *value)
{
	hull_msg_raw(m, "(");
	hull_msg_text(m, key);
	if (value != NULL)
		hull_msg_text(m, value);
	hull_msg_raw(m, ")");
}

/* netem's names, as `KEY=VALUE` words, into an impairment's entries,
 * sorted as a canonical form's are. 0, or 2 having said why. */
/* A segment fault's KEY=VALUE words into `(fault ...)` entries, in key
 * order; match and set take OFFSET:HEX, carried as a string the core
 * splits. 0, or 2 having said why. */
static int fault_words(int argc, char **argv, hull_msg *m)
{
	static const char *const keys[] = { "copies", "drop", "every", "flip", "match", "replay",
	                                    "rewrite", "set" };
	const char *given[sizeof keys / sizeof keys[0]] = { NULL };

	for (int i = 0; i < argc; i++) {
		const char *equals = strchr(argv[i], '=');
		size_t      length = equals != NULL ? (size_t)(equals - argv[i]) : strlen(argv[i]);
		unsigned    k;

		for (k = 0u; k < sizeof keys / sizeof keys[0]; k++)
			if (strlen(keys[k]) == length && strncmp(keys[k], argv[i], length) == 0)
				break;
		if (k == sizeof keys / sizeof keys[0] || equals == NULL) {
			fprintf(stderr, "hull: segment: %s is not a fault: match and set with OFFSET:HEX, "
			        "and drop, rewrite, flip, replay, copies and every with =VALUE\n", argv[i]);
			return 2;
		}
		given[k] = equals + 1;
	}
	for (unsigned k = 0u; k < sizeof keys / sizeof keys[0]; k++)
		if (given[k] != NULL)
			msg_entry(m, keys[k], given[k]);
	return 0;
}

static int impairment_words(int argc, char **argv, hull_msg *m)
{
	static const char *const keys[] = { "correlation", "corrupt", "delay", "distribution", "down",
	                                    "duplicate", "gemodel", "jitter", "limit", "loss", "mtu",
	                                    "rate", "reorder", "seed" };
	const char *given[sizeof keys / sizeof keys[0]] = { NULL };

	for (int i = 0; i < argc; i++) {
		const char *equals = strchr(argv[i], '=');
		size_t      length = equals != NULL ? (size_t)(equals - argv[i]) : strlen(argv[i]);
		unsigned    k;

		for (k = 0u; k < sizeof keys / sizeof keys[0]; k++)
			if (strlen(keys[k]) == length && strncmp(keys[k], argv[i], length) == 0)
				break;
		if (k == sizeof keys / sizeof keys[0] || (equals == NULL) != (strcmp(keys[k], "down") == 0)) {
			fprintf(stderr, "hull: segment: %s is not an impairment: down, or one of loss, "
			        "gemodel, delay, jitter, distribution, correlation, reorder, duplicate, "
			        "corrupt, rate, limit, mtu and seed with =VALUE\n", argv[i]);
			return 2;
		}
		given[k] = equals != NULL ? equals + 1 : "true";
	}
	for (unsigned k = 0u; k < sizeof keys / sizeof keys[0]; k++) {
		char        values[4][32];
		const char *at = given[k];
		unsigned    count = 0u;

		if (at == NULL)
			continue;
		/* gemodel=p[,r[,1-h[,1-k]]], with netem's defaults: r is 1-p,
		 * 1-h is 1 and 1-k is 0. */
		if (strcmp(keys[k], "gemodel") != 0) {
			msg_entry(m, keys[k], at);
			continue;
		}
		while (count < 4u && *at != '\0') {
			size_t length = strcspn(at, ",");

			if (length == 0u || length >= sizeof values[0])
				break;
			memcpy(values[count], at, length);
			values[count++][length] = '\0';
			at += length + (at[length] == ',' ? 1u : 0u);
		}
		if (count == 0u || *at != '\0') {
			fprintf(stderr, "hull: segment: gemodel is p[,r[,1-h[,1-k]]]\n");
			return 2;
		}
		if (count < 2u)
			snprintf(values[1], sizeof values[1], "%.17g", 1.0 - strtod(values[0], NULL));
		if (count < 3u)
			snprintf(values[2], sizeof values[2], "1");
		if (count < 4u)
			snprintf(values[3], sizeof values[3], "0");
		/* As the spec writes it, its parts sorted: bad_loss, good_loss,
		 * p and r. */
		hull_msg_raw(m, "(7:gemodel");
		msg_entry(m, "bad_loss", values[2]);
		msg_entry(m, "good_loss", values[3]);
		msg_entry(m, "p", values[0]);
		msg_entry(m, "r", values[1]);
		hull_msg_raw(m, ")");
	}
	return 0;
}

/* The core's `done` or `error` for a request on channel 1: 0, or 1 said. */
static int answered(hull_wire *wire)
{
	hull_sexpr_doc  body;
	hull_sexpr_form value;
	char            reason[512] = "an error";

	if (hull_wire_read(wire, &body) != HULL_WIRE_OK) {
		fprintf(stderr, "hull: the core went away\n");
		return 1;
	}
	if (hull_msg_is(&body, "done"))
		return 0;
	if (hull_msg_find(&body, "reason", &value))
		hull_msg_string(&body, &value, reason, sizeof reason);
	fprintf(stderr, "hull: %s\n", reason);
	return 1;
}

/* One way of a port as `ports` gives it: its counts, named, in the order
 * printed. */
static void print_way(const hull_sexpr_doc *body, const hull_sexpr_form *way, const char *machine,
                      const char *direction)
{
	static const char *const counts[] = { "frames", "bytes", "passed", "passed_bytes", "lost",
	                                      "too_big", "overflow", "full", "duplicated",
	                                      "corrupted", "matched", "tampered", "replayed", "injected" };
	hull_sexpr_form item;
	int             more;

	printf("%-20s %-3s", machine, direction);
	for (unsigned k = 0u; k < sizeof counts / sizeof counts[0]; k++) {
		long long n = 0;

		hull_sexpr_child(body, way, &item);
		for (more = hull_sexpr_next(body, &item, &item); more;
		     more = hull_sexpr_next(body, &item, &item)) {
			hull_sexpr_form name;
			hull_sexpr_form value;

			if (item.kind == HULL_SEXPR_LIST && hull_sexpr_child(body, &item, &name) &&
			    hull_sexpr_is(body, &name, counts[k]) && hull_sexpr_next(body, &name, &value))
				hull_msg_long(body, &value, &n);
		}
		printf(" %s=%lld", counts[k], n);
	}
	printf("\n");
}

/* `hull segment NAME`: each port's counts, a line each way. `hull segment
 * NAME impair MACHINE|all in|out|both [KEY=VALUE|down]...`: that impairment,
 * in netem's terms, for those ports' directions, replacing theirs; with no
 * words, a wire. */
static int segment_command(int argc, char **argv)
{
	hull_wire       wire;
	hull_sexpr_doc  body;
	hull_sexpr_form message;
	hull_sexpr_form item;
	int             status;
	int             more;

	int tamper = argc > 3 && strcmp(argv[3], "tamper") == 0;
	int inject = argc > 3 && strcmp(argv[3], "inject") == 0;
	int capture = argc > 3 && strcmp(argv[3], "capture") == 0;

	/* capture NAME MACHINE|all in|out FILE [frames=N], or capture stop: the
	 * core writes the file, so it is named to it whole. */
	if (capture) {
		hull_msg m = { 0 };
		char     whole[4096];
		int      stop = argc == 5 && strcmp(argv[4], "stop") == 0;

		if (!stop && (argc < 7 || argc > 8 ||
		              (strcmp(argv[5], "in") != 0 && strcmp(argv[5], "out") != 0) ||
		              (argc == 8 && strncmp(argv[7], "frames=", 7u) != 0))) {
			usage(stderr);
			return 2;
		}
		if (!stop) {
			char here[4096];

			if (argv[6][0] == '/')
				snprintf(whole, sizeof whole, "%s", argv[6]);
			else if (getcwd(here, sizeof here) == NULL ||
			         snprintf(whole, sizeof whole, "%s/%s", here, argv[6]) >= (int)sizeof whole) {
				fprintf(stderr, "hull: segment: %s cannot be named whole\n", argv[6]);
				return 2;
			}
		}
		if ((status = connect_core(&wire, 0)) != 0) {
			if (status == 1)
				fprintf(stderr, "hull: no core is running, so no machine is on a segment\n");
			return status == 1 ? 1 : 2;
		}
		hull_msg_raw(&m, "(7:capture");
		hull_msg_channel(&m, 1u);
		if (stop) {
			msg_entry(&m, "frames", "0");
		} else {
			if (argc == 8)
				msg_entry(&m, "frames", argv[7] + 7);
			msg_entry(&m, "machine", strcmp(argv[4], "all") == 0 ? "*" : argv[4]);
		}
		msg_entry(&m, "name", argv[2]);
		if (!stop) {
			msg_entry(&m, "path", whole);
			msg_entry(&m, "way", argv[5]);
		}
		hull_msg_raw(&m, ")");
		status = m.failed || hull_wire_send(wire.fd, &m) != 0 ? 1 : answered(&wire);
		hull_msg_free(&m);
		hull_wire_close(&wire);
		return status;
	}

	/* inject NAME MACHINE|all KEY=VALUE...: the switch's own frames, no
	 * direction. Sent on its own, outside the impair/tamper shape. */
	if (inject) {
		hull_msg m = { 0 };
		int      d;

		if (argc < 5) {
			usage(stderr);
			return 2;
		}
		if ((status = connect_core(&wire, 0)) != 0) {
			if (status == 1)
				fprintf(stderr, "hull: no core is running, so no machine is on a segment\n");
			return status == 1 ? 1 : 2;
		}
		hull_msg_raw(&m, "(6:inject");
		hull_msg_channel(&m, 1u);
		for (d = 5; d < argc; d++) {
			const char *equals = strchr(argv[d], '=');

			if (equals == NULL) {
				fprintf(stderr, "hull: segment: %s is KEY=VALUE\n", argv[d]);
				hull_msg_free(&m);
				hull_wire_close(&wire);
				return 2;
			}
			*(char *)equals = '\0';
			msg_entry(&m, argv[d], equals + 1);
			*(char *)equals = '=';
		}
		msg_entry(&m, "machine", strcmp(argv[4], "all") == 0 ? "*" : argv[4]);
		msg_entry(&m, "name", argv[2]);
		hull_msg_raw(&m, ")");
		status = m.failed || hull_wire_send(wire.fd, &m) != 0 ? 1 : answered(&wire);
		hull_msg_free(&m);
		hull_wire_close(&wire);
		return status;
	}

	if (argc < 3 || (argc > 3 && ((strcmp(argv[3], "impair") != 0 && !tamper) || argc < 6))) {
		usage(stderr);
		return 2;
	}
	if (argc > 3 && strcmp(argv[5], "in") != 0 && strcmp(argv[5], "out") != 0 &&
	    strcmp(argv[5], "both") != 0) {
		fprintf(stderr, "hull: segment: a direction is in, out or both\n");
		return 2;
	}
	if ((status = connect_core(&wire, 0)) != 0) {
		if (status == 1)
			fprintf(stderr, "hull: no core is running, so no machine is on a segment\n");
		return status == 1 ? 1 : 2;
	}
	if (argc > 3) {
		static const char *const directions[] = { "in", "out" };

		for (unsigned d = 0u; d < 2u && status == 0; d++) {
			hull_msg m = { 0 };

			if (strcmp(argv[5], "both") != 0 && strcmp(argv[5], directions[d]) != 0)
				continue;
			hull_msg_raw(&m, tamper ? "(6:tamper" : "(6:impair");
			hull_msg_channel(&m, 1u);
			msg_entry(&m, "direction", directions[d]);
			hull_msg_raw(&m, tamper ? "(5:fault" : "(10:impairment");
			status = tamper ? fault_words(argc - 6, argv + 6, &m)
			                : impairment_words(argc - 6, argv + 6, &m);
			hull_msg_raw(&m, ")");
			msg_entry(&m, "machine", strcmp(argv[4], "all") == 0 ? "*" : argv[4]);
			msg_entry(&m, "name", argv[2]);
			hull_msg_raw(&m, ")");
			if (status == 0 && (m.failed || hull_wire_send(wire.fd, &m) != 0))
				status = 1;
			hull_msg_free(&m);
			if (status == 0)
				status = answered(&wire);
		}
		hull_wire_close(&wire);
		return status;
	}
	{
		hull_msg m = { 0 };

		hull_msg_raw(&m, "(7:segment");
		hull_msg_channel(&m, 1u);
		msg_entry(&m, "name", argv[2]);
		hull_msg_raw(&m, ")");
		status = m.failed || hull_wire_send(wire.fd, &m) != 0;
		hull_msg_free(&m);
	}
	if (status != 0 || hull_wire_read(&wire, &body) != HULL_WIRE_OK) {
		hull_wire_close(&wire);
		fprintf(stderr, "hull: the core went away\n");
		return 1;
	}
	if (!hull_msg_is(&body, "ports")) {
		hull_sexpr_form value;
		char            reason[512] = "an error";

		if (hull_msg_find(&body, "reason", &value))
			hull_msg_string(&body, &value, reason, sizeof reason);
		fprintf(stderr, "hull: %s\n", reason);
		hull_wire_close(&wire);
		return 1;
	}
	hull_sexpr_first(&body, &message);
	hull_sexpr_child(&body, &message, &item);
	for (more = hull_sexpr_next(&body, &item, &item); more;
	     more = hull_sexpr_next(&body, &item, &item)) {
		hull_sexpr_form name;
		hull_sexpr_form field;
		hull_sexpr_form ways[2];
		char            machine[80] = "?";
		int             found = 0;
		int             gone = 0;

		if (item.kind != HULL_SEXPR_LIST || !hull_sexpr_child(&body, &item, &name) ||
		    !hull_sexpr_is(&body, &name, "port"))
			continue;
		for (int go = hull_sexpr_next(&body, &name, &field); go;
		     go = hull_sexpr_next(&body, &field, &field)) {
			hull_sexpr_form key;
			hull_sexpr_form value;

			if (field.kind != HULL_SEXPR_LIST || !hull_sexpr_child(&body, &field, &key))
				continue;
			if (hull_sexpr_is(&body, &key, "machine") && hull_sexpr_next(&body, &key, &value))
				hull_msg_string(&body, &value, machine, sizeof machine);
			else if (hull_sexpr_is(&body, &key, "gone"))
				gone = 1;
			else if (hull_sexpr_is(&body, &key, "out"))
				ways[0] = field, found |= 1;
			else if (hull_sexpr_is(&body, &key, "in"))
				ways[1] = field, found |= 2;
		}
		/* A machine that has left the segment, its counts as it left. */
		if (gone)
			strncat(machine, " (gone)", sizeof machine - strlen(machine) - 1u);
		if (found & 1)
			print_way(&body, &ways[0], machine, "out");
		if (found & 2)
			print_way(&body, &ways[1], machine, "in");
	}
	hull_wire_close(&wire);
	return 0;
}

/* `hull pause ID` and `hull unpause ID`: every process of the machine
 * stopped where it is, or let go on. */
static int pause_of(const char *id, int paused)
{
	hull_wire wire;
	int       status = connect_core(&wire, 0);

	if (status != 0) {
		if (status == 1)
			fprintf(stderr, "hull: no core is running, so no machine is\n");
		return status == 1 ? 1 : 2;
	}
	send_request(&wire, paused ? "(5:pause" : "(7:unpause", 1u, id, NULL);
	status = answered(&wire);
	hull_wire_close(&wire);
	return status;
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
		/* -1, unasked: the machine's own grace. */
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
			if (e->accept != NULL && strcmp(e->accept, HULL_MATRIX_ROOT) == 0)
				printf(" (root mode: --accept root:HASH, hull plan --root-hash)");
			else if (e->accept != NULL)
				printf(" (--accept %s)", e->accept);
			printf("\n");
		}
	}
	/* Root mode is there only where its helper is (section 7). */
	{
		const char *helper = getenv("HULL_ROOT_HELPER") != NULL ? getenv("HULL_ROOT_HELPER") :
		                     HULL_ROOT_HELPER_PATH;
		struct stat installed;

		if (stat(helper, &installed) == 0 && (installed.st_mode & S_ISUID) != 0 &&
		    installed.st_uid == 0)
			printf("root mode: %s, setuid root\n", helper);
		else if (access(helper, X_OK) == 0)
			printf("root mode: %s, not setuid root, so it grants only what the user "
			       "could do already\n", helper);
		else
			printf("root mode: not installed (make install-root, as root)\n");
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

/* `hull dial`: the far end of `-H`, stdin and stdout joined to this
 * user's core, one started as for any frontend. */
static int dial(void)
{
	int fd = dial_local();

	if (fd < 0 && getenv("XDG_RUNTIME_DIR") != NULL) {
		start_core();
		for (int tries = 0; fd < 0 && tries < 100; tries++) {
			struct timespec pause = { 0, 50000000L };

			nanosleep(&pause, NULL);
			fd = dial_local();
		}
	}
	if (fd < 0) {
		fprintf(stderr, "hull: dial: no core here, and none started\n");
		return 125;
	}
	return hull_dial_pump(fd);
}

/* Whether `command` is a core's to answer, so that -H can mean anything:
 * the store and the rest are this host's, and doing them here under a
 * flag naming another would say what is not so. */
static int remote_command(const char *command)
{
	static const char *const commands[] = {
		"run", "up", "plan", "check", "ps", "stop", "kill", "logs", "attach", "exec", "stats",
		"pause", "unpause", "segment", "ssh", "proxy-ssh", "lease", "pool", "reserve", "release",
		NULL
	};

	for (const char *const *c = commands; *c != NULL; c++)
		if (strcmp(*c, command) == 0)
			return 1;
	return 0;
}

/* `hull import [--build-arg NAME=VALUE]... [--target STAGE] DOCKERFILE`. */
static int import_flags(int argc, char **argv)
{
	static hull_build_arg args[HULL_MAX_BUILD_ARGS];
	unsigned              count = 0u;
	const char           *target = NULL;
	int                   i;

	for (i = 2; i + 1 < argc; i += 2) {
		char *eq = strchr(argv[i + 1], '=');

		if (strcmp(argv[i], "--target") == 0) {
			target = argv[i + 1];
		} else if (strcmp(argv[i], "--build-arg") == 0 && eq != NULL &&
		           count < HULL_MAX_BUILD_ARGS) {
			snprintf(args[count].name, sizeof args[count].name, "%.*s",
			         (int)(eq - argv[i + 1]), argv[i + 1]);
			snprintf(args[count].value, sizeof args[count].value, "%s", eq + 1);
			count++;
		} else {
			break;
		}
	}
	if (i != argc - 1) {
		usage(stderr);
		return 2;
	}
	return hull_import_build(argv[i], args, count, target);
}

int main(int argc, char **argv)
{
	hull_registry registry;
	options       o;
	const char   *command;
	const char   *reason = NULL;
	const char   *name   = NULL;
	int           check  = 0;

	memset(&o, 0, sizeof o);
	o.wait = -1;
	/* A service account's job, as a CI server runs one, has no login and
	 * so no XDG_RUNTIME_DIR: one is chosen, once, here, for the core and
	 * the engines it starts, and nothing is said of it unless it fails. */
	{
		const char *given = getenv("XDG_RUNTIME_DIR");

		if (given == NULL || given[0] == '\0') {
			char chosen[4096];

			if (hull_runtime_choose(getuid(), hull_runtime_bases, 3u, chosen, sizeof chosen,
			                        runtime_why, sizeof runtime_why) == 0)
				setenv("XDG_RUNTIME_DIR", chosen, 1);
			else
				unsetenv("XDG_RUNTIME_DIR");
		}
	}
	o.file   = "hull.conf";
	o.stream = stdout;
	/* -H URL leads the command, and HULL_HOST says the same, so that a
	 * ProxyCommand this hull writes reaches the same core. */
	{
		const char *url = getenv("HULL_HOST");
		char        why[300];

		if (argc >= 3 && strcmp(argv[1], "-H") == 0) {
			url = argv[2];
			argv[2] = argv[0];
			argv += 2;
			argc -= 2;
		}
		if (url != NULL && url[0] != '\0') {
			if (hull_dial_parse(url, &remote, why, sizeof why) != 0) {
				fprintf(stderr, "hull: %s\n", why);
				return 2;
			}
			remote_given = 1;
			setenv("HULL_HOST", url, 1);
			if (argc >= 2 && argv[1][0] != '-' && !remote_command(argv[1])) {
				fprintf(stderr, "hull: %s works on this host, whatever -H says: run it there\n",
				        argv[1]);
				return 2;
			}
		}
	}
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

	if (strcmp(command, "doctor") == 0 && argc == 2)
		return hull_doctor(runtime_why);
	hull_registry_init(&registry, host_arch());
	if (strcmp(command, "dial") == 0 && argc == 2)
		return dial();
	if (strcmp(command, "core") == 0 && argc == 2)
		return hull_core_run(host_arch());
	if (strcmp(command, "ps") == 0 && argc == 2)
		return list_machines(NULL, NULL, 0u);
	if (strcmp(command, "gc") == 0 && argc == 2)
		return collect();
	if (strcmp(command, "commit") == 0 && argc == 4)
		return commit(argv[2], argv[3], NULL, 0);
	if (strcmp(command, "commit") == 0 && argc == 5 && strcmp(argv[2], "--tar") == 0)
		return commit(argv[3], argv[4], NULL, 1);
	if (strcmp(command, "commit") == 0 && argc == 6 && strcmp(argv[2], "--arch") == 0)
		return commit(argv[4], argv[5], argv[3], 0);
	if (strcmp(command, "materialize") == 0 && argc == 4)
		return materialize(argv[2], argv[3]);
	if (strcmp(command, "kernel") == 0 && (argc == 3 || (argc == 5 && strcmp(argv[3], "--arch") == 0)) &&
	    strcmp(argv[2], "fetch") == 0) {
		char why[4400] = "";

		if (hull_kernel_fetch_arch(argc == 5 ? argv[4] : "amd64", why, sizeof why) != 0) {
			fprintf(stderr, "hull: kernel fetch: %s\n", why);
			return 1;
		}
		return 0;
	}
	if (strcmp(command, "vagrant") == 0 && argc == 6 && strcmp(argv[2], "box") == 0 &&
	    strcmp(argv[3], "add") == 0)
		return vagrant_box_add(argv[4], argv[5]);
	if (strcmp(command, "virtualbox") == 0 && argc == 3 && strcmp(argv[2], "fetch") == 0) {
		char why[4400] = "";

		if (hull_vbox_fetch(why, sizeof why) != 0) {
			fprintf(stderr, "hull: virtualbox fetch: %s\n", why);
			return 1;
		}
		return 0;
	}
	if (strcmp(command, "virtualbox") == 0 && argc >= 10 && argc % 2 == 0 &&
	    strcmp(argv[2], "host") == 0)
		return hull_vbox_host(argv[3], argv[4], argv[5], argv[6], argv[7], argv[8], argv[9],
		                      argv + 10, (unsigned)(argc - 10) / 2u);
	if (strcmp(command, "sshd") == 0 && argc == 3 && strcmp(argv[2], "fetch") == 0) {
		char why[4400] = "";

		if (hull_sshd_fetch(why, sizeof why) != 0) {
			fprintf(stderr, "hull: sshd fetch: %s\n", why);
			return 1;
		}
		return 0;
	}
	if (strcmp(command, "export") == 0 && (argc == 6 || argc == 7) &&
	    strcmp(argv[2], "--bootable") == 0 && strcmp(argv[3], "-f") == 0)
		return export_machine(argv[4], argc == 7 ? argv[5] : NULL, argv[argc - 1]);
	if (strcmp(command, "export") == 0 && argc == 5 && strcmp(argv[2], "--bootable") == 0)
		return export_bootable(argv[3], argv[4], "/sbin/init", NULL);
	if (strcmp(command, "export") == 0 && argc == 7 && strcmp(argv[2], "--bootable") == 0 &&
	    strcmp(argv[5], "--init") == 0)
		return export_bootable(argv[3], argv[4], argv[6], NULL);
	if (strcmp(command, "export") == 0 && argc == 7 && strcmp(argv[2], "--bootable") == 0 &&
	    strcmp(argv[5], "--host") == 0)
		return export_bootable(argv[3], argv[4], NULL, argv[6]);
	if (strcmp(command, "pack") == 0 && argc == 4)
		return pack(argv[2], argv[3]);
	if (strcmp(command, "unpack") == 0 && argc == 4)
		return unpack(argv[2], argv[3]);
	if (strcmp(command, "reserve") == 0 && argc >= 3)
		return reserve(argc, argv);
	if (strcmp(command, "release") == 0 && argc == 3)
		return release(argv[2]);
	if (strcmp(command, "lease") == 0 && argc == 3 && strcmp(argv[2], "ls") == 0)
		return list_of(0);
	if (strcmp(command, "pool") == 0 && argc == 3 && strcmp(argv[2], "ls") == 0)
		return list_of(1);
	if (strcmp(command, "images") == 0 && argc == 2)
		return images();
	if (strcmp(command, "diff") == 0 && argc == 4)
		return diff(argv[2], argv[3]);
	if (strcmp(command, "tag") == 0 && argc == 4 && strcmp(argv[2], "-d") == 0)
		return untag(argv[3]);
	if (strcmp(command, "tag") == 0 && argc == 4)
		return tag(argv[2], argv[3]);
	if (strcmp(command, "push") == 0 && argc == 4)
		return push(argv[2], argv[3]);
	if (strcmp(command, "pull") == 0 && argc >= 5 && strcmp(argv[2], "--arch") == 0 &&
	    argc <= 6)
		return pull(argv[4], argc == 6 ? argv[5] : argv[4], argv[3]);
	if (strcmp(command, "load") == 0 && (argc == 3 || argc == 4))
		return load(argv[2], argc == 4 ? argv[3] : NULL);
	if (strcmp(command, "pull") == 0 && (argc == 3 || argc == 4))
		return pull(argv[2], argc == 4 ? argv[3] : argv[2], NULL);
	if (strcmp(command, "manifest") == 0 && argc == 4 && strcmp(argv[2], "show") == 0)
		return show_manifest(argv[3]);
	if (strcmp(command, "manifest") == 0 && argc == 5 && strcmp(argv[2], "composefs") == 0)
		return composefs_manifest(argv[3], argv[4]);
	if (strcmp(command, "manifest") == 0 && (argc == 3 || argc == 4) &&
	    strcmp(argv[2], "compile") == 0)
		return compile_manifest(argc == 4 ? argv[3] : NULL);
	if (strcmp(command, "media") == 0 && argc == 4 && strcmp(argv[2], "verify") == 0)
		return media_verify(argv[3]);
	if (strcmp(command, "media") == 0 && (argc == 5 || argc == 6) && strcmp(argv[2], "change") == 0)
		return media_change(argv[3], argv[4], argc == 6 ? argv[5] : NULL);
	if (strcmp(command, "media") == 0 && argc == 5 && strcmp(argv[2], "export") == 0)
		return media_export(argv[3], argv[4]);
	if ((strcmp(command, "media") == 0 && argc == 5 && strcmp(argv[2], "import") == 0) ||
	    (strcmp(command, "import") == 0 && argc == 4))
		return media_import(argv[argc - 2], argv[argc - 1]);
	if (strcmp(command, "import") == 0 && argc >= 5 && strncmp(argv[2], "--", 2u) == 0)
		return import_flags(argc, argv);
	if (strcmp(command, "import") == 0 && argc == 3)
		return hull_import(argv[2]);
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
		long        timeout     = -1;

		for (int i = 2; i < argc; i++) {
			if (strcmp(command, "logs") == 0 && strcmp(argv[i], "-f") == 0)
				follow = 1;
			else if (strcmp(command, "stop") == 0 && strcmp(argv[i], "--timeout") == 0 &&
			         i + 1 < argc)
				timeout = labs(strtol(argv[++i], NULL, 10)) * 1000L;
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
		return about(command, id, follow, timeout, signal_name);
	}

	if (strcmp(command, "segment") == 0)
		return segment_command(argc, argv);
	if ((strcmp(command, "pause") == 0 || strcmp(command, "unpause") == 0) && argc == 3 &&
	    argv[2][0] != '-')
		return pause_of(argv[2], command[0] == 'p');
	if (strcmp(command, "stats") == 0) {
		const char *id = NULL;
		int         guest = 0;
		long        over = 0;

		for (int i = 2; i < argc; i++) {
			if (strcmp(argv[i], "--guest") == 0) {
				guest = 1;
			} else if (strcmp(argv[i], "--over") == 0 && i + 1 < argc) {
				char *end = NULL;

				over = strtol(argv[++i], &end, 10);
				if (end == argv[i] || *end != '\0' || over < 1 || over > 3600) {
					fprintf(stderr, "hull: stats --over takes 1 to 3600 seconds\n");
					return 2;
				}
			} else if (argv[i][0] != '-' && id == NULL) {
				id = argv[i];
			} else {
				usage(stderr);
				return 2;
			}
		}
		if (id == NULL) {
			usage(stderr);
			return 2;
		}
		return stats_of(id, over, guest);
	}
	if (strcmp(command, "ssh") == 0 && argc == 3 && argv[2][0] != '-')
		return ssh_into(argv[2]);

	/* Planning needs the engines' declarations, not their operations,
	 * which run in the core: nothing here reports to a core. */
	if (hull_registry_add(&registry, hull_engine_docker(NULL), &reason) != HULL_REGISTRY_OK ||
	    hull_registry_add(&registry, hull_engine_qemu(NULL), &reason) != HULL_REGISTRY_OK ||
	    hull_registry_add(&registry, hull_engine_virtualbox(NULL), &reason) != HULL_REGISTRY_OK ||
	    hull_registry_add(&registry, hull_engine_chroot(NULL), &reason) != HULL_REGISTRY_OK ||
	    hull_registry_add(&registry, hull_engine_dosbox(NULL), &reason) != HULL_REGISTRY_OK ||
	    hull_registry_add(&registry, hull_engine_dosemu(NULL), &reason) != HULL_REGISTRY_OK ||
	    hull_registry_add(&registry, hull_engine_qemu_pc(NULL), &reason) != HULL_REGISTRY_OK ||
	    hull_registry_set_default(&registry, "ns", "chroot") != HULL_REGISTRY_OK) {
		fprintf(stderr, "hull: a built-in engine is refused: %s\n", reason);
		return 2;
	}

	if (strcmp(command, "engines") == 0 && argc == 2) {
		list_engines(&registry);
		return 0;
	}
	/* `hull up` is `hull run` of the compose file beside it, section 9's
	 * compose dialect, or of the file -f names. */
	if (strcmp(command, "up") == 0) {
		command = "run";
		o.file  = NULL;
	}
	if (strcmp(command, "plan") != 0 && strcmp(command, "check") != 0 &&
	    strcmp(command, "run") != 0 && strcmp(command, "build") != 0 &&
	    strcmp(command, "systemd") != 0) {
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
		} else if (strcmp(arg, "--profile") == 0 && i + 1 < argc && strcmp(argv[1], "up") == 0) {
			/* Compose's own variable, which the dialect reads. */
			const char *had = getenv("COMPOSE_PROFILES");
			char        list[512];

			snprintf(list, sizeof list, "%s%s%s", had != NULL ? had : "",
			         had != NULL ? "," : "", argv[++i]);
			setenv("COMPOSE_PROFILES", list, 1);
		} else if (strcmp(arg, "--explain") == 0 && strcmp(command, "plan") == 0) {
			o.explain = 1;
		} else if (strcmp(arg, "--resolved") == 0 && strcmp(command, "plan") == 0) {
			o.resolved = 1;
		} else if (strcmp(arg, "--root-hash") == 0 && strcmp(command, "plan") == 0) {
			o.root_hash = 1;
		} else if (strcmp(arg, "--wait") == 0 && i + 1 < argc && strcmp(command, "run") == 0) {
			if ((o.wait = seconds_of(argv[++i])) <= 0) {
				fprintf(stderr, "hull: --wait is a duration: 30s, 5m, 2h\n");
				return 2;
			}
		} else if (strcmp(arg, "--no-wait") == 0 && strcmp(command, "run") == 0) {
			o.wait = 0;
		} else if (strcmp(arg, "--lease") == 0 && i + 1 < argc && strcmp(command, "run") == 0) {
			o.lease = argv[++i];
		} else if (strcmp(arg, "--check") == 0 && strcmp(command, "build") == 0) {
			check = 1;
		} else if ((strcmp(command, "run") == 0 || strcmp(command, "build") == 0 ||
		            strcmp(command, "systemd") == 0) && arg[0] != '-' && name == NULL) {
			name = arg;
		} else {
			usage(stderr);
			return 2;
		}
	}
	/* Run as root, hull is root already: what root mode would honour is
	 * honoured without a listing (section 7). */
	if (geteuid() == 0 && o.token_count < MAX_TOKENS)
		o.tokens[o.token_count++] = HULL_MATRIX_ROOT_SELF;
	if (strcmp(command, "check") != 0 && o.backend_count > 1u) {
		fprintf(stderr, "hull: %s takes one backend; check takes several\n", command);
		return strcmp(command, "run") == 0 ? 125 : 2;
	}
	if (o.file == NULL && (o.file = compose_file()) == NULL) {
		fprintf(stderr, "hull: up: no compose.yaml, compose.yml, docker-compose.yaml, "
		        "docker-compose.yml or Vagrantfile here; -f names one\n");
		return 125;
	}
	/* A file read here, its relative paths meaning the far directory's:
	 * the same directory rsync'd there, section 4.10. */
	o.base = remote_given && remote.dir[0] != '\0' ? strdup(remote.dir) : base_of(o.file);
	if (o.base == NULL) {
		fprintf(stderr, "%s: %s\n", o.file, strerror(errno));
		return strcmp(command, "run") == 0 ? 125 : 2;
	}
	if (strcmp(command, "run") == 0) {
		o.stream = stderr;
		if (hull_import_is_compose(o.file) && compose_builds(&o, &registry) != 0)
			return 125;
		if (hull_vagrant_is_file(o.file) && vagrant_builds(&o, &registry) != 0)
			return 125;
		return run(&o, &registry, name);
	}
	if (strcmp(command, "systemd") == 0)
		return systemd_unit(&o, name);
	if (strcmp(command, "build") == 0) {
		if (name == NULL) {
			fprintf(stderr, "hull: build names the ref it makes: hull build [-f FILE] REF\n");
			return 2;
		}
		o.stream = stderr;
		return build_file(&o, &registry, name, check);
	}
	if (strcmp(command, "check") == 0 && o.backend_count == 0u) {
		fprintf(stderr, "hull: check needs --backend, the backends to hold the spec to\n");
		return 2;
	}
	return resolve_all(&o, &registry);
}
