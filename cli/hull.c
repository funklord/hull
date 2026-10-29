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
#include "import.h"
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
	      "       hull commit [--arch A] DIR REF | hull commit --tar FILE REF\n"
	      "       hull materialize REF DIR | hull images\n"
	      "       hull diff REF REF | hull tag REF NEW | hull tag -d REF\n"
	      "       hull manifest show REF | hull manifest compile [REF]\n"
	      "       hull pull IMAGE [REF] | hull push REF IMAGE\n"
	      "       hull import DOCKERFILE\n"
	      "       hull core\n"
	      "       hull engines | hull doctor\n"
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
	if (record_find(&doc, &runtime, "user", &found)) {
		hull_sexpr_form head;
		hull_sexpr_form user;

		hull_sexpr_child(&doc, &found, &head);
		hull_sexpr_next(&doc, &head, &user);
		if (!hull_sexpr_is(&doc, &user, "root") && !hull_sexpr_is(&doc, &user, "0") &&
		    !hull_sexpr_is(&doc, &user, "0:0"))
			fprintf(stderr, "%s: %s: the image runs as %.*s, which is not taken: the "
			        "machine runs as its root\n", file, name, (int)user.content_length,
			        (const char *)doc.data + user.content);
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
static int store_sources(const char *file, uint8_t *data, size_t *length, int create)
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
		add_text(&source, "\"dir:");
		add_text(&source, path);
		add_text(&source, "\")");
		if (!has_mutable)
			add_text(&source, " (mutable overlay)");
		/* A stored image's non-root owners, which the checkout flattened to
		 * the user's, given back when the machine starts: its manifest
		 * holds them, hull_store_owners lists each as uid, gid and path, and
		 * the engine chowns them in the overlay -- chroot's namespace one,
		 * qemu's host-side. Docker keeps an image's own owners and declares
		 * the key rejected, so it is given none; and a plan, which makes no
		 * checkout, names none. The list is a sibling of the checkout,
		 * written only on a real run and only where the manifest gives an
		 * explicit owner. */
		if (create) {
			char            backend[64] = "";
			hull_sexpr_form be_entry;
			hull_sexpr_form be_head;
			hull_sexpr_form be_value;

			if (hull_spec_find(&spec, &machines[i], "backend", &be_entry) &&
			    hull_sexpr_child(&spec.doc, &be_entry, &be_head) &&
			    hull_sexpr_next(&spec.doc, &be_head, &be_value) &&
			    be_value.content_length < sizeof backend) {
				memcpy(backend, spec.doc.data + be_value.content, be_value.content_length);
				backend[be_value.content_length] = '\0';
			}
			if (strcmp(backend, "docker") != 0) {
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
	hull_msg_raw(&hello, "(5:hello(2:ch1:0)(5:proto1:1))");
	if (hull_wire_send(fd, &hello) != 0 || hull_wire_read(wire, &body) != HULL_WIRE_OK ||
	    !hull_msg_is(&body, "hello")) {
		fprintf(stderr, "hull: the core did not answer hello\n");
		hull_msg_free(&hello);
		hull_wire_close(wire);
		return -1;
	}
	hull_msg_free(&hello);
	/* A core left running from an older build serves its old engine, where
	 * this frontend plans against the new one, so a feature this build adds
	 * is planned and then silently does nothing -- which cost a day's
	 * misdiagnosis once. The core reports the commit it was built from; warn
	 * when it is not this build's. Both unknown (no git at build) cannot be
	 * told apart, so say nothing then. */
	{
		hull_sexpr_form top;
		char           *core_commit;

		if (hull_sexpr_first(&body, &top) &&
		    (core_commit = hull_access_value(&body, &top, "core_version")) != NULL) {
			if (strcmp(core_commit, "unknown") != 0 && strcmp(HULL_COMMIT, "unknown") != 0 &&
			    strcmp(core_commit, HULL_COMMIT) != 0)
				fprintf(stderr,
				        "hull: warning: the running core was built from %s, but this is %s; a "
				        "machine planned here runs on the core's older engine, so a newer "
				        "feature may do nothing until the core is restarted -- it exits on its "
				        "own once its machines are gone and it has idled, or end its `hull core` "
				        "process, and the next command starts this build\n",
				        core_commit, HULL_COMMIT);
			free(core_commit);
		}
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
	r.options  = o;
	r.spec     = spec;
	r.machine  = shown;
	r.choice   = &choice;
	r.rejected = 0u;
	planned = hull_plan_build(&plan, spec, machine, name, o->base, &choice,
	                          registry->host_arch, o->tokens, print_resolution, &r);
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

/* One machine of a whole file's run: its channel is 1 + 2 * its place, and
 * its stop's the one after. */
typedef struct member {
	char     id[64];
	char     name[64];
	int      ended;
	int      status;
	size_t   held[2];             /* a line not yet ended, per stream */
	uint8_t  line[2][LINE_CAP];
} member;

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

/* Every machine of a (machines ...) file: each planned before any starts,
 * so one the file cannot run starts none; all run at once on one
 * connection, a channel each, their output a line at a time behind their
 * names; and the first to fail, or SIGINT or SIGTERM, stops the rest. The
 * run ends when every machine has: 0 when each ended 0, else the status
 * of the first that did not. */
static int run_all(options *o, const hull_registry *registry, hull_spec *spec, uint8_t *data)
{
	static hull_names names_drawn;
	const hull_names *names = &names_drawn;
	static member   members[HULL_NAMES_MEMBERS];
	static hull_msg requests[HULL_NAMES_MEMBERS];
	hull_sexpr_form entry;
	hull_wire       wire;
	struct sigaction action;
	unsigned        count = 0u;
	unsigned        left;
	int             width = 0;
	int             status = 0;
	int             stopping = 0;

	memset(requests, 0, sizeof requests);
	memset(members, 0, sizeof members);
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
		                &requests[count], m->id, sizeof m->id) != 0) {
			status = 125;
			break;
		}
		if ((int)length > width)
			width = (int)length;
		count++;
	}
	free(data);
	if (status != 0) {
		for (unsigned i = 0u; i <= count && i < 64u; i++)
			hull_msg_free(&requests[i]);
		return status;
	}

	memset(&action, 0, sizeof action);
	action.sa_handler = interrupt;
	sigaction(SIGINT, &action, NULL);
	sigaction(SIGTERM, &action, NULL);
	if (connect_core(&wire, 1) != 0) {
		for (unsigned i = 0u; i < count; i++)
			hull_msg_free(&requests[i]);
		return 125;
	}
	for (unsigned i = 0u; i < count; i++) {
		if (requests[i].failed || hull_wire_send(wire.fd, &requests[i]) != 0) {
			fprintf(stderr, "hull: the run of %s could not be sent\n", members[i].name);
			for (unsigned j = i; j < count; j++)
				hull_msg_free(&requests[j]);
			hull_wire_close(&wire);
			return 125;
		}
		hull_msg_free(&requests[i]);
	}

	for (left = count; left > 0u;) {
		hull_sexpr_doc   body;
		hull_sexpr_form  value;
		long long        channel = -1;
		hull_wire_status read;
		member          *m;
		int              ended = -1;

		if ((interrupted || status != 0) && !stopping) {
			stopping = 1;
			for (unsigned i = 0u; i < count; i++)
				if (!members[i].ended)
					send_request(&wire, "(4:stop", 2u + 2u * i, members[i].id,
					             "(10:timeout_ms5:10000)");
		}
		read = hull_wire_read(&wire, &body);
		if (read == HULL_WIRE_WAIT)
			continue;
		if (read != HULL_WIRE_OK) {
			fprintf(stderr, "hull: the core went away\n");
			hull_wire_close(&wire);
			return 125;
		}
		if (!hull_msg_find(&body, "ch", &value) || !hull_msg_long(&body, &value, &channel) ||
		    channel < 1 || channel % 2 != 1 || (unsigned long long)channel >= 1u + 2u * count)
			continue;
		m = &members[(channel - 1) / 2];
		if (m->ended)
			continue;
		if (hull_msg_is(&body, "data")) {
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
			ended = 125;
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
		}
	}
	hull_wire_close(&wire);
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

	data = read_file(o->file, &length);
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
 * run as a chroot machine on it, `(mutable direct)` and its network none
 * unless it says (network true), and the tree committed, /tmp and its
 * discard paths dropped, with the parent's runtime and A.5's build
 * provenance. The last step's manifest is tagged `ref`.
 */
#define MAX_BUILD_STEPS 256u

static int build_file(options *o, const hull_registry *registry, const char *ref, int check)
{
	static char     first_pass[MAX_BUILD_STEPS][65];
	char            base_hex[65];
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
		int               chrooted;
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

		record_text(&spec, &step, "name", name, sizeof name);
		has_run = record_text(&spec, &step, "run", run_line, sizeof run_line);
		record_text(&spec, &step, "backend", backend, sizeof backend);
		/* Exported only where a machine runs: a step of copies alone gives
		 * no owner, and its tree is the directory as it stands. */
		chrooted = has_run && (backend[0] == '\0' || strcmp(backend, "ns") == 0 ||
		                       strcmp(backend, "chroot") == 0);
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
				char absolute[8200];

				if (input_count == MAX_STEP_INPUTS ||
				    !record_text(&spec, &item, "from", from, sizeof from)) {
					snprintf(why, sizeof why, "step %s: too many copies", name);
					goto done;
				}
				snprintf(absolute, sizeof absolute, "%s%s%s", from[0] == '/' ? "" : o->base,
				         from[0] == '/' ? "" : "/", from);
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
			/* On chroot, the step's machine is an overlay of that copy, the
			 * parent's owners given back in it and its tree exported, so an
			 * owner it gives, a package's own user's, is kept with no file
			 * of another owner on the user's disk (section 7). */
			snprintf(owners_path, sizeof owners_path, "%s/owners.list", scratch);
			snprintf(tar_path, sizeof tar_path, "%s/tree.tar", scratch);
			if (ok && chrooted) {
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
					ok = inputs[first].manifest[0] != '\0' ?
					     copy_tree(&store, root, &inputs[first], to, why, sizeof why) == 0 :
					     copy_file(root, inputs[first].path, to, why, sizeof why) == 0;
				}
			}
			/* The run line, in a chroot machine on the tree, or the
			 * engine the step names. */
			if (ok && has_run) {
				text_buf conf = { 0 };
				options  sub = { 0 };
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
				if (chrooted) {
					add_text(&conf, "\") (mutable overlay) (owners ");
					add_quoted(&conf, (const uint8_t *)owners_path, strlen(owners_path));
					add_text(&conf, ") (export ");
					add_quoted(&conf, (const uint8_t *)tar_path, strlen(tar_path));
					add_text(&conf, ") (backend ");
				} else {
					add_text(&conf, "\") (mutable direct) (backend ");
				}
				add_text(&conf, backend[0] == '\0' || strcmp(backend, "ns") == 0 ? "chroot" :
				                backend);
				add_text(&conf, ") (hostname \"build-");
				add_bytes(&conf, key_hex, 12u);
				add_text(&conf, "\")");
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
			if (chrooted) {
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
			if (ok && !chrooted) {
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
				if (!provenance.failed && chrooted) {
					int tar = open(tar_path, O_RDONLY | O_CLOEXEC);

					ok = tar >= 0 && hull_store_commit_tar(&store, tar, host_arch(), cache, drop,
					                                       &extra, &made, why, sizeof why) == 0;
					if (tar < 0)
						snprintf(why, sizeof why, "step %s: its tree was not exported: %s", name,
						         strerror(errno));
					if (tar >= 0)
						close(tar);
				} else {
					ok = !provenance.failed &&
					     hull_store_commit(&store, root_path, host_arch(), cache, &extra, &made,
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
	hull_store_release(held);
	hull_store_close(&store);
	free(data);
	return result;
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

/* `hull commit`: a directory, or with `tar` a tar of engine/tree.c's shape
 * from the file `dir` names, `-` for stdin, its owners kept. */
static int commit(const char *dir, const char *ref, const char *arch, int tar)
{
	hull_store  store;
	hull_commit made;
	char        why[4400] = "";
	int         status;
	int         fd = -1;

	if (tar && (fd = strcmp(dir, "-") == 0 ? 0 : open(dir, O_RDONLY | O_CLOEXEC)) < 0) {
		fprintf(stderr, "hull: commit: %s: %s\n", dir, strerror(errno));
		return 1;
	}
	if ((status = open_store(&store, 1)) != 0) {
		if (fd > 0)
			close(fd);
		return status;
	}
	{
		char              provenance[200];
		hull_commit_extra extra = { 0 };

		/* A.5's builder, alone: a commit has no build and no registry. */
		snprintf(provenance, sizeof provenance, "(10:provenance(7:builder%zu:%s))",
		         strlen(HULL_BUILDER), HULL_BUILDER);
		extra.provenance = (const uint8_t *)provenance;
		extra.provenance_length = strlen(provenance);
		status = tar ? hull_store_commit_tar(&store, fd, arch, ref, NULL, &extra, &made, why,
		                                     sizeof why) :
		               hull_store_commit(&store, dir, arch, ref, &extra, &made, why, sizeof why);
	}
	hull_store_close(&store);
	if (fd > 0)
		close(fd);
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

/* An image from its registry into the store, as `ref`, or as the image's
 * own name, so that a machine whose source is that name runs it. */
static int pull(const char *name, const char *ref)
{
	hull_store  store;
	hull_commit made;
	char        why[4400] = "";
	const char *arch = host_arch();
	int         status;

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
	snprintf(target, sizeof target, "root@%s.hull", id);
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
	/* Prefer the machine's own sshd where one runs (section 8.1 item 2):
	 * a (ssh true)-provisioned one, known by its persistent host key and
	 * pinned; else an image's own, found by a port-22 probe and left to
	 * the user's own known_hosts. A login shell over the agent otherwise,
	 * or where there is no ssh client. */
	if (machine_host_pub(id, pub, sizeof pub)) {
		if ((rc = ssh_client(id, pub)) >= 0)
			return rc;
	} else if (machine_sshd_open(id)) {
		if ((rc = ssh_client(id, NULL)) >= 0)
			return rc;
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
	static const char *const keys[] = { "corrupt", "delay", "down", "duplicate", "gemodel",
	                                    "jitter", "limit", "loss", "mtu", "rate", "reorder",
	                                    "seed" };
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
			        "gemodel, delay, jitter, reorder, duplicate, corrupt, rate, limit, mtu "
			        "and seed with =VALUE\n", argv[i]);
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
		char            machine[64] = "?";
		int             found = 0;

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
			else if (hull_sexpr_is(&body, &key, "out"))
				ways[0] = field, found |= 1;
			else if (hull_sexpr_is(&body, &key, "in"))
				ways[1] = field, found |= 2;
		}
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
	int           check  = 0;

	memset(&o, 0, sizeof o);
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
	if (strcmp(command, "core") == 0 && argc == 2)
		return hull_core_run(host_arch());
	if (strcmp(command, "ps") == 0 && argc == 2)
		return list_machines();
	if (strcmp(command, "gc") == 0 && argc == 2)
		return collect();
	if (strcmp(command, "commit") == 0 && argc == 4)
		return commit(argv[2], argv[3], host_arch(), 0);
	if (strcmp(command, "commit") == 0 && argc == 5 && strcmp(argv[2], "--tar") == 0)
		return commit(argv[3], argv[4], host_arch(), 1);
	if (strcmp(command, "commit") == 0 && argc == 6 && strcmp(argv[2], "--arch") == 0)
		return commit(argv[4], argv[5], argv[3], 0);
	if (strcmp(command, "materialize") == 0 && argc == 4)
		return materialize(argv[2], argv[3]);
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
	if (strcmp(command, "pull") == 0 && (argc == 3 || argc == 4))
		return pull(argv[2], argc == 4 ? argv[3] : argv[2]);
	if (strcmp(command, "manifest") == 0 && argc == 4 && strcmp(argv[2], "show") == 0)
		return show_manifest(argv[3]);
	if (strcmp(command, "manifest") == 0 && (argc == 3 || argc == 4) &&
	    strcmp(argv[2], "compile") == 0)
		return compile_manifest(argc == 4 ? argv[3] : NULL);
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
	    hull_registry_add(&registry, hull_engine_chroot(NULL), &reason) != HULL_REGISTRY_OK ||
	    hull_registry_add(&registry, hull_engine_dosbox(NULL), &reason) != HULL_REGISTRY_OK ||
	    hull_registry_add(&registry, hull_engine_dosemu(NULL), &reason) != HULL_REGISTRY_OK ||
	    hull_registry_set_default(&registry, "ns", "chroot") != HULL_REGISTRY_OK) {
		fprintf(stderr, "hull: a built-in engine is refused: %s\n", reason);
		return 2;
	}

	if (strcmp(command, "engines") == 0 && argc == 2) {
		list_engines(&registry);
		return 0;
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
		} else if (strcmp(arg, "--explain") == 0 && strcmp(command, "plan") == 0) {
			o.explain = 1;
		} else if (strcmp(arg, "--resolved") == 0 && strcmp(command, "plan") == 0) {
			o.resolved = 1;
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
