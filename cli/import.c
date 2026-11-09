/*
 * hull import FILE -- a Dockerfile to a (build ...) form (section 9, J.6),
 * on stdout, one-way and best-effort: what it cannot translate faithfully
 * it says in a `;` comment in the output, which stays a valid spec, rather
 * than dropping it silently. Section 3.6's step.env, step.workdir and
 * build.runtime exist for this: a Dockerfile's ENV and WORKDIR affect every
 * later instruction and the final image, so each becomes the following
 * steps' per-step env and workdir and the build's runtime; CMD, ENTRYPOINT,
 * EXPOSE, USER, VOLUME and STOPSIGNAL become the runtime.
 *
 * Each stage before the last is a (stage ...) of the form, the last the
 * build itself: a FROM naming a stage is its `stage:NAME` base, and COPY
 * --from a copy from its tree. A foreign image, `hull import IMAGE DIR`, is
 * engine/media.c's.
 *
 * A compose file is translated the same way to a (machines ...) form, and
 * is also section 9's compose dialect: hull_import_compose() gives the
 * form to the planner in place of a file, each note said on stderr as
 * well, so that `hull up` and `-f compose.yaml` run it with no step
 * between and nothing it leaves out goes unsaid.
 */
#define _GNU_SOURCE

#include <ctype.h>
#include <errno.h>
#include <sys/stat.h>
#include <arpa/inet.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "import.h"
#include "user.h"

#define MAX_ENV   128

/* Where a translation for the dialect says its notes, and of which file;
 * NULL for `hull import`, whose notes are in what it prints. */
static FILE       *warnings;
static const char *warned_path;
static int         unrunnable;
#define LINE_CAP  (1u << 20)

typedef struct sb {
	char  *data;
	size_t len;
	size_t cap;
	int    bad;
} sb;

static void sb_add(sb *b, const char *s, size_t n)
{
	if (b->bad)
		return;
	if (b->len + n + 1u > b->cap) {
		size_t cap = b->cap ? b->cap * 2u : 256u;
		char  *grown;

		while (cap < b->len + n + 1u)
			cap *= 2u;
		if ((grown = realloc(b->data, cap)) == NULL) {
			b->bad = 1;
			return;
		}
		b->data = grown;
		b->cap  = cap;
	}
	memcpy(b->data + b->len, s, n);
	b->len += n;
	b->data[b->len] = '\0';
}

static void sb_str(sb *b, const char *s)
{
	sb_add(b, s, strlen(s));
}

/* `s` as a hull string literal (A.1's readable: " \ \n \t escaped). */
static void sb_quoted(sb *b, const char *s)
{
	sb_str(b, "\"");
	for (; *s != '\0'; s++) {
		if (*s == '"' || *s == '\\')
			sb_add(b, (char[]){ '\\', *s }, 2u);
		else if (*s == '\n')
			sb_str(b, "\\n");
		else if (*s == '\t')
			sb_str(b, "\\t");
		else
			sb_add(b, s, 1u);
	}
	sb_str(b, "\"");
}

