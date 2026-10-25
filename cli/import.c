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
 * Only a single stage is translated: a second FROM ends it with a comment,
 * since multi-stage COPY --from has no (build ...) shape. The compose and
 * foreign-image imports the CLI sketch names are not written.
 */
#define _GNU_SOURCE

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "import.h"

#define MAX_ENV   128
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

typedef struct build {
	sb       steps;        /* each (step ...) as it is met */
	char     env_name[MAX_ENV][256];
	char     env_value[MAX_ENV][1024];
	unsigned env_count;
	char     workdir[4096];   /* "" until a WORKDIR, then absolute */
	int      had_from;
	int      done;            /* a second FROM ended the stage */
	unsigned step_no;
} build;

/* ENV accumulated so far, as the entries of an (env ...) record, and the
 * current (workdir ...) -- a RUN step gets both, since a Dockerfile's ENV
 * and WORKDIR reach every later instruction. */
static void step_context(build *b, sb *out)
{
	if (b->env_count > 0u) {
		sb_str(out, " (env");
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
	sb_str(&b->steps, ")");
	step_context(b, &b->steps);
	sb_str(&b->steps, ")");
}

/* A COPY or ADD: a step whose copies are its sources to the destination,
 * each made absolute against the workdir. ADD's url and tar-extract, and a
 * --from or --chown flag, are noted rather than done. */
static void do_copy(build *b, const char *instr, char *arg)
{
	char  name[32];
	char *words[64];
	int   n = 0;
	char *save;
	char *dst;

	for (char *w = strtok_r(trim(arg), " \t", &save); w != NULL && n < 64;
	     w = strtok_r(NULL, " \t", &save)) {
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
	if (strcmp(instr, "ADD") == 0)
		note(&b->steps, "ADD treated as COPY", "its url fetch and tar extraction are not done");
	dst = words[n - 1];
	snprintf(name, sizeof name, "copy-%u", ++b->step_no);
	sb_str(&b->steps, "\n    (step (name \"");
	sb_str(&b->steps, name);
	sb_str(&b->steps, "\") (copy");
	for (int i = 0; i < n - 1; i++) {
		char to[8192];

		if (dst[0] == '/' || b->workdir[0] == '\0')
			snprintf(to, sizeof to, "%s", dst);
		else
			snprintf(to, sizeof to, "%s/%s", b->workdir, dst);
		sb_str(&b->steps, " (path (from ");
		sb_quoted(&b->steps, words[i]);
		sb_str(&b->steps, ") (to ");
		sb_quoted(&b->steps, to);
		sb_str(&b->steps, "))");
	}
	sb_str(&b->steps, ")");
	if (b->workdir[0] != '\0') {
		sb_str(&b->steps, " (workdir ");
		sb_quoted(&b->steps, b->workdir);
		sb_str(&b->steps, ")");
	}
	sb_str(&b->steps, ")");
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

int hull_import(const char *path)
{
	FILE  *file = fopen(path, "r");
	build  b = { { NULL, 0u, 0u, 0 }, { { 0 } }, { { 0 } }, 0u, "", 0, 0, 0u };
	sb     rt = { NULL, 0u, 0u, 0 };
	sb     after = { NULL, 0u, 0u, 0 };   /* CMD/ENTRYPOINT/etc. order kept */
	char  *base = NULL;
	char   line[LINE_CAP];
	char   logical[LINE_CAP];
	size_t at = 0u;
	int    status = 0;

	if (file == NULL) {
		fprintf(stderr, "hull: import: %s: cannot open\n", path);
		return 2;
	}
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
			if (b.done && strcmp(instr, "FROM") != 0) {
				continue;   /* past the first stage */
			}
			if (strcmp(instr, "FROM") == 0) {
				char *as;

				if (b.had_from) {
					note(&b.steps, "multi-stage not translated", "only the first FROM's stage");
					b.done = 1;
					continue;
				}
				b.had_from = 1;
				if ((as = strstr(s, " AS ")) != NULL || (as = strstr(s, " as ")) != NULL)
					*as = '\0';
				base = strdup(trim(s));
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
				note(&b.steps, "USER set on the image; a RUN is not run as it", trim(s));
			} else if (strcmp(instr, "STOPSIGNAL") == 0) {
				sb_str(&after, "\n    (stop_signal ");
				sb_quoted(&after, trim(s));
				sb_str(&after, ")");
			} else {
				note(&b.steps, "not translated", instr);
			}
		}
	}
	fclose(file);
	if (base == NULL) {
		fprintf(stderr, "hull: import: %s has no FROM; only a Dockerfile is imported\n", path);
		free(b.steps.data);
		free(rt.data);
		free(after.data);
		return 2;
	}
	/* The final ENV and WORKDIR are the image's too. */
	if (b.env_count > 0u) {
		sb_str(&rt, "\n    (env");
		for (unsigned i = 0u; i < b.env_count; i++) {
			sb_str(&rt, " (");
			sb_str(&rt, b.env_name[i]);
			sb_str(&rt, " ");
			sb_quoted(&rt, b.env_value[i]);
			sb_str(&rt, ")");
		}
		sb_str(&rt, ")");
	}
	if (b.workdir[0] != '\0') {
		sb_str(&rt, "\n    (workdir ");
		sb_quoted(&rt, b.workdir);
		sb_str(&rt, ")");
	}
	sb_add(&rt, after.data ? after.data : "", after.len);
	if (b.steps.bad || rt.bad || after.bad) {
		fprintf(stderr, "hull: import: out of memory\n");
		status = 2;
	} else {
		printf("(build\n  (base ");
		/* base is a token; a string keeps a slash and a colon safe. */
		printf("\"%s\")", base);
		if (rt.len > 0u)
			printf("\n  (runtime%s)", rt.data);
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
	return status;
}