/* The instruction's argument with leading and trailing space removed. */
static char *trim(char *s)
{
	char *end;

	while (*s == ' ' || *s == '\t')
		s++;
	end = s + strlen(s);
	while (end > s && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r'))
		*--end = '\0';
	return s;
}

/* A JSON array of strings, `["a","b"]`, Docker's exec form. Returns the
 * count and fills `args` (pointers into `s`, which it rewrites in place),
 * or -1 when `s` is not one. Only the escapes Docker allows, \" and \\. */
static int exec_form(char *s, char **args, int max)
{
	int n = 0;

	s = trim(s);
	if (*s != '[')
		return -1;
	s++;
	for (;;) {
		char *out;

		while (*s == ' ' || *s == '\t' || *s == ',')
			s++;
		if (*s == ']')
			return n;
		if (*s != '"' || n == max)
			return -1;
		s++;
		args[n++] = out = s;
		while (*s != '"') {
			if (*s == '\0')
				return -1;
			if (*s == '\\' && (s[1] == '"' || s[1] == '\\'))
				s++;
			*out++ = *s++;
		}
		s++;
		*out = '\0';
	}
}

#define MAX_STAGES 32u
#define MAX_ARGS   64u

/* An ARG: its name, and its value, or NULL where it has none. */
typedef struct build_arg {
	char name[128];
	char value[1024];
	int  given;
} build_arg;

/* What a build was given from outside, `--build-arg` or compose's args:
 * and target:, for the next import_dockerfile(). */
static const hull_build_arg *given_args;
static unsigned              given_count;
static const char           *given_target;

typedef struct build {
	char      user[128];    /* the USER in force for a RUN, or "" for root */
	sb       steps;        /* each (step ...) as it is met */
	char     env_name[MAX_ENV][256];
	char     env_value[MAX_ENV][1024];
	unsigned env_count;
	char     workdir[4096];   /* "" until a WORKDIR, then absolute */
	int      had_from;
	unsigned step_no;
	char     stage[MAX_STAGES][128];  /* each stage's name so far, or its number */
	char     stage_number[MAX_STAGES][16];
	unsigned stage_count;
	build_arg global[MAX_ARGS];   /* before the first FROM: FROM's alone */
	unsigned  global_count;
	build_arg arg[MAX_ARGS];      /* this stage's */
	unsigned  arg_count;
} build;

/* The value the build was given for `name`, or NULL. */
static const char *given_arg(const char *name)
{
	for (unsigned i = 0u; i < given_count; i++)
		if (strcmp(given_args[i].name, name) == 0)
			return given_args[i].value;
	return NULL;
}

/* `name` among `list`, or NULL. */
static const build_arg *arg_called(const build_arg *list, unsigned count, const char *name,
                                   size_t length)
{
	for (unsigned i = 0u; i < count; i++)
		if (strlen(list[i].name) == length && memcmp(list[i].name, name, length) == 0)
			return &list[i];
	return NULL;
}

/* `text` with each $NAME, ${NAME}, ${NAME:-word} and ${NAME-word} of a
 * declared ARG put in, into `out`. Any other $ stays: a name may be the
 * base image's, and a Dockerfile's reader cannot see that. */
static void expand_args(const build_arg *list, unsigned count, const char *text, char *out,
                        size_t cap)
{
	size_t at = 0u;

	while (*text != '\0' && at + 1u < cap) {
		const char      *name = text + 1;
		const char      *end;
		const char      *word = NULL;
		const char      *close = NULL;
		const build_arg *arg;
		int              braced;
		int              empty_too = 0;

		if (*text != '$') {
			out[at++] = *text++;
			continue;
		}
		braced = *name == '{';
		name += braced;
		end = name;
		while (isalnum((unsigned char)*end) || *end == '_')
			end++;
		if (braced) {
			if (end[0] == ':' && end[1] == '-') {
				word = end + 2;
				empty_too = 1;
			} else if (end[0] == '-') {
				word = end + 1;
			} else if (end[0] != '}') {
				out[at++] = *text++;
				continue;
			}
			close = strchr(end, '}');
		}
		arg = end > name ? arg_called(list, count, name, (size_t)(end - name)) : NULL;
		if (arg == NULL || (braced && close == NULL)) {
			out[at++] = *text++;
			continue;
		}
		if (arg->given && !(empty_too && arg->value[0] == '\0'))
			at += (size_t)snprintf(out + at, cap - at, "%s", arg->value);
		else if (word != NULL)
			at += (size_t)snprintf(out + at, cap - at, "%.*s", (int)(close - word), word);
		if (at >= cap)
			at = cap - 1u;
		text = braced ? close + 1 : end;
	}
	out[at] = '\0';
}

static void note(sb *steps, const char *why, const char *text);

/* An ARG's `NAME[=value]` words, into `list`: the value the build was
 * given, the one written, or for a stage's bare name the global of that
 * name, as Docker scopes them. */
static void declare_args(build *b, build_arg *list, unsigned *count, char *text, int global)
{
	char *save;

	for (char *w = strtok_r(text, " \t", &save); w != NULL; w = strtok_r(NULL, " \t", &save)) {
		char            *eq = strchr(w, '=');
		const char      *from_outside;
		const build_arg *outer;
		build_arg       *arg;

		if (eq != NULL)
			*eq = '\0';
		/* Declared again, the later one stands. */
		arg = (build_arg *)arg_called(list, *count, w, strlen(w));
		if (arg == NULL && *count == MAX_ARGS) {
			note(&b->steps, "ARG past the most kept not translated", w);
			continue;
		}
		if (arg == NULL)
			arg = &list[(*count)++];
		memset(arg, 0, sizeof *arg);
		snprintf(arg->name, sizeof arg->name, "%s", w);
		from_outside = given_arg(arg->name);
		outer = global ? NULL : arg_called(b->global, b->global_count, w, strlen(w));
		if (from_outside != NULL) {
			snprintf(arg->value, sizeof arg->value, "%s", from_outside);
			arg->given = 1;
		} else if (eq != NULL) {
			char  *value = eq + 1;
			size_t n = strlen(value);

			if (n >= 2u && (value[0] == '"' || value[0] == '\'') && value[n - 1u] == value[0]) {
				value[n - 1u] = '\0';
				value++;
			}
			expand_args(list, *count, value, arg->value, sizeof arg->value);
			arg->given = 1;
		} else if (outer != NULL && outer->given) {
			snprintf(arg->value, sizeof arg->value, "%s", outer->value);
			arg->given = 1;
		} else if (!global) {
			note(&b->steps, "ARG given no value, and unset in what follows", w);
		}
	}
}

/* The stage `word` names, by its name or its number, or NULL. */
static const char *stage_called(const build *b, const char *word)
{
	for (unsigned i = 0u; i < b->stage_count; i++)
		if (strcasecmp(b->stage[i], word) == 0 || strcmp(b->stage_number[i], word) == 0)
			return b->stage[i];
	return NULL;
}

/* ENV accumulated so far, as the entries of an (env ...) record, and the
 * current (workdir ...) -- a RUN step gets both, since a Dockerfile's ENV
 * and WORKDIR reach every later instruction. */
static void step_context(build *b, sb *out)
{
	unsigned args = 0u;

	/* An ARG is a RUN's environment and no part of the image's. */
	for (unsigned i = 0u; i < b->arg_count; i++)
		args += (unsigned)b->arg[i].given;
	if (b->env_count > 0u || args > 0u) {
		sb_str(out, " (env");
		for (unsigned i = 0u; i < b->arg_count; i++) {
			int shadowed = !b->arg[i].given;

			for (unsigned e = 0u; e < b->env_count; e++)
				shadowed |= strcmp(b->env_name[e], b->arg[i].name) == 0;
			if (shadowed)
				continue;
			sb_str(out, " (");
			sb_str(out, b->arg[i].name);
			sb_str(out, " ");
			sb_quoted(out, b->arg[i].value);
			sb_str(out, ")");
		}
		for (unsigned i = 0u; i < b->env_count; i++) {
			sb_str(out, " (");
			sb_str(out, b->env_name[i]);
			sb_str(out, " ");
			sb_quoted(out, b->env_value[i]);
			sb_str(out, ")");
		}
		sb_str(out, ")");
	}
	if (b->workdir[0] != '\0') {
		sb_str(out, " (workdir ");
		sb_quoted(out, b->workdir);
		sb_str(out, ")");
	}
}

static void set_env(build *b, const char *name, const char *value)
{
	for (unsigned i = 0u; i < b->env_count; i++)
		if (strcmp(b->env_name[i], name) == 0) {
			snprintf(b->env_value[i], sizeof b->env_value[i], "%s", value);
			return;
		}
	if (b->env_count == MAX_ENV)
		return;
	snprintf(b->env_name[b->env_count], sizeof b->env_name[0], "%s", name);
	snprintf(b->env_value[b->env_count], sizeof b->env_value[0], "%s", value);
	b->env_count++;
}

/* WORKDIR, absolute or joined onto the one before it, as Docker does. */
static void set_workdir(build *b, const char *dir)
{
	if (dir[0] == '/' || b->workdir[0] == '\0')
		snprintf(b->workdir, sizeof b->workdir, "%s%s", dir[0] == '/' ? "" : "/", dir);
	else {
		size_t at = strlen(b->workdir);

		snprintf(b->workdir + at, sizeof b->workdir - at, "%s%s",
		         b->workdir[at - 1u] == '/' ? "" : "/", dir);
	}
}

/* A runtime word list -- (cmd w...) or (entrypoint w...) -- from either
 * form: ["a","b"] gives the words, a shell line gives /bin/sh -c LINE. */
static void runtime_words(sb *rt, const char *key, char *arg)
{
	char *words[64];
	int   n = exec_form(arg, words, 64);

	sb_str(rt, "\n    (");
	sb_str(rt, key);
	if (n < 0) {
		sb_str(rt, " \"/bin/sh\" \"-c\" ");
		sb_quoted(rt, trim(arg));
	} else
		for (int i = 0; i < n; i++) {
			sb_str(rt, " ");
			sb_quoted(rt, words[i]);
		}
	sb_str(rt, ")");
}

/* A runtime string list -- (ports ...) or (volumes ...). EXPOSE and VOLUME
 * take a space list or an exec form; EXPOSE's ports gain /tcp where bare. */
static void runtime_list(sb *rt, const char *key, char *arg, int ports)
{
	char *words[64];
	int   n = exec_form(arg, words, 64);
	char *save;

	sb_str(rt, "\n    (");
	sb_str(rt, key);
	if (n < 0) {
		n = 0;
		for (char *w = strtok_r(trim(arg), " \t", &save); w != NULL && n < 64;
		     w = strtok_r(NULL, " \t", &save))
			words[n++] = w;
	}
	for (int i = 0; i < n; i++) {
		sb_str(rt, " ");
		if (ports && strchr(words[i], '/') == NULL) {
			char p[64];

			snprintf(p, sizeof p, "%s/tcp", words[i]);
			sb_quoted(rt, p);
		} else
			sb_quoted(rt, words[i]);
	}
	sb_str(rt, ")");
}

static void note(sb *steps, const char *why, const char *text)
{
	sb_str(steps, "\n  ; ");
	sb_str(steps, why);
	if (text != NULL && text[0] != '\0') {
		sb_str(steps, ": ");
		sb_str(steps, text);
	}
}

/* A RUN: a step with the run line and the env and workdir in force. An exec
 * form has no shell; hull's run line is `/bin/sh -c`, so it is joined with
 * a note that the shell is now in the middle. */
static void do_run(build *b, char *arg)
{
	char  name[32];
	char *words[64];
	int   n = exec_form(arg, words, 64);
	char  joined[LINE_CAP];

	snprintf(name, sizeof name, "run-%u", ++b->step_no);
	if (n >= 0) {
		size_t at = 0u;

		note(&b->steps, "RUN exec form joined into a shell line", NULL);
		for (int i = 0; i < n && at + 1u < sizeof joined; i++)
			at += (size_t)snprintf(joined + at, sizeof joined - at, "%s%s", i ? " " : "",
			                       words[i]);
		arg = joined;
	}
	sb_str(&b->steps, "\n    (step (name \"");
	sb_str(&b->steps, name);
	sb_str(&b->steps, "\") (run ");
	sb_quoted(&b->steps, trim(arg));
	/* Docker gives every RUN the network; a file written for it counts on
	 * that, `apt-get install` first of all. */
	sb_str(&b->steps, ") (network true)");
	if (b->user[0] != '\0') {
		sb_str(&b->steps, " (user ");
		sb_quoted(&b->steps, b->user);
		sb_str(&b->steps, ")");
	}
	step_context(b, &b->steps);
	sb_str(&b->steps, ")");
}

/* A COPY or ADD: a step whose copies are its sources to the destination,
 * each made absolute against the workdir. ADD's url and tar-extract, and a
 * --from or --chown flag, are noted rather than done. */
/* A stage's final ENV and WORKDIR, which are its image's too, and what its
 * CMD and the rest gathered in `after`, into `rt`. */
static void stage_runtime(const build *b, sb *rt, const sb *after)
{
	if (b->env_count > 0u) {
		sb_str(rt, "\n    (env");
		for (unsigned i = 0u; i < b->env_count; i++) {
			sb_str(rt, " (");
			sb_str(rt, b->env_name[i]);
			sb_str(rt, " ");
			sb_quoted(rt, b->env_value[i]);
			sb_str(rt, ")");
		}
		sb_str(rt, ")");
	}
	if (b->workdir[0] != '\0') {
		sb_str(rt, "\n    (workdir ");
		sb_quoted(rt, b->workdir);
		sb_str(rt, ")");
	}
	sb_add(rt, after->data != NULL ? after->data : "", after->len);
}

/* The .dockerignore beside the Dockerfile, its patterns as quoted atoms,
 * which every COPY from the context takes as its (exclude ...). */
static sb ignore_list;

static void read_dockerignore(const char *dockerfile)
{
	const char *slash = strrchr(dockerfile, '/');
	char        path[4400];
	char        line[1024];
	FILE       *file;

	ignore_list.len = 0u;
	snprintf(path, sizeof path, "%.*s%s.dockerignore", slash != NULL ? (int)(slash - dockerfile) : 0,
	         dockerfile, slash != NULL ? "/" : "");
	if ((file = fopen(path, "r")) == NULL)
		return;
	while (fgets(line, sizeof line, file) != NULL) {
		char *text = trim(line);

		text[strcspn(text, "\r\n")] = '\0';
		text = trim(text);
		if (*text == '\0' || *text == '#')
			continue;
		sb_str(&ignore_list, " ");
		sb_quoted(&ignore_list, text);
	}
	fclose(file);
}

/* Whether `name` is a tar this reads, by its name: hull_unpack_layer()
 * takes plain and gzip. */
static int packed(const char *name)
{
	size_t n = strlen(name);

	return (n > 4u && strcmp(name + n - 4u, ".tar") == 0) ||
	       (n > 7u && strcmp(name + n - 7u, ".tar.gz") == 0) ||
	       (n > 4u && strcmp(name + n - 4u, ".tgz") == 0);
}

/* Where `dst` is, by the workdir: `.` and `./x` as a shell would read them, a
 * destination that is a directory keeping its slash, which puts a file in it. */
static void copy_target(const build *b, const char *dst, int several, char *to, size_t cap)
{
	const char *base = b->workdir[0] != '\0' ? b->workdir : "/";
	char        joined[16384];
	size_t      at = 0u;
	int         dir = several || strcmp(dst, ".") == 0 || dst[strlen(dst) - 1u] == '/' ||
	                  (strlen(dst) >= 2u && strcmp(dst + strlen(dst) - 2u, "/.") == 0);

	if (dst[0] == '/')
		snprintf(joined, sizeof joined, "%s", dst);
	else
		snprintf(joined, sizeof joined, "%s/%s", base, dst);
	for (const char *c = joined; *c != '\0' && at + 2u < cap;) {
		if (c[0] == '/' && c[1] == '/') {
			c++;
		} else if (c[0] == '/' && c[1] == '.' && (c[2] == '/' || c[2] == '\0')) {
			c += 2;
		} else {
			to[at++] = *c++;
		}
	}
	if (at == 0u || (dir && to[at - 1u] != '/'))
		to[at++] = '/';
	if (!dir && at > 1u && to[at - 1u] == '/')
		at--;
	to[at] = '\0';
}

/* What a chown or chmod after a copy names: `from`'s landing, added to `list`,
 * each single-quoted for the shell. A directory's contents land in `to`
 * itself; a file sent to a directory, under its own name. */
static void landed_at(char *list, size_t cap, const char *from, const char *to)
{
	size_t      at = strlen(list);
	size_t      length = strlen(from);
	const char *leaf = strrchr(from, '/');
	int         contents = strcmp(from, ".") == 0 || (length > 0u && from[length - 1u] == '/');

	if (strchr(to, '\'') != NULL || strchr(from, '\'') != NULL)
		return;
	if (contents || to[strlen(to) - 1u] != '/')
		snprintf(list + at, cap - at, " '%.4000s'", to);
	else
		snprintf(list + at, cap - at, " '%.4000s%.3000s'", to, leaf != NULL ? leaf + 1 : from);
}

/* A step of a line after a copy, as root whatever USER says, as docker's
 * --chown and --chmod are. */
static void after_copy(build *b, const char *tool, const char *value, const char *list)
{
	char name[32];
	char line[8400];

	snprintf(name, sizeof name, "%s-%u", tool, ++b->step_no);
	snprintf(line, sizeof line, "%s -R '%s'%s", tool, value, list);
	sb_str(&b->steps, "\n    (step (name \"");
	sb_str(&b->steps, name);
	sb_str(&b->steps, "\") (run ");
	sb_quoted(&b->steps, line);
	sb_str(&b->steps, "))");
}

static void do_copy(build *b, const char *instr, char *arg)
{
	char  name[32];
	char *words[64];
	int   n = 0;
	char *save;
	char *dst;
	const char *stage = NULL;
	const char *chown_to = NULL;
	const char *chmod_to = NULL;
	char        landed[8192] = "";

	for (char *w = strtok_r(trim(arg), " \t", &save); w != NULL && n < 64;
	     w = strtok_r(NULL, " \t", &save)) {
		if (strncmp(w, "--from=", 7u) == 0) {
			if ((stage = stage_called(b, w + 7)) == NULL) {
				note(&b->steps, "COPY --from an image not translated", w + 7);
				return;
			}
			continue;
		}
		if (strncmp(w, "--chown=", 8u) == 0) {
			chown_to = w + 8;
			continue;
		}
		if (strncmp(w, "--chmod=", 8u) == 0) {
			chmod_to = w + 8;
			continue;
		}
		/* How docker stores a layer, which says nothing of what is in it. */
		if (strcmp(w, "--link") == 0)
			continue;
		if (w[0] == '-' && w[1] == '-') {
			note(&b->steps, "copy flag not translated", w);
			continue;
		}
		words[n++] = w;
	}
	if (n < 2) {
		note(&b->steps, instr, "needs a source and a destination");
		return;
	}
	/* ADD's url Docker fetches, and nothing here does. */
	for (int i = 0; i < n - 1; i++)
		if (strcmp(instr, "ADD") == 0 && strstr(words[i], "://") != NULL)
			note(&b->steps, "ADD of a url not fetched, and left a COPY", words[i]);
	dst = words[n - 1];
	snprintf(name, sizeof name, "copy-%u", ++b->step_no);
	sb_str(&b->steps, "\n    (step (name \"");
	sb_str(&b->steps, name);
	sb_str(&b->steps, "\") (copy");
	for (int i = 0; i < n - 1; i++) {
		char to[8192];

		copy_target(b, dst, n > 2, to, sizeof to);
		landed_at(landed, sizeof landed, words[i], to);
		sb_str(&b->steps, " (path (from ");
		sb_quoted(&b->steps, words[i]);
		sb_str(&b->steps, ") (to ");
		sb_quoted(&b->steps, to);
		if (stage != NULL) {
			sb_str(&b->steps, ") (stage ");
			sb_quoted(&b->steps, stage);
		}
		sb_str(&b->steps, ")");
		/* ADD unpacks a local tar, as a base image's first line does. */
		if (strcmp(instr, "ADD") == 0 && stage == NULL && strstr(words[i], "://") == NULL &&
		    packed(words[i]))
			sb_str(&b->steps, " (unpack true)");
		else if (stage == NULL && ignore_list.len > 0u) {
			sb_str(&b->steps, " (exclude");
			sb_add(&b->steps, ignore_list.data, ignore_list.len);
			sb_str(&b->steps, ")");
		}
		sb_str(&b->steps, ")");
	}
	sb_str(&b->steps, ")");
	if (b->workdir[0] != '\0') {
		sb_str(&b->steps, " (workdir ");
		sb_quoted(&b->steps, b->workdir);
		sb_str(&b->steps, ")");
	}
	sb_str(&b->steps, ")");
	if (chown_to != NULL && landed[0] != '\0')
		after_copy(b, "chown", chown_to, landed);
	if (chmod_to != NULL && landed[0] != '\0')
		after_copy(b, "chmod", chmod_to, landed);
}

static void do_env(build *b, char *arg)
{
	char *space = strchr(arg, ' ');
	char *eq    = strchr(arg, '=');

	if (eq != NULL && (space == NULL || eq < space)) {
		char *save;

		for (char *w = strtok_r(arg, " \t", &save); w != NULL; w = strtok_r(NULL, " \t", &save)) {
			char *v = strchr(w, '=');

			if (v == NULL)
				note(&b->steps, "ENV pair without a value not translated", w);
			else {
				*v = '\0';
				set_env(b, w, v + 1);
			}
		}
	} else if (space != NULL) {
		*space = '\0';
		set_env(b, arg, trim(space + 1));
	} else {
		note(&b->steps, "ENV without a value not translated", arg);
	}
}

static int import_dockerfile(FILE *file)
{
	static build b;
	sb     rt = { NULL, 0u, 0u, 0 };
	sb     stages = { NULL, 0u, 0u, 0 };
	sb     after = { NULL, 0u, 0u, 0 };   /* CMD/ENTRYPOINT/etc. order kept */
	char  *base = NULL;
	char   line[LINE_CAP];
	char   logical[LINE_CAP];
	static char expanded[LINE_CAP];
	size_t at = 0u;
	int    status = 0;

	/* Static for its size, and `hull up` imports one Dockerfile after another. */
	memset(&b, 0, sizeof b);
	while (fgets(line, sizeof line, file) != NULL) {
		char  *p = line;
		size_t n;

		p[strcspn(p, "\n")] = '\0';
		while (*p == ' ' || *p == '\t')
			p++;
		if (at == 0u && *p == '#')   /* a comment, or a # syntax= directive */
			continue;
		n = strlen(p);
		if (n > 0u && p[n - 1u] == '\\') {   /* a continuation: join the next */
			p[n - 1u] = ' ';
			at += (size_t)snprintf(logical + at, sizeof logical - at, "%s", at == 0u ? line : p);
			continue;
		}
		at += (size_t)snprintf(logical + at, sizeof logical - at, "%s", at == 0u ? line : p);
		at = 0;
		{
			char *s = trim(logical);
			char *sp;
			char  instr[32];
			size_t k = 0u;

			if (*s == '\0' || *s == '#')
				continue;
			for (sp = s; *sp != '\0' && *sp != ' ' && *sp != '\t' && k + 1u < sizeof instr; sp++)
				instr[k++] = (char)toupper((unsigned char)*sp);
			instr[k] = '\0';
			s = trim(sp);
			/* Asked for a stage, what follows it is no part of the image. */
			if (strcmp(instr, "FROM") == 0 && given_target != NULL && b.stage_count > 0u &&
			    strcasecmp(b.stage[b.stage_count - 1u], given_target) == 0)
				break;
			if (strcmp(instr, "ARG") == 0) {
				if (b.had_from)
					declare_args(&b, b.arg, &b.arg_count, s, 0);
				else
					declare_args(&b, b.global, &b.global_count, s, 1);
				continue;
			}
			/* A RUN has its ARGs as its environment, and CMD and ENTRYPOINT
			 * are not expanded by Docker: everything else has them put in. */
			if (strcmp(instr, "FROM") == 0) {
				expand_args(b.global, b.global_count, s, expanded, sizeof expanded);
				s = expanded;
			} else if (strcmp(instr, "RUN") != 0 && strcmp(instr, "CMD") != 0 &&
			           strcmp(instr, "ENTRYPOINT") != 0) {
				expand_args(b.arg, b.arg_count, s, expanded, sizeof expanded);
				s = expanded;
			}
			if (strcmp(instr, "FROM") == 0) {
				char       *as;
				char       *word = s;
				const char *earlier;
				char        named[160];

				/* The stage before this one is whole. */
				if (b.had_from) {
					stage_runtime(&b, &rt, &after);
					sb_str(&stages, "\n  (stage (name ");
					sb_quoted(&stages, b.stage[b.stage_count - 1u]);
					sb_str(&stages, ") (base ");
					sb_quoted(&stages, base);
					sb_str(&stages, ")");
					if (rt.len > 0u) {
						sb_str(&stages, "\n    (runtime");
						sb_add(&stages, rt.data, rt.len);
						sb_str(&stages, ")");
					}
					if (b.steps.len > 0u) {
						sb_str(&stages, "\n    (steps");
						sb_add(&stages, b.steps.data, b.steps.len);
						sb_str(&stages, "\n    )");
					}
					sb_str(&stages, ")");
					rt.len = after.len = b.steps.len = 0u;
					b.env_count = 0u;
					b.workdir[0] = '\0';
					free(base);
				}
				b.had_from = 1;
				b.user[0] = '\0';   /* a stage starts as its base's root */
				b.arg_count = 0u;
				while (strncmp(word, "--", 2u) == 0) {   /* --platform and the like */
					char *end = strpbrk(word, " \t");

					if (end != NULL)
						*end = '\0';
					note(&b.steps, "FROM flag not translated", word);
					word = end != NULL ? trim(end + 1) : word + strlen(word);
				}
				if ((as = strstr(word, " AS ")) != NULL || (as = strstr(word, " as ")) != NULL)
					*as = '\0';
				word = trim(word);
				earlier = stage_called(&b, word);
				snprintf(named, sizeof named, "stage:%s", earlier != NULL ? earlier : "");
				base = strdup(earlier != NULL ? named : word);
				if (b.stage_count == MAX_STAGES) {
					fprintf(stderr, "hull: import: more than %u stages\n", MAX_STAGES);
					free(base);
					return 2;
				}
				snprintf(b.stage_number[b.stage_count], sizeof b.stage_number[0], "%u", b.stage_count);
				snprintf(b.stage[b.stage_count], sizeof b.stage[0], "%s",
				         as != NULL ? trim(as + 4) : b.stage_number[b.stage_count]);
				b.stage_count++;
			} else if (!b.had_from) {
				note(&b.steps, "instruction before FROM not translated", instr);
			} else if (strcmp(instr, "RUN") == 0) {
				do_run(&b, s);
			} else if (strcmp(instr, "COPY") == 0 || strcmp(instr, "ADD") == 0) {
				do_copy(&b, instr, s);
			} else if (strcmp(instr, "ENV") == 0) {
				do_env(&b, s);
			} else if (strcmp(instr, "WORKDIR") == 0) {
				set_workdir(&b, trim(s));
			} else if (strcmp(instr, "CMD") == 0) {
				runtime_words(&after, "cmd", s);
			} else if (strcmp(instr, "ENTRYPOINT") == 0) {
				runtime_words(&after, "entrypoint", s);
			} else if (strcmp(instr, "EXPOSE") == 0) {
				runtime_list(&after, "ports", s, 1);
			} else if (strcmp(instr, "VOLUME") == 0) {
				runtime_list(&after, "volumes", s, 0);
			} else if (strcmp(instr, "USER") == 0) {
				sb_str(&after, "\n    (user ");
				sb_quoted(&after, trim(s));
				sb_str(&after, ")");
				/* Every RUN after it runs as it, root being what one runs as
				 * unasked. */
				snprintf(b.user, sizeof b.user, "%s", hull_user_is_root(trim(s)) ? "" : trim(s));
			} else if (strcmp(instr, "STOPSIGNAL") == 0) {
				sb_str(&after, "\n    (stop_signal ");
				sb_quoted(&after, trim(s));
				sb_str(&after, ")");
			} else {
				note(&b.steps, "not translated", instr);
			}
		}
	}
	if (base == NULL) {
		fprintf(stderr, "hull: import: the Dockerfile has no FROM\n");
		free(b.steps.data);
		free(rt.data);
		free(after.data);
		return 2;
	}
	if (given_target != NULL && strcasecmp(b.stage[b.stage_count - 1u], given_target) != 0) {
		fprintf(stderr, "hull: import: the Dockerfile has no stage %s\n", given_target);
		free(base);
		free(b.steps.data);
		free(rt.data);
		free(after.data);
		free(stages.data);
		return 2;
	}
	stage_runtime(&b, &rt, &after);
	if (b.steps.bad || rt.bad || after.bad || stages.bad) {
		fprintf(stderr, "hull: import: out of memory\n");
		status = 2;
	} else {
		printf("(build\n  (base ");
		/* base is a token; a string keeps a slash and a colon safe. */
		printf("\"%s\")", base);
		if (rt.len > 0u)
			printf("\n  (runtime%s)", rt.data);
		if (stages.len > 0u)
			printf("\n  (stages%s\n  )", stages.data);
		/* The steps' closing paren is on its own line: the last entry may be
		 * a `;` comment, which would swallow a paren on the same line. */
		if (b.steps.len > 0u)
			printf("\n  (steps%s\n  )", b.steps.data);
		printf(")\n");
	}
	free(base);
	free(b.steps.data);
	free(rt.data);
	free(after.data);
	free(stages.data);
	return status;
}

/* --- compose ------------------------------------------------------------- */

static hull_compose_build builds[MAX_COMPOSE_BUILDS];
static unsigned           build_count;
static char               build_project[64];

unsigned hull_compose_builds(const hull_compose_build **list)
{
	*list = builds;
	return build_count;
}

/* A docker-compose.yml to a (machines ...) file (section 9, J.7). Only a
 * common block subset of YAML is read; what it cannot map onto hull's model
 * it says in a `;` comment. depends_on is (after ...), and a healthcheck
 * (ready ...), so a service starts once those it depends on are up, as
 * under compose. A network is a segment
 * (Appendix D) named PROJECT-NETWORK, as compose names it PROJECT_NETWORK
 * but for the `_` a segment's name cannot hold, and addressed, so that its
 * services find each other by name as they do under compose: a service
 * with no networks and no network_mode is on `default`, as under compose.
 */
#define MAX_NETS 16
typedef struct svc {
	char name[128];
	char source[512];      /* from image: */
	char workdir[512];
	char hostname[256];
	char restart[32];
	char network[8];       /* from network_mode: host, nat or none, or "" */
	sb   command;          /* a (command ...) word list, or empty */
	sb   env;              /* (NAME "value") entries */
	sb   ports;            /* (port ...) entries */
	sb   volumes;          /* (volume ...) entries */
	sb   notes;            /* this service's `;` annotations */
	int  open;             /* a service is being read */
	int  mode_given;       /* network_mode said, so no network is implied */
	char nets[MAX_NETS][64];
	int  net_count;
	int  net_indent;       /* where a networks map's names are, or -1 */
	char net_current[64];  /* the networks map's name being read */
	sb   after;            /* depends_on, as machine names */
	int  dep_indent;       /* where a depends_on map's names are, or -1 */
	char dep_current[64];  /* the depends_on map's name being read */
	int  health_given;     /* a healthcheck test said */
	int  health_off;       /* disable: true, or a test of NONE */
	int  health_shell;     /* the test is a shell line */
	char test[16][256];    /* the test's words, CMD's or the line */
	int  test_count;
	int  in_test;          /* a block list of the test's words follows */
	char build_context[512];   /* from build:, relative to the file */
	char build_dockerfile[256];
	char build_target[128];
	sb   file_env;         /* env_file:'s `NAME=value` lines, environment: winning */
	char memory[32];       /* from mem_limit: */
	char cpus[16];         /* from cpus:, a whole number */
	char user[128];        /* from user: */
	char shm_size[32];     /* from shm_size: */
	char arch[16];         /* from platform: */
	sb   devices;          /* from devices:, each path quoted */
	sb   secrets;          /* from secrets:, each a whole entry */
	int  in_reservations;  /* under deploy:'s reservations:, not its limits: */
	char stop_signal[32];  /* from stop_signal: */
	long stop_ms;          /* from stop_grace_period:, or 0 */
	sb   tmpfs;            /* from tmpfs:, each path quoted */
	sb   hosts;            /* from extra_hosts:, quoted NAME:ADDRESS */
	sb   dns;              /* from dns: */
	sb   sysctls;          /* from sysctls: */
	sb   ulimits;          /* from ulimits: */
	char ulimit_name[32];  /* the name a nested soft: or hard: is under */
	char ulimit_soft[32];
	int  read_only;        /* from read_only: */
	hull_build_arg build_arg[HULL_MAX_BUILD_ARGS];
	unsigned build_arg_count;
	int  args_indent;      /* where build's args: key is, or -1 */
	char profile[8][64];   /* from profiles: */
	int  profile_count;
	long interval_ms;      /* compose's own, or -1 for its default */
	long timeout_ms;
	long retries;
	long start_ms;
} svc;

/* The networks a file's services are on, in the order first met, each
 * given its subnet once the project's name is known. */
static char file_nets[MAX_NETS * 8][64];
static int  file_net_count;

/* What the file's top-level networks: say of each, by its name as a
 * label: enable_ipv6, and the subnets its ipam config gives. */
static struct declared {
	char name[64];
	int  ipv6;
	char subnet[64];
	char subnet6[64];
} declared[MAX_NETS * 8];
static int declared_count;

static void as_label(char *out, size_t cap, const char *text);

/* The declared network named `name`, made where `make` and it is not yet;
 * NULL where there is none or no room. */
static struct declared *declared_of(const char *name, int make)
{
	char label[64];

	as_label(label, sizeof label, name);
	for (int i = 0; i < declared_count; i++)
		if (strcmp(declared[i].name, label) == 0)
			return &declared[i];
	if (!make || declared_count == MAX_NETS * 8)
		return NULL;
	memset(&declared[declared_count], 0, sizeof declared[0]);
	snprintf(declared[declared_count].name, sizeof declared[0].name, "%s", label);
	return &declared[declared_count++];
}

/* Whether `text` is a network hull takes as a segment's subnet: IPv4,
 * a.b.c.d/n with n from 8 to 29, or IPv6, with n from 48 to 120, its host
 * bits clear, as the spec checks them. */
static int usable_subnet(const char *text, int six)
{
	char          copy[64];
	char         *slash;
	unsigned char address[16];
	unsigned long prefix;
	unsigned      bits = six ? 128u : 32u;

	snprintf(copy, sizeof copy, "%s", text);
	if ((slash = strchr(copy, '/')) == NULL)
		return 0;
	*slash = '\0';
	prefix = strtoul(slash + 1, NULL, 10);
	if (inet_pton(six ? AF_INET6 : AF_INET, copy, address) != 1 ||
	    prefix < (six ? 48u : 8u) || prefix > (six ? 120u : 29u))
		return 0;
	for (unsigned bit = (unsigned)prefix; bit < bits; bit++)
		if (address[bit / 8u] & (0x80u >> (bit % 8u)))
			return 0;
	return 1;
}

/* `text` as a DNS label, as a segment's and a machine's name must be:
 * lowercase, `-` for anything else, none leading, trailing or doubled,
 * at most 63; "x" where nothing is left. */
static void as_label(char *out, size_t cap, const char *text)
{
	size_t n = 0u;

	for (const char *c = text; *c != '\0' && n + 1u < cap && n < 63u; c++) {
		char ch = (char)tolower((unsigned char)*c);

		if (!isalnum((unsigned char)ch))
			ch = '-';
		if (ch == '-' && (n == 0u || out[n - 1u] == '-'))
			continue;
		out[n++] = ch;
	}
	while (n > 0u && out[n - 1u] == '-')
		n--;
	if (n == 0u && cap > 1u)
		out[n++] = 'x';
	out[n] = '\0';
}

/* Put the service on network `name`, once. */
static void svc_join(svc *s, const char *name)
{
	char label[64];

	as_label(label, sizeof label, name);
	for (int i = 0; i < s->net_count; i++)
		if (strcmp(s->nets[i], label) == 0)
			return;
	if (s->net_count < MAX_NETS)
		snprintf(s->nets[s->net_count++], sizeof s->nets[0], "%s", label);
}

/* The scalar `s` without surrounding quotes and outer space. */
static char *scalar(char *s)
{
	size_t n;

	s = trim(s);
	n = strlen(s);
	if (n >= 2u && (s[0] == '"' || s[0] == '\'') && s[n - 1u] == s[0]) {
		s[n - 1u] = '\0';
		s++;
	}
	return s;
}

/* A flow list `[a, "b"]` into `args` (pointers into `s`, rewritten), or -1
 * where `s` is not one. Bare and quoted items, which is all compose uses. */
static int flow_list(char *s, char **args, int max)
{
	int n = 0;

	s = trim(s);
	if (*s != '[')
		return -1;
	s++;
	for (;;) {
		char *out;
		char  q = 0;
		char  stop;

		while (*s == ' ' || *s == '\t' || *s == ',')
			s++;
		if (*s == ']')
			return n;
		if (n == max)
			return -1;
		if (*s == '"' || *s == '\'') {
			q = *s++;
		}
		args[n++] = out = s;
		while (*s != '\0' && (q ? *s != q : (*s != ',' && *s != ']'))) {
			if (*s == '\\' && q && s[1] != '\0')
				s++;
			*out++ = *s++;
		}
		if (q && *s == q)
			s++;
		else
			while (out > args[n - 1] && (out[-1] == ' ' || out[-1] == '\t'))
				out--;
		/* An unquoted item is ended in place, which overwrites the `,` or
		 * `]` that ended it: so what that was is read first. */
		stop = *s;
		*out = '\0';
		if (stop == '\0')
			return -1;
		if (!q) {
			if (stop == ']')
				return n;
			s++;
		}
	}
}

static void svc_note(svc *s, const char *why, const char *text)
{
	if (warnings != NULL)
		fprintf(warnings, "%s: %s: %s%s%s\n", warned_path, s->name, why,
		        text != NULL && text[0] != '\0' ? ": " : "", text != NULL ? text : "");
	sb_str(&s->notes, "\n    ; ");
	sb_str(&s->notes, why);
	if (text != NULL && text[0] != '\0') {
		sb_str(&s->notes, ": ");
		sb_str(&s->notes, text);
	}
}

/* K=V (compose list form) or K: V (map form) into the env list. */
static void compose_env(svc *s, char *item, int map_form)
{
	char *eq = map_form ? strchr(item, ':') : strchr(item, '=');

	if (eq == NULL) {
		svc_note(s, "environment entry without a value not translated", item);
		return;
	}
	*eq = '\0';
	sb_str(&s->env, " (");
	sb_str(&s->env, trim(item));
	sb_str(&s->env, " ");
	sb_quoted(&s->env, scalar(eq + 1));
	sb_str(&s->env, ")");
}

/* "[host:]guest[/proto]" -> a (port ...). The ip:host:guest and the
 * host-range forms are noted, not translated. */
static void compose_port(svc *s, char *spec)
{
	char *p = scalar(spec);
	char *slash = strrchr(p, '/');
	char *colon;
	char  proto[8] = "";

	if (strchr(p, ':') != NULL && strchr(p, ':') != strrchr(p, ':')) {
		svc_note(s, "port with an ip or range not translated", p);
		return;
	}
	if (slash != NULL) {
		*slash = '\0';
		snprintf(proto, sizeof proto, "%s", slash + 1);
	}
	colon = strchr(p, ':');
	sb_str(&s->ports, " (port (host ");
	if (colon != NULL) {
		*colon = '\0';
		sb_str(&s->ports, p);
		sb_str(&s->ports, ") (guest ");
		sb_str(&s->ports, colon + 1);
	} else {
		sb_str(&s->ports, p);
		sb_str(&s->ports, ") (guest ");
		sb_str(&s->ports, p);
	}
	sb_str(&s->ports, ")");
	if (proto[0] != '\0' && strcmp(proto, "tcp") != 0) {
		sb_str(&s->ports, " (proto ");
		sb_str(&s->ports, proto);
		sb_str(&s->ports, ")");
	}
	sb_str(&s->ports, ")");
}

/* "host:guest[:ro]" -> a (volume ...). A named volume, no leading /, is
 * noted: hull has no named volumes. */
/* $XDG_DATA_HOME/hull/compose/PROJECT/NAME into `out`, made where the
 * dialect reads the file, `hull import` only printing one: 1, or 0. */
static int named_volume(const char *name, char *out, size_t cap)
{
	const char *data = getenv("XDG_DATA_HOME");
	const char *home = getenv("HOME");
	char        base[4000];
	char        step[4400];

	if (strchr(name, '/') != NULL)
		return 0;
	if (data != NULL && data[0] != '\0')
		snprintf(base, sizeof base, "%s", data);
	else if (home != NULL && home[0] != '\0')
		snprintf(base, sizeof base, "%s/.local/share", home);
	else
		return 0;
	snprintf(out, cap, "%s/hull/compose/%s/%s", base, build_project, name);
	if (warnings == NULL)
		return 1;
	for (char *slash = strchr(out + 1, '/'); slash != NULL; slash = strchr(slash + 1, '/')) {
		snprintf(step, sizeof step, "%.*s", (int)(slash - out), out);
		mkdir(step, 0700);
	}
	return mkdir(out, 0700) == 0 || errno == EEXIST;
}

static void compose_volume(svc *s, char *spec)
{
	char *p = scalar(spec);
	char *c1 = strchr(p, ':');
	char *c2;
	char  kept[4400];
	int   ro;

	if (c1 == NULL) {
		svc_note(s, "anonymous volume not translated", p);
		return;
	}
	*c1 = '\0';
	/* A named volume outlives the machine, so it is a directory of the
	 * user's, the project's and its name's, made as the dialect reads the
	 * file: a database's data survives a `hull up` as under compose. */
	if (p[0] != '/' && p[0] != '.' && p[0] != '~') {
		if (!named_volume(p, kept, sizeof kept)) {
			svc_note(s, "named volume not translated", p);
			return;
		}
		svc_note(s, "named volume kept, starting empty, not from the image, at", kept);
		p = kept;
	}
	c2 = strchr(c1 + 1, ':');
	ro = c2 != NULL && strstr(c2, "ro") != NULL;
	if (c2 != NULL)
		*c2 = '\0';
	sb_str(&s->volumes, " (volume (host ");
	sb_quoted(&s->volumes, p);
	sb_str(&s->volumes, ") (guest ");
	sb_quoted(&s->volumes, trim(c1 + 1));
	sb_str(&s->volumes, ")");
	if (ro)
		sb_str(&s->volumes, " (mode ro)");
	sb_str(&s->volumes, ")");
}

/* `/path` or `/path:size=64m`: the path; its options said and left. */
static void compose_tmpfs(svc *s, char *spec)
{
	char *options = strchr(spec, ':');

	if (options != NULL) {
		*options = '\0';
		svc_note(s, "tmpfs options not translated", options + 1);
	}
	if (spec[0] != '/') {
		svc_note(s, "tmpfs not translated", spec);
		return;
	}
	sb_str(&s->tmpfs, " ");
	sb_quoted(&s->tmpfs, spec);
}

static void compose_word(sb *list, const char *word)
{
	sb_str(list, " ");
	sb_quoted(list, word);
}

/* `name:address` or the newer `name=address`, as hull's NAME:ADDRESS. */
static void compose_host(svc *s, char *entry)
{
	char *equals = strchr(entry, '=');

	if (equals != NULL)
		*equals = ':';
	compose_word(&s->hosts, entry);
}

/* A map's entry of extra_hosts:, sysctls: or ulimits:, `KEY: VALUE`, a limit's
 * `soft:` and `hard:` nested under its name. */
static void compose_pair(svc *s, const char *field, char *content)
{
	char  line[300];
	char *colon = strchr(content, ':');
	char *key = content;
	char *value;

	if (colon == NULL)
		return;
	*colon = '\0';
	key = trim(key);
	value = scalar(trim(colon + 1));
	if (strcmp(field, "extra_hosts") == 0) {
		snprintf(line, sizeof line, "%s:%s", key, value);
		compose_word(&s->hosts, line);
	} else if (strcmp(field, "sysctls") == 0) {
		snprintf(line, sizeof line, "%s=%s", key, value);
		compose_word(&s->sysctls, line);
	} else if (strcmp(key, "soft") == 0) {
		snprintf(s->ulimit_soft, sizeof s->ulimit_soft, "%s", value);
	} else if (strcmp(key, "hard") == 0) {
		snprintf(line, sizeof line, "%s=%s:%s", s->ulimit_name, s->ulimit_soft, value);
		compose_word(&s->ulimits, line);
	} else if (value[0] == '\0') {
		snprintf(s->ulimit_name, sizeof s->ulimit_name, "%s", key);
	} else {
		snprintf(line, sizeof line, "%s=%s", key, value);
		compose_word(&s->ulimits, line);
	}
}

static void svc_reset(svc *s)
{
	free(s->after.data);
	free(s->command.data);
	free(s->env.data);
	free(s->ports.data);
	free(s->volumes.data);
	free(s->notes.data);
	free(s->file_env.data);
	free(s->tmpfs.data);
	free(s->devices.data);
	free(s->secrets.data);
	free(s->hosts.data);
	free(s->dns.data);
	free(s->sysctls.data);
	free(s->ulimits.data);
	memset(s, 0, sizeof *s);
	s->dep_indent  = -1;
	s->args_indent = -1;
	s->interval_ms = -1;
	s->timeout_ms  = -1;
	s->retries     = -1;
	s->start_ms    = -1;
}

/* A compose duration, `1m30s` or `500ms`, in milliseconds; -1 where it is
 * not one. */
static long compose_ms(const char *text)
{
	static const struct { const char *unit; long ms; } units[] = {
		{ "ms", 1 }, { "us", 0 }, { "ns", 0 }, { "h", 3600000 }, { "m", 60000 }, { "s", 1000 },
	};
	long total = 0;

	if (*text == '\0')
		return -1;
	while (*text != '\0') {
		char  *end;
		double n = strtod(text, &end);
		size_t i;

		if (end == text)
			return -1;
		for (i = 0u; i < sizeof units / sizeof units[0]; i++)
			if (strncmp(end, units[i].unit, strlen(units[i].unit)) == 0)
				break;
		if (i == sizeof units / sizeof units[0])
			return -1;
		total += (long)(n * (double)units[i].ms);
		text = end + strlen(units[i].unit);
	}
	return total;
}

/* depends_on's `name`, as the machine it became. */
static void svc_after(svc *s, const char *name)
{
	char label[64];

	as_label(label, sizeof label, name);
	sb_str(&s->after, " ");
	sb_str(&s->after, label);
}

/* Whether COMPOSE_PROFILES, comma-separated, names one of the service's. */
static int profile_active(const svc *s)
{
	const char *active = getenv("COMPOSE_PROFILES");
	char        list[512];
	char       *save;

	if (active == NULL)
		return 0;
	snprintf(list, sizeof list, "%s", active);
	for (char *w = strtok_r(list, ",", &save); w != NULL; w = strtok_r(NULL, ",", &save))
		for (int i = 0; i < s->profile_count; i++)
			if (strcmp(s->profile[i], trim(w)) == 0 || strcmp(trim(w), "*") == 0)
				return 1;
	return 0;
}

/* `NAME=value` or `NAME: value`, a build's arg. */
static void svc_build_arg(svc *s, char *text, char separator)
{
	char *at = strchr(text, separator);

	if (at == NULL) {
		svc_note(s, "build arg with no value not translated", text);
		return;
	}
	if (s->build_arg_count == HULL_MAX_BUILD_ARGS) {
		svc_note(s, "build arg past the most kept not translated", text);
		return;
	}
	*at = '\0';
	snprintf(s->build_arg[s->build_arg_count].name, sizeof s->build_arg[0].name, "%s", trim(text));
	snprintf(s->build_arg[s->build_arg_count].value, sizeof s->build_arg[0].value, "%s",
	         scalar(at + 1));
	s->build_arg_count++;
}

/* A healthcheck's test: a flow list, a block list's first word, or a line. */
static void svc_test_word(svc *s, const char *word)
{
	if (s->test_count == 0) {
		s->health_given = 1;
		if (strcmp(word, "NONE") == 0) {
			s->health_off = 1;
			return;
		}
		if (strcmp(word, "CMD-SHELL") == 0) {
			s->health_shell = 1;
			s->test_count = -1;  /* the word is no word of the command */
		} else if (strcmp(word, "CMD") == 0) {
			s->test_count = -1;
		}
		if (s->test_count == -1) {
			s->test_count = 0;
			s->in_test = 2;      /* the marker is read; words follow */
			return;
		}
	}
	if (s->test_count < 16)
		snprintf(s->test[s->test_count++], sizeof s->test[0], "%s", word);
}

/* The buffered service, as a (name ...) machine, its closing paren on its
 * own line lest a trailing note comment it out. */
static void svc_flush(svc *s, sb *out)
{
	if (!s->open)
		return;
	/* As compose: a service with profiles runs only where one is asked
	 * for, by COMPOSE_PROFILES, which `hull up --profile` adds to. */
	if (s->profile_count > 0 && !profile_active(s)) {
		if (warnings != NULL)
			fprintf(warnings, "%s: %s: left out: none of its profiles is active\n", warned_path,
			        s->name);
		sb_str(out, "\n  ; ");
		sb_str(out, s->name);
		sb_str(out, " left out: none of its profiles is active");
		svc_reset(s);
		return;
	}
	/* An env_file's names, but those environment: gave, which win. */
	if (s->file_env.len > 0u) {
		char *save;

		for (char *w = strtok_r(s->file_env.data, "\n", &save); w != NULL;
		     w = strtok_r(NULL, "\n", &save)) {
			char   wanted[200];
			size_t n = strcspn(w, "=");

			snprintf(wanted, sizeof wanted, "(%.*s ", (int)n, w);
			if (s->env.data == NULL || strstr(s->env.data, wanted) == NULL)
				compose_env(s, w, 0);
		}
	}
	/* build: the service's image is what `hull up` makes of its Dockerfile,
	 * under a ref of the project's, even where an image: names it too. */
	if (s->build_context[0] != '\0' || s->build_dockerfile[0] != '\0') {
		if (build_count == MAX_COMPOSE_BUILDS) {
			svc_note(s, "build not translated", "too many services build");
		} else {
			hull_compose_build *b = &builds[build_count++];

			snprintf(b->ref, sizeof b->ref, "compose/%s/%s", build_project, s->name);
			snprintf(b->context, sizeof b->context, "%s",
			         s->build_context[0] != '\0' ? s->build_context : ".");
			snprintf(b->dockerfile, sizeof b->dockerfile, "%s",
			         s->build_dockerfile[0] != '\0' ? s->build_dockerfile : "Dockerfile");
			snprintf(b->target, sizeof b->target, "%s", s->build_target);
			memcpy(b->args, s->build_arg, sizeof b->args);
			b->arg_count = s->build_arg_count;
			snprintf(s->source, sizeof s->source, "%s", b->ref);
			svc_note(s, "built by hull up from its Dockerfile, as", b->ref);
		}
	}
	if (s->source[0] == '\0' && warnings != NULL) {
		fprintf(warnings, "%s: %s: no image to run it from\n", warned_path, s->name);
		unrunnable = 1;
	}
	{
		char label[64];

		as_label(label, sizeof label, s->name);
		if (strcmp(label, s->name) != 0) {
			char why[200];

			snprintf(why, sizeof why, "a machine's name is a DNS label, so this one is %s",
			         label);
			svc_note(s, why, NULL);
			snprintf(s->name, sizeof s->name, "%s", label);
		}
	}
	if (!s->mode_given && s->net_count == 0)
		svc_join(s, "default");
	sb_str(out, "\n  (");
	sb_str(out, s->name);
	sb_str(out, " (source ");
	sb_quoted(out, s->source[0] != '\0' ? s->source : "SET-A-SOURCE");
	sb_str(out, ")");
	/* A container never writes its image, its writes being a layer of its
	 * own: a directory named as the image is run so, as a stored image
	 * already is. */
	if (strncmp(s->source, "dir:", 4u) == 0)
		sb_str(out, " (mutable overlay)");
	if (s->command.len > 0u) {
		sb_str(out, " (command");
		sb_add(out, s->command.data, s->command.len);
		sb_str(out, ")");
	}
	if (s->env.len > 0u) {
		sb_str(out, " (env");
		sb_add(out, s->env.data, s->env.len);
		sb_str(out, ")");
	}
	/* docker discards published ports under host, and refuses them under
	 * none; hull refuses both, so they go, said, rather than leave a file
	 * that does not plan. */
	if (s->ports.len > 0u && (strcmp(s->network, "host") == 0 || strcmp(s->network, "none") == 0)) {
		svc_note(s, "ports not translated", strcmp(s->network, "host") == 0 ?
		         "network_mode host is the host's network, where a port is the host's own" :
		         "network_mode none has no network to put a port on");
	} else if (s->ports.len > 0u) {
		sb_str(out, " (ports");
		sb_add(out, s->ports.data, s->ports.len);
		sb_str(out, ")");
	}
	if (s->network[0] != '\0') {
		sb_str(out, " (network ");
		sb_str(out, s->network);
		sb_str(out, ")");
	}
	/* Each network as a mark the whole file's translation replaces with
	 * the segment, its name and subnet known only once the file is read. */
	if (!s->mode_given && s->net_count > 0) {
		sb_str(out, " (segments");
		for (int i = 0; i < s->net_count; i++) {
			int known = 0;

			while (known < file_net_count && strcmp(file_nets[known], s->nets[i]) != 0)
				known++;
			if (known == file_net_count && file_net_count < MAX_NETS * 8)
				snprintf(file_nets[file_net_count++], sizeof file_nets[0], "%s", s->nets[i]);
			sb_str(out, " \001");
			sb_str(out, s->nets[i]);
			sb_str(out, "\002");
		}
		sb_str(out, ")");
	}
	if (s->volumes.len > 0u) {
		sb_str(out, " (volumes");
		sb_add(out, s->volumes.data, s->volumes.len);
		sb_str(out, ")");
	}
	if (s->workdir[0] != '\0') {
		sb_str(out, " (workdir ");
		sb_quoted(out, s->workdir);
		sb_str(out, ")");
	}
	if (s->hostname[0] != '\0') {
		sb_str(out, " (hostname ");
		sb_quoted(out, s->hostname);
		sb_str(out, ")");
	}
	if (s->memory[0] != '\0') {
		sb_str(out, " (memory ");
		sb_quoted(out, s->memory);
		sb_str(out, ")");
	}
	if (s->read_only)
		sb_str(out, " (security (readonly_root true))");
	if (s->arch[0] != '\0') {
		sb_str(out, " (arch ");
		sb_str(out, s->arch);
		sb_str(out, ")");
	}
	if (s->devices.data != NULL) {
		sb_str(out, " (devices (node");
		sb_str(out, s->devices.data);
		sb_str(out, "))");
	}
	if (s->secrets.data != NULL) {
		sb_str(out, " (secrets");
		sb_str(out, s->secrets.data);
		sb_str(out, ")");
	}
	{
		const struct { const char *key; const sb *list; } lists[] = {
			{ "hosts", &s->hosts }, { "dns", &s->dns }, { "sysctls", &s->sysctls },
			{ "ulimits", &s->ulimits },
		};

		for (size_t i = 0u; i < sizeof lists / sizeof lists[0]; i++)
			if (lists[i].list->data != NULL) {
				sb_str(out, " (");
				sb_str(out, lists[i].key);
				sb_str(out, lists[i].list->data);
				sb_str(out, ")");
			}
	}
	if (s->tmpfs.data != NULL) {
		sb_str(out, " (tmpfs");
		sb_str(out, s->tmpfs.data);
		sb_str(out, ")");
	}
	if (s->shm_size[0] != '\0') {
		sb_str(out, " (shm_size ");
		sb_quoted(out, s->shm_size);
		sb_str(out, ")");
	}
	if (s->stop_signal[0] != '\0') {
		sb_str(out, " (stop_signal ");
		sb_quoted(out, s->stop_signal);
		sb_str(out, ")");
	}
	if (s->stop_ms > 0) {
		char grace[32];

		snprintf(grace, sizeof grace, "%lds", (s->stop_ms + 999) / 1000);
		sb_str(out, " (stop_grace ");
		sb_quoted(out, grace);
		sb_str(out, ")");
	}
	if (s->user[0] != '\0') {
		sb_str(out, " (user ");
		sb_quoted(out, s->user);
		sb_str(out, ")");
	}
	if (s->cpus[0] != '\0') {
		sb_str(out, " (cpus ");
		sb_str(out, s->cpus);
		sb_str(out, ")");
	}
	if (s->restart[0] != '\0') {
		sb_str(out, " (restart ");
		sb_str(out, s->restart);
		sb_str(out, ")");
	}
	if (s->after.len > 0u) {
		sb_str(out, " (after");
		sb_add(out, s->after.data, s->after.len);
		sb_str(out, ")");
	}
	/* A healthcheck as (ready ...): its test the command, its interval
	 * the interval, and the longest compose waits before calling the
	 * service unhealthy the timeout -- start_period, then retries checks
	 * each of an interval and at most a timeout. compose's defaults: 30s,
	 * 30s, 3 and 0. */
	if (s->health_given && !s->health_off && s->test_count > 0) {
		long interval = s->interval_ms >= 0 ? s->interval_ms : 30000;
		long each     = s->timeout_ms >= 0 ? s->timeout_ms : 30000;
		long retries  = s->retries >= 0 ? s->retries : 3;
		long start    = s->start_ms >= 0 ? s->start_ms : 0;
		long total    = start + retries * (interval + each);
		char text[64];

		sb_str(out, " (ready (command");
		if (s->health_shell)
			sb_str(out, " \"/bin/sh\" \"-c\"");
		for (int i = 0; i < s->test_count; i++) {
			sb_str(out, " ");
			sb_quoted(out, s->test[i]);
		}
		snprintf(text, sizeof text, ") (interval \"%lds\") (timeout \"%lds\"))",
		         interval < 1000 ? 1 : (interval + 999) / 1000,
		         total < 1000 ? 1 : (total + 999) / 1000);
		sb_str(out, text);
	}
	sb_add(out, s->notes.data ? s->notes.data : "", s->notes.len);
	sb_str(out, "\n  )");
	svc_reset(s);
}

/* The leading-space count, and the content after it. Tabs in the indent are
 * YAML-illegal; treat one as a space and let the shape fall where it may. */
static int indent_of(const char *line, const char **content)
{
	int n = 0;

	while (line[n] == ' ' || line[n] == '\t')
		n++;
	*content = line + n;
	return n;
}

/* A /24 under 10.160.0.0/11 for each of the file's networks, drawn from
 * its segment's name by FNV-1a so that it is the same each run, and the
 * next one along where two of the file's would be one, which a file may
 * not hold; and `text` with each network's mark made its segment. */
static char *segments(char *text, const char *project)
{
	unsigned subnet[MAX_NETS * 8];
	sb       out = { NULL, 0u, 0u, 0 };
	sb       notes = { NULL, 0u, 0u, 0 };

	for (int i = 0; i < file_net_count; i++) {
		char     form[160];
		char     segment[64];
		uint32_t h = 2166136261u;

		snprintf(form, sizeof form, "%.63s-%.63s", project, file_nets[i]);
		as_label(segment, sizeof segment, form);
		for (const char *c = segment; *c != '\0'; c++)
			h = (h ^ (uint8_t)*c) * 16777619u;
		subnet[i] = h % 8192u;
		for (int j = 0; j < i; j++)
			if (subnet[j] == subnet[i]) {
				subnet[i] = (subnet[i] + 1u) % 8192u;
				j = -1;
			}
	}
	for (const char *c = text; *c != '\0'; c++) {
		const char *end;
		char        net[64];
		char        segment[64];
		char        form[320];
		int         i = 0;

		if (*c != '\001' || (end = strchr(c, '\002')) == NULL) {
			sb_add(&out, c, 1u);
			continue;
		}
		snprintf(net, sizeof net, "%.*s", (int)(end - c - 1), c + 1);
		while (i < file_net_count && strcmp(file_nets[i], net) != 0)
			i++;
		snprintf(form, sizeof form, "%.63s-%.63s", project, net);
		as_label(segment, sizeof segment, form);
		{
			const struct declared *d = declared_of(net, 0);
			char                   v4[64];
			char                   v6[64] = "";

			/* Its ipam subnets where hull can take them, else drawn: IPv4
			 * under 10.160.0.0/11, and for enable_ipv6 a ULA /64 drawn by
			 * FNV-1a from the segment's name, as RFC 4193 draws a global
			 * id at random, so each is the same each run. */
			if (d != NULL && d->subnet[0] != '\0' && usable_subnet(d->subnet, 0))
				snprintf(v4, sizeof v4, "%s", d->subnet);
			else
				snprintf(v4, sizeof v4, "10.%u.%u.0/24",
				         160u + (i < file_net_count ? subnet[i] / 256u : 0u),
				         i < file_net_count ? subnet[i] % 256u : 0u);
			if (d != NULL && d->subnet6[0] != '\0' && usable_subnet(d->subnet6, 1)) {
				snprintf(v6, sizeof v6, "%s", d->subnet6);
			} else if (d != NULL && (d->ipv6 || d->subnet6[0] != '\0')) {
				uint64_t h = 14695981039346656037u;

				for (const char *p = segment; *p != '\0'; p++)
					h = (h ^ (uint8_t)*p) * 1099511628211u;
				snprintf(v6, sizeof v6, "fd%02x:%02x%02x:%02x%02x::/64",
				         (unsigned)(h >> 32 & 0xffu), (unsigned)(h >> 24 & 0xffu),
				         (unsigned)(h >> 16 & 0xffu), (unsigned)(h >> 8 & 0xffu),
				         (unsigned)(h & 0xffu));
			}
			/* Said once per network, at the head of the file: a comment in
			 * a machine's line would end the line. */
			for (int family = 0; d != NULL && family < 2; family++) {
				const char *given = family == 0 ? d->subnet : d->subnet6;
				char        line[300];

				if (given[0] == '\0' || usable_subnet(given, family) ||
				    (notes.data != NULL && strstr(notes.data, given) != NULL))
					continue;
				snprintf(line, sizeof line, "network %s: ipam subnet %s is not one hull takes, "
				         "so one is drawn", net, given);
				sb_str(&notes, "\n; ");
				sb_str(&notes, line);
				if (warnings != NULL)
					fprintf(warnings, "%s: %s\n", warned_path, line);
			}
			if (v6[0] != '\0')
				snprintf(form, sizeof form, "(%s (subnet \"%s\") (subnet6 \"%s\"))", segment, v4,
				         v6);
			else
				snprintf(form, sizeof form, "(%s (subnet \"%s\"))", segment, v4);
		}
		sb_str(&out, form);
		c = end;
	}
	free(text);
	if (out.bad || notes.bad) {
		fprintf(stderr, "hull: import: out of memory\n");
		free(out.data);
		free(notes.data);
		return NULL;
	}
	/* The notes after the opening `(machines`, with the file's others. */
	if (notes.data != NULL && strncmp(out.data, "(machines", 9u) == 0) {
		sb whole = { NULL, 0u, 0u, 0 };

		sb_str(&whole, "(machines");
		sb_str(&whole, notes.data);
		sb_str(&whole, out.data + 9);
		free(out.data);
		out = whole;
		if (out.bad) {
			fprintf(stderr, "hull: import: out of memory\n");
			free(out.data);
			free(notes.data);
			return NULL;
		}
	}
	free(notes.data);
	return out.data;
}

/* The compose project's name, as compose takes it where the file has no
 * `name:`: the name of the directory `path` is in. */
static void project_of(const char *path, char *out, size_t cap)
{
	char        *copy = realpath(path, NULL);
	const char  *slash;

	if (copy == NULL) {
		snprintf(out, cap, "compose");
		return;
	}
	*strrchr(copy, '/') = '\0';
	slash = strrchr(copy, '/');
	snprintf(out, cap, "%s", slash != NULL && slash[1] != '\0' ? slash + 1 : "compose");
	free(copy);
}

/* The compose file `file` as a (machines ...) form, allocated, or NULL
 * having said why; its networks' segments named for `project`. */
/* --- variables ----------------------------------------------------------- */

#define MAX_DOTENV 128u

static char     compose_dir[4096];   /* the file's, for .env and env_file */
static char     dotenv_name[MAX_DOTENV][64];
static char     dotenv_value[MAX_DOTENV][512];
static unsigned dotenv_count;

/* A `NAME=value` line of an env file, in place: 1 with `*name` and `*value`
 * set, a leading `export` and the value's quotes gone, or 0 for a blank,
 * a comment or no `=`. */
static int env_line(char *line, char **name, char **value)
{
	char  *text = trim(line);
	char  *eq;
	size_t n;

	text[strcspn(text, "\r\n")] = '\0';
	if (*text == '#' || (eq = strchr(text, '=')) == NULL)
		return 0;
	if (strncmp(text, "export ", 7u) == 0)
		text = trim(text + 7);
	*eq = '\0';
	*name = trim(text);
	*value = trim(eq + 1);
	n = strlen(*value);
	if (n >= 2u && ((*value)[0] == '"' || (*value)[0] == '\'') && (*value)[n - 1u] == (*value)[0]) {
		(*value)[n - 1u] = '\0';
		(*value)++;
	}
	return (*name)[0] != '\0';
}

/* The .env beside the compose file, as compose reads one. */
static void read_dotenv(void)
{
	char  path[4200];
	char  line[1024];
	FILE *file;

	dotenv_count = 0u;
	snprintf(path, sizeof path, "%s/.env", compose_dir);
	if ((file = fopen(path, "r")) == NULL)
		return;
	while (fgets(line, sizeof line, file) != NULL && dotenv_count < MAX_DOTENV) {
		char *name;
		char *value;

		if (env_line(line, &name, &value)) {
			snprintf(dotenv_name[dotenv_count], sizeof dotenv_name[0], "%s", name);
			snprintf(dotenv_value[dotenv_count], sizeof dotenv_value[0], "%s", value);
			dotenv_count++;
		}
	}
	fclose(file);
}

/* A variable's value: the environment's, then .env's, or NULL. */
static const char *compose_variable(const char *name)
{
	const char *value = getenv(name);

	if (value != NULL)
		return value;
	for (unsigned i = 0u; i < dotenv_count; i++)
		if (strcmp(dotenv_name[i], name) == 0)
			return dotenv_value[i];
	return NULL;
}

/* `line` with compose's interpolation done, written to `out`: $NAME,
 * ${NAME}, the :- - :? ? :+ + forms, and $$ for a dollar. Unset is empty,
 * and said, as compose says it. */
static void interpolate(const char *line, FILE *out)
{
	while (*line != '\0') {
		const char *end;
		const char *word = NULL;
		const char *close = NULL;
		const char *value;
		char        name[64];
		char        op = '\0';
		int         colon = 0;
		int         braced;
		size_t      n;

		if (*line != '$') {
			fputc(*line++, out);
			continue;
		}
		if (line[1] == '$') {
			fputc('$', out);
			line += 2;
			continue;
		}
		braced = line[1] == '{';
		end = line + 1 + braced;
		n = 0u;
		while ((isalnum((unsigned char)end[n]) || end[n] == '_') && n + 1u < sizeof name) {
			name[n] = end[n];
			n++;
		}
		name[n] = '\0';
		end += n;
		if (n == 0u || isdigit((unsigned char)name[0])) {
			fputc(*line++, out);
			continue;
		}
		if (braced) {
			int depth = 1;

			colon = *end == ':';
			if (strchr("-?+", end[colon]) != NULL && end[colon] != '\0') {
				op = end[colon];
				word = end + colon + 1;
			} else if (*end != '}') {
				fputc(*line++, out);
				continue;
			}
			for (close = end; *close != '\0'; close++) {
				if (close[0] == '$' && close[1] == '{')
					depth++;
				else if (*close == '}' && --depth == 0)
					break;
			}
			if (*close != '}') {
				fputc(*line++, out);
				continue;
			}
		}
		value = compose_variable(name);
		{
			int set = value != NULL && (!colon || value[0] != '\0');

			if (op == '-' && !set) {
				char inner[1024];

				snprintf(inner, sizeof inner, "%.*s", (int)(close - word), word);
				interpolate(inner, out);
			} else if (op == '+') {
				if (set)
					fprintf(out, "%.*s", (int)(close - word), word);
			} else if (op == '?' && !set) {
				if (warnings != NULL)
					fprintf(warnings, "%s: required variable %s: %.*s\n", warned_path, name,
					        (int)(close - word), word);
				unrunnable = 1;
			} else if (value != NULL) {
				fputs(value, out);
			} else if (warnings != NULL) {
				fprintf(warnings, "%s: the variable %s is not set, and is empty\n", warned_path,
				        name);
			}
		}
		line = braced ? close + 1 : end;
	}
}

/* `file`, read to its end, as the file compose would parse: a new stream
 * the caller closes, or `file` itself if memory ran out. */
static FILE *interpolated(FILE *file, char **held)
{
	char   line[LINE_CAP];
	char  *text = NULL;
	size_t length = 0u;
	FILE  *out = open_memstream(&text, &length);
	FILE  *in;

	if (out == NULL)
		return file;
	read_dotenv();
	while (fgets(line, sizeof line, file) != NULL) {
		if (*trim(line) == '#')
			continue;
		interpolate(line, out);
	}
	fclose(out);
	if (length == 0u || (in = fmemopen(text, length, "r")) == NULL) {
		free(text);
		return tmpfile();
	}
	*held = text;   /* the stream's own bytes, the caller's to free after it */
	return in;
}

/* `path`'s directory, for what the file names beside it. */
static void set_compose_dir(const char *path)
{
	const char *slash = strrchr(path, '/');

	if (slash == NULL)
		snprintf(compose_dir, sizeof compose_dir, ".");
	else
		snprintf(compose_dir, sizeof compose_dir, "%.*s", (int)(slash - path), path);
}

/* An env_file:, relative to the compose file, into the service's env. */
static void compose_env_file(svc *s, const char *path)
{
	char  full[4600];
	char  line[1024];
	FILE *file;

	snprintf(full, sizeof full, "%s%s%s", path[0] == '/' ? "" : compose_dir,
	         path[0] == '/' ? "" : "/", path);
	if ((file = fopen(full, "r")) == NULL) {
		svc_note(s, "env_file not read", path);
		return;
	}
	while (fgets(line, sizeof line, file) != NULL) {
		char *name;
		char *value;
		char  entry[800];

		if (env_line(line, &name, &value)) {
			snprintf(entry, sizeof entry, "%s=%s\n", name, value);
			sb_str(&s->file_env, entry);
		}
	}
	fclose(file);
}

/* mem_limit, `256m` or bytes, as a spec's size, `256M`. */
static void compose_memory(svc *s, const char *text)
{
	char         *end;
	unsigned long n = strtoul(text, &end, 10);
	char          unit = (char)toupper((unsigned char)*end);

	if (end == text || (*end != '\0' && strchr("KMG", unit) == NULL && unit != 'B')) {
		svc_note(s, "mem_limit not translated", text);
		return;
	}
	if (*end == '\0' || unit == 'B')
		snprintf(s->memory, sizeof s->memory, "%luM", n / (1024u * 1024u) != 0u ?
		         n / (1024u * 1024u) : 1u);
	else
		snprintf(s->memory, sizeof s->memory, "%lu%c", n, unit);
}

/* A cpus: value, whole, or noted. */
static void compose_cpus(svc *s, const char *text)
{
	if (text[0] != '\0' && strspn(text, "0123456789") == strlen(text) &&
	    strtol(text, NULL, 10) > 0)
		snprintf(s->cpus, sizeof s->cpus, "%s", text);
	else
		svc_note(s, "cpus of a fraction not translated", text);
}

/* An entry under deploy:. Its limits are the machine's; a reservation asks
 * for what hull has no way to promise, so it is said and left. */
static void compose_deploy(svc *s, char *content)
{
	char *colon = strchr(content, ':');
	char *key = content;
	char *value;

	if (colon == NULL)
		return;
	*colon = '\0';
	key = trim(key);
	value = scalar(trim(colon + 1));
	if (strcmp(key, "resources") == 0)
		return;
	if (strcmp(key, "limits") == 0) {
		s->in_reservations = 0;
	} else if (strcmp(key, "reservations") == 0) {
		s->in_reservations = 1;
	} else if (s->in_reservations) {
		svc_note(s, "deploy reservation not translated", key);
	} else if (strcmp(key, "memory") == 0) {
		compose_memory(s, value);
	} else if (strcmp(key, "cpus") == 0) {
		compose_cpus(s, value);
	} else {
		svc_note(s, "deploy key not translated", key);
	}
}

/* Whether a compose file's top-level secrets: gives `name` a file: its path
 * into `path`. Read from the file before its services, which come first. */
static char secret_files[16][2][256];
static unsigned secret_file_count;

static void compose_secrets_scan(FILE *file)
{
	char line[LINE_CAP];
	int  in_secrets = 0;
	char current[256] = "";

	secret_file_count = 0u;
	while (fgets(line, sizeof line, file) != NULL) {
		int   indented = line[0] == ' ' || line[0] == '\t';
		char *content;
		char *colon;

		line[strcspn(line, "\r\n")] = '\0';
		content = trim(line);
		colon = strchr(content, ':');

		if (!indented && content[0] != '\0' && content[0] != '#') {
			in_secrets = strncmp(content, "secrets:", 8u) == 0;
			continue;
		}
		if (!in_secrets || colon == NULL)
			continue;
		*colon = '\0';
		if (strcmp(trim(content), "file") == 0 && current[0] != '\0' && secret_file_count < 16u) {
			snprintf(secret_files[secret_file_count][0], 256u, "%s", current);
			snprintf(secret_files[secret_file_count][1], 256u, "%s", scalar(trim(colon + 1)));
			secret_file_count++;
		} else {
			snprintf(current, sizeof current, "%s", trim(content));
		}
	}
	rewind(file);
}

static void compose_secret(svc *s, const char *name)
{
	for (unsigned i = 0u; i < secret_file_count; i++)
		if (strcmp(secret_files[i][0], name) == 0) {
			char source[300];

			sb_str(&s->secrets, " (");
			sb_str(&s->secrets, name);
			sb_str(&s->secrets, " (source ");
			snprintf(source, sizeof source, "file:%s", secret_files[i][1]);
			sb_quoted(&s->secrets, source);
			sb_str(&s->secrets, "))");
			svc_note(s, "secret at /run/hull/secrets, not /run/secrets", name);
			return;
		}
	svc_note(s, "secret with no file: not translated", name);
}

/* `/dev/x` or `/dev/x:/dev/y:rwm`: hull's node, the guest path its own. */
static void compose_device(svc *s, char *spec)
{
	char *colon = strchr(spec, ':');

	if (colon != NULL) {
		if (strncmp(colon + 1, spec, (size_t)(colon - spec)) != 0)
			svc_note(s, "device at another path not translated", spec);
		*colon = '\0';
	}
	sb_str(&s->devices, " ");
	sb_quoted(&s->devices, spec);
}

/* no-new-privileges is what every machine has; the rest is docker's. */
static void compose_security_opt(svc *s, const char *word)
{
	if (strcmp(word, "no-new-privileges:true") != 0 && strcmp(word, "no-new-privileges") != 0)
		svc_note(s, "security_opt not translated", word);
}

static char *compose_text(FILE *file, const char *project)
{
	svc    s = { 0 };
	sb     out = { NULL, 0u, 0u, 0 };
	sb     top_notes = { NULL, 0u, 0u, 0 };
	char   line[LINE_CAP];
	int    in_services = 0;
	int    svc_indent = -1;
	int    field_indent = -1;
	char   field[64] = "";
	int    any = 0;
	char  *text;
	char   name[64];
	int    in_networks = 0;
	int    network_indent = -1;
	int    key_indent = -1;
	int    in_ipam = 0;
	char   network[64] = "";

	file_net_count = 0;
	declared_count = 0;
	build_count = 0u;
	as_label(name, sizeof name, project);
	snprintf(build_project, sizeof build_project, "%s", name);

	/* Not from a pipe: it cannot be read twice, and its secrets are noted. */
	secret_file_count = 0u;
	if (ftell(file) >= 0)
		compose_secrets_scan(file);
	while (fgets(line, sizeof line, file) != NULL) {
		const char *c;
		int         ind;
		char       *content;
		char       *colon;
		char       *key;
		char       *value;

		line[strcspn(line, "\n")] = '\0';
		ind = indent_of(line, &c);
		if (*c == '\0' || *c == '#')
			continue;
		content = (char *)c;
		if (ind == 0) {               /* a top-level key */
			svc_flush(&s, &out);
			in_services = 0;
			in_networks = 0;
			colon = strchr(content, ':');
			if (colon != NULL && strncmp(content, "services:", 9u) == 0) {
				in_services = 1;
			} else if (colon != NULL && strncmp(content, "networks:", 9u) == 0) {
				in_networks = 1;
				network_indent = -1;
			} else if (colon != NULL && strncmp(content, "name:", 5u) == 0) {
				as_label(name, sizeof name, scalar(colon + 1));
				snprintf(build_project, sizeof build_project, "%s", name);
			} else if (colon != NULL) {
				*colon = '\0';
				/* A top-level volumes: declares the names a service's
				 * volumes then keep. */
				if (strcmp(content, "version") != 0 && strcmp(content, "volumes") != 0 &&
				    strcmp(content, "secrets") != 0) {
					sb_str(&top_notes, "\n; top-level key not translated: ");
					sb_str(&top_notes, content);
					if (warnings != NULL)
						fprintf(warnings, "%s: top-level key not translated: %s\n",
						        warned_path, content);
				}
			}
			continue;
		}
		/* A network's options are docker's to give it; a segment has
		 * none, so each is said, `external` above all, which would join a
		 * network made elsewhere. */
		if (in_networks) {
			char *item = content;
			char *value;

			if (*item == '-')
				item = trim(item + 1);
			colon = strchr(item, ':');
			if (network_indent < 0)
				network_indent = ind;
			if (colon != NULL)
				*colon = '\0';
			value = colon != NULL ? scalar(colon + 1) : (char *)"";
			if (ind == network_indent) {
				snprintf(network, sizeof network, "%s", trim(item));
				declared_of(network, 1);
				key_indent = -1;
				in_ipam = 0;
				continue;
			}
			if (key_indent < 0)
				key_indent = ind;
			if (ind == key_indent)
				in_ipam = 0;
			/* IPv6 where enable_ipv6 asks it, and the subnets an ipam
			 * config gives; the rest said, as before. */
			if (ind == key_indent && strcmp(trim(item), "enable_ipv6") == 0) {
				struct declared *d = declared_of(network, 1);

				if (d != NULL)
					d->ipv6 = strcmp(value, "true") == 0;
			} else if (ind == key_indent && strcmp(trim(item), "ipam") == 0) {
				in_ipam = 1;
			} else if (in_ipam && (strcmp(trim(item), "config") == 0)) {
				;
			} else if (in_ipam && strcmp(trim(item), "subnet") == 0) {
				struct declared *d = declared_of(network, 1);

				if (d != NULL)
					snprintf(strchr(value, ':') != NULL ? d->subnet6 : d->subnet,
					         sizeof d->subnet, "%s", value);
			} else {
				sb_str(&top_notes, "\n; network ");
				sb_str(&top_notes, network);
				sb_str(&top_notes, in_ipam ? ": ipam " : ": ");
				sb_str(&top_notes, trim(item));
				sb_str(&top_notes, " not translated");
				if (warnings != NULL)
					fprintf(warnings, "%s: network %s: %s%s not translated\n", warned_path,
					        network, in_ipam ? "ipam " : "", trim(item));
			}
			continue;
		}
		if (!in_services)
			continue;
		if (svc_indent < 0)
			svc_indent = ind;
		colon = strchr(content, ':');
		if (ind == svc_indent && *content != '-') {   /* a service name */
			svc_flush(&s, &out);
			if (colon != NULL)
				*colon = '\0';
			snprintf(s.name, sizeof s.name, "%s", trim(content));
			s.open = 1;
			s.net_indent  = -1;
			s.dep_indent  = -1;
			s.args_indent = -1;
			s.interval_ms = -1;
			s.timeout_ms  = -1;
			s.retries     = -1;
			s.start_ms    = -1;
			any = 1;
			field_indent = -1;
			field[0] = '\0';
			continue;
		}
		if (!s.open)
			continue;
		if (*content == '-') {        /* a block-list item of `field` */
			char *item = trim(content + 1);

			if (strcmp(field, "environment") == 0)
				compose_env(&s, item, 0);
			else if (strcmp(field, "ports") == 0)
				compose_port(&s, item);
			else if (strcmp(field, "volumes") == 0)
				compose_volume(&s, item);
			else if (strcmp(field, "command") == 0 || strcmp(field, "entrypoint") == 0) {
				sb_str(&s.command, " ");
				sb_quoted(&s.command, scalar(item));
			} else if (strcmp(field, "networks") == 0)
				svc_join(&s, scalar(item));
			else if (strcmp(field, "depends_on") == 0)
				svc_after(&s, scalar(item));
			else if (strcmp(field, "healthcheck") == 0 && s.in_test)
				svc_test_word(&s, scalar(item));
			else if (strcmp(field, "build") == 0 && s.args_indent >= 0)
				svc_build_arg(&s, item, '=');
			else if (strcmp(field, "env_file") == 0)
				compose_env_file(&s, scalar(item));
			else if (strcmp(field, "tmpfs") == 0)
				compose_tmpfs(&s, scalar(item));
			else if (strcmp(field, "devices") == 0)
				compose_device(&s, scalar(item));
			else if (strcmp(field, "secrets") == 0)
				compose_secret(&s, scalar(item));
			else if (strcmp(field, "security_opt") == 0)
				compose_security_opt(&s, scalar(item));
			else if (strcmp(field, "extra_hosts") == 0)
				compose_host(&s, scalar(item));
			else if (strcmp(field, "dns") == 0)
				compose_word(&s.dns, scalar(item));
			else if (strcmp(field, "sysctls") == 0)
				compose_word(&s.sysctls, scalar(item));
			else if (strcmp(field, "profiles") == 0 && s.profile_count < 8)
				snprintf(s.profile[s.profile_count++], sizeof s.profile[0], "%s", scalar(item));
			continue;
		}
		if (field_indent < 0)
			field_indent = ind;
		if (ind > field_indent && field[0] != '\0') {   /* a map entry of `field` */
			if (strcmp(field, "environment") == 0)
				compose_env(&s, content, 1);
			else if (strcmp(field, "extra_hosts") == 0 || strcmp(field, "sysctls") == 0 ||
			         strcmp(field, "ulimits") == 0) {
				compose_pair(&s, field, content);
			} else if (strcmp(field, "deploy") == 0) {
				compose_deploy(&s, content);
			} else if (strcmp(field, "networks") == 0) {
				/* A networks map: each name, its options under it, an
				 * alias or an address, which a segment does not take. */
				char *at = strchr(content, ':');

				if (at != NULL)
					*at = '\0';
				if (s.net_indent < 0)
					s.net_indent = ind;
				if (ind == s.net_indent) {
					svc_join(&s, trim(content));
					snprintf(s.net_current, sizeof s.net_current, "%s", trim(content));
				} else {
					char why[200];

					snprintf(why, sizeof why, "network %s: %s not translated", s.net_current,
					         trim(content));
					svc_note(&s, why, NULL);
				}
			} else if (strcmp(field, "depends_on") == 0) {
				/* A depends_on map: each name, and under it its
				 * condition, which (after ...) is but for one. */
				char *at = strchr(content, ':');
				char *option = at != NULL ? scalar(at + 1) : "";

				if (at != NULL)
					*at = '\0';
				if (s.dep_indent < 0)
					s.dep_indent = ind;
				if (ind == s.dep_indent) {
					svc_after(&s, trim(content));
					snprintf(s.dep_current, sizeof s.dep_current, "%s", trim(content));
				} else if (strcmp(trim(content), "condition") == 0 &&
				           strcmp(option, "service_completed_successfully") == 0) {
					char why[200];

					snprintf(why, sizeof why, "depends_on %s: service_completed_successfully "
					         "not translated, its start waited for instead", s.dep_current);
					svc_note(&s, why, NULL);
				} else if (strcmp(trim(content), "condition") != 0 &&
				           !(strcmp(trim(content), "required") == 0 &&
				             strcmp(option, "true") == 0)) {
					char why[200];

					snprintf(why, sizeof why, "depends_on %s: %s not translated", s.dep_current,
					         trim(content));
					svc_note(&s, why, NULL);
				}
			} else if (strcmp(field, "build") == 0) {
				/* build: as a block: its context, its Dockerfile, and
				 * the rest said; args and target change what is built. */
				char *at = strchr(content, ':');

				if (s.args_indent >= 0 && ind > s.args_indent) {
					svc_build_arg(&s, content, ':');
					continue;
				}
				s.args_indent = -1;
				if (at != NULL)
					*at = '\0';
				if (at != NULL && strcmp(trim(content), "args") == 0 && *trim(at + 1) == '\0')
					s.args_indent = ind;
				else if (at != NULL && strcmp(trim(content), "target") == 0)
					snprintf(s.build_target, sizeof s.build_target, "%s", scalar(at + 1));
				else if (at != NULL && strcmp(trim(content), "context") == 0)
					snprintf(s.build_context, sizeof s.build_context, "%s", scalar(at + 1));
				else if (at != NULL && strcmp(trim(content), "dockerfile") == 0)
					snprintf(s.build_dockerfile, sizeof s.build_dockerfile, "%s", scalar(at + 1));
				else
					svc_note(&s, "build key not translated", trim(content));
			} else if (strcmp(field, "healthcheck") == 0) {
				char *at = strchr(content, ':');
				char *value_text;
				char *words[16];
				int   n;

				if (at == NULL)
					continue;
				*at = '\0';
				value_text = trim(at + 1);
				s.in_test = 0;
				if (strcmp(trim(content), "test") == 0) {
					if (*value_text == '\0') {
						s.in_test = 1;
					} else if ((n = flow_list(value_text, words, 16)) >= 0) {
						s.in_test = 1;
						for (int i = 0; i < n; i++)
							svc_test_word(&s, words[i]);
						s.in_test = 0;
					} else {
						s.health_given = 1;
						s.health_shell = 1;
						s.test_count   = 1;
						snprintf(s.test[0], sizeof s.test[0], "%s", scalar(value_text));
					}
				} else if (strcmp(trim(content), "disable") == 0) {
					s.health_off = strcmp(scalar(value_text), "true") == 0;
				} else if (strcmp(trim(content), "interval") == 0) {
					s.interval_ms = compose_ms(scalar(value_text));
				} else if (strcmp(trim(content), "timeout") == 0) {
					s.timeout_ms = compose_ms(scalar(value_text));
				} else if (strcmp(trim(content), "start_period") == 0) {
					s.start_ms = compose_ms(scalar(value_text));
				} else if (strcmp(trim(content), "retries") == 0) {
					s.retries = strtol(scalar(value_text), NULL, 10);
				} else {
					char why[200];

					snprintf(why, sizeof why, "healthcheck %s not translated", trim(content));
					svc_note(&s, why, NULL);
				}
			}
			continue;
		}
		/* a field: key and its value, which may be empty (a block follows). */
		if (colon == NULL)
			continue;
		*colon = '\0';
		key = trim(content);
		value = trim(colon + 1);
		snprintf(field, sizeof field, "%s", key);
		if (value[0] == '\0') {
			/* A block list or map follows, which only these are read
			 * under: another goes unread, and is said. */
			static const char *const blocks[] = {
				"environment", "ports", "volumes", "command", "entrypoint", "networks",
				"depends_on", "healthcheck", "build", "profiles", "env_file", "tmpfs",
				"extra_hosts", "dns", "sysctls", "ulimits", "deploy", "devices", "secrets",
				"security_opt", NULL
			};
			int known = 0;

			for (const char *const *b = blocks; *b != NULL; b++)
				known |= strcmp(*b, key) == 0;
			if (!known)
				svc_note(&s, "key not translated", key);
			continue;
		}
		field[0] = '\0';          /* a scalar or flow value, handled now */
		if (strcmp(key, "image") == 0)
			snprintf(s.source, sizeof s.source, "%s", scalar(value));
		else if (strcmp(key, "working_dir") == 0)
			snprintf(s.workdir, sizeof s.workdir, "%s", scalar(value));
		else if (strcmp(key, "hostname") == 0 || strcmp(key, "container_name") == 0)
			snprintf(s.hostname, sizeof s.hostname, "%s", scalar(value));
		else if (strcmp(key, "restart") == 0) {
			char *r = scalar(value);

			if (strcmp(r, "unless-stopped") == 0) {
				snprintf(s.restart, sizeof s.restart, "always");
				svc_note(&s, "restart unless-stopped mapped to always", NULL);
			} else if (strcmp(r, "no") == 0 || strcmp(r, "always") == 0 ||
			           strcmp(r, "on-failure") == 0)
				snprintf(s.restart, sizeof s.restart, "%s", r);
			else
				svc_note(&s, "restart value not translated", r);
		} else if (strcmp(key, "command") == 0 || strcmp(key, "entrypoint") == 0) {
			char *words[64];
			int   n = flow_list(value, words, 64);

			if (n < 0) {
				sb_str(&s.command, " \"/bin/sh\" \"-c\" ");
				sb_quoted(&s.command, scalar(value));
			} else
				for (int i = 0; i < n; i++) {
					sb_str(&s.command, " ");
					sb_quoted(&s.command, words[i]);
				}
		} else if (strcmp(key, "ports") == 0 || strcmp(key, "environment") == 0 ||
		           strcmp(key, "volumes") == 0) {
			char *items[64];
			int   n = flow_list(value, items, 64);

			for (int i = 0; i < n; i++) {
				if (strcmp(key, "ports") == 0)
					compose_port(&s, items[i]);
				else if (strcmp(key, "volumes") == 0)
					compose_volume(&s, items[i]);
				else
					compose_env(&s, items[i], 0);
			}
			if (n < 0)
				svc_note(&s, key, "value not translated");
		} else if (strcmp(key, "profiles") == 0) {
			char *items[8];
			int   n = flow_list(value, items, 8);

			for (int i = 0; i < n; i++)
				snprintf(s.profile[s.profile_count++], sizeof s.profile[0], "%s", items[i]);
		} else if (strcmp(key, "build") == 0)
			snprintf(s.build_context, sizeof s.build_context, "%s", scalar(value));
		else if (strcmp(key, "networks") == 0) {
			char *items[MAX_NETS];
			int   n = flow_list(value, items, MAX_NETS);

			for (int i = 0; i < n; i++)
				svc_join(&s, items[i]);
			if (n < 0)
				svc_note(&s, "networks not translated", scalar(value));
		} else if (strcmp(key, "depends_on") == 0) {
			char *items[64];
			int   n = flow_list(value, items, 64);

			for (int i = 0; i < n; i++)
				svc_after(&s, items[i]);
			if (n < 0)
				svc_note(&s, "depends_on not translated", scalar(value));
		} else if (strcmp(key, "healthcheck") == 0)
			svc_note(&s, "healthcheck not translated", "a flow map; write it as a block");
		else if (strcmp(key, "env_file") == 0) {
			char *items[16];
			int   n = flow_list(value, items, 16);

			if (n < 0)
				compose_env_file(&s, scalar(value));
			for (int i = 0; i < n; i++)
				compose_env_file(&s, items[i]);
		} else if (strcmp(key, "mem_limit") == 0) {
			compose_memory(&s, scalar(value));
		} else if (strcmp(key, "cpus") == 0) {
			compose_cpus(&s, scalar(value));
		} else if (strcmp(key, "user") == 0) {
			snprintf(s.user, sizeof s.user, "%s", scalar(value));
		} else if (strcmp(key, "shm_size") == 0) {
			char saved[sizeof s.memory];

			/* memory's own reading of a size, kept apart from it. */
			snprintf(saved, sizeof saved, "%s", s.memory);
			compose_memory(&s, scalar(value));
			snprintf(s.shm_size, sizeof s.shm_size, "%s", s.memory);
			snprintf(s.memory, sizeof s.memory, "%s", saved);
		} else if (strcmp(key, "stop_signal") == 0) {
			snprintf(s.stop_signal, sizeof s.stop_signal, "%s", scalar(value));
		} else if (strcmp(key, "stop_grace_period") == 0) {
			long ms = compose_ms(scalar(value));

			if (ms >= 1000)
				s.stop_ms = ms;
			else
				svc_note(&s, "stop_grace_period not translated", scalar(value));
		} else if (strcmp(key, "tmpfs") == 0 || strcmp(key, "dns") == 0 ||
		           strcmp(key, "devices") == 0 || strcmp(key, "secrets") == 0 ||
		           strcmp(key, "security_opt") == 0) {
			/* One value, or a flow list of them: `dns: [1.1.1.1, 8.8.8.8]`. */
			char *rest = value[0] == '[' ? value + 1 : value;
			char *stop = value[0] == '[' ? strrchr(value, ']') : NULL;

			if (stop != NULL)
				*stop = '\0';
			for (char *item = strtok(rest, ","); item != NULL; item = strtok(NULL, ",")) {
				char *word = scalar(trim(item));

				if (key[0] == 't')
					compose_tmpfs(&s, word);
				else if (strcmp(key, "dns") == 0)
					compose_word(&s.dns, word);
				else if (key[0] == 'd')
					compose_device(&s, word);
				else if (strcmp(key, "secrets") == 0)
					compose_secret(&s, word);
				else
					compose_security_opt(&s, word);
			}
		} else if (strcmp(key, "platform") == 0) {
			char *slash = strchr(scalar(value), '/');

			if (slash != NULL && strchr(slash + 1, '/') == NULL)
				snprintf(s.arch, sizeof s.arch, "%s", slash + 1);
			else
				svc_note(&s, "platform not translated", scalar(value));
		} else if (strcmp(key, "read_only") == 0) {
			s.read_only = strcmp(scalar(value), "true") == 0;
		} else if (strcmp(key, "init") == 0 || strcmp(key, "expose") == 0) {
			/* Said by every machine already: a PID 1 that reaps, on every
			 * engine, and an EXPOSE that publishes nothing. */
		} else if (strcmp(key, "privileged") == 0) {
			if (strcmp(scalar(value), "false") != 0)
				svc_note(&s, "privileged not translated", scalar(value));
		} else if (strcmp(key, "network_mode") == 0) {
			/* bridge is docker's own network, which hull's nat names on
			 * every engine; host and none are hull's as they are. Another
			 * container's network, service: or container:, hull has no
			 * way to share. */
			char *mode = scalar(value);

			s.mode_given = 1;
			if (strcmp(mode, "bridge") == 0) {
				snprintf(s.network, sizeof s.network, "nat");
				svc_note(&s, "network_mode bridge mapped to nat",
				         "(network bridge) is hull's own bridge, its core run as root");
			}
			else if (strcmp(mode, "host") == 0 || strcmp(mode, "none") == 0)
				snprintf(s.network, sizeof s.network, "%s", mode);
			else
				svc_note(&s, "network_mode not translated", mode);
		}
		else
			svc_note(&s, "key not translated", key);
	}
	svc_flush(&s, &out);
	if (!any) {
		fprintf(stderr, "hull: import: no services: found; is this a compose file?\n");
		free(out.data);
		free(top_notes.data);
		return NULL;
	}
	if (out.bad || top_notes.bad ||
	    asprintf(&text, "(machines%s%s\n)\n", top_notes.data ? top_notes.data : "",
	             out.data) < 0) {
		fprintf(stderr, "hull: import: out of memory\n");
		text = NULL;
	}
	free(out.data);
	free(top_notes.data);
	return text != NULL ? segments(text, name) : NULL;
}

static int import_compose(FILE *file, const char *project)
{
	char *text = compose_text(file, project);

	if (text == NULL)
		return 2;
	fputs(text, stdout);
	free(text);
	return 0;
}

int hull_import_is_compose(const char *path)
{
	const char *base = strrchr(path, '/');
	const char *dot;

	base = base != NULL ? base + 1 : path;
	dot = strrchr(base, '.');
	return dot != NULL && (strcmp(dot, ".yml") == 0 || strcmp(dot, ".yaml") == 0);
}

char *hull_import_compose(const char *path, FILE *notes)
{
	FILE *file = fopen(path, "r");
	char *text;
	char *held = NULL;
	char  project[256];

	if (file == NULL) {
		perror(path);
		return NULL;
	}
	project_of(path, project, sizeof project);
	warnings = notes;
	warned_path = path;
	unrunnable = 0;
	set_compose_dir(path);
	{
		FILE *read = interpolated(file, &held);

		if (read != file) {
			fclose(file);
			file = read;
		}
	}
	text = compose_text(file, project);
	fclose(file);
	free(held);
	warnings = NULL;
	if (text != NULL && unrunnable) {
		fprintf(notes != NULL ? notes : stderr,
		        "%s: a service with no image does not run: hull runs one from an image "
		        "or a directory\n", path);
		free(text);
		text = NULL;
	}
	return text;
}

/* Dockerfile or compose, told apart by the file's name: a compose file ends
 * .yml or .yaml or is named for compose, anything else is a Dockerfile. */
int hull_import_build(const char *path, const hull_build_arg *args, unsigned count,
                      const char *target)
{
	int status;

	given_args   = args;
	given_count  = count;
	given_target = target;
	status       = hull_import(path);
	given_args   = NULL;
	given_count  = 0u;
	given_target = NULL;
	return status;
}

int hull_import(const char *path)
{
	const char *base = strrchr(path, '/');
	FILE       *file;
	char       *held = NULL;
	int         compose;
	int         status;

	/* `hull import` also takes a compose file by a name with compose in
	 * it, which a spec reader does not, a spec's own name being free. */
	base = base != NULL ? base + 1 : path;
	compose = hull_import_is_compose(path) || strstr(base, "compose") != NULL;

	if ((file = fopen(path, "r")) == NULL) {
		fprintf(stderr, "hull: import: %s: cannot open\n", path);
		return 2;
	}
	if (compose) {
		char project[256];

		FILE *read;

		project_of(path, project, sizeof project);
		set_compose_dir(path);
		read = interpolated(file, &held);
		if (read != file) {
			fclose(file);
			file = read;
		}
		status = import_compose(file, project);
	} else {
		read_dockerignore(path);
		status = import_dockerfile(file);
	}
	fclose(file);
	free(held);
	return status;
}
