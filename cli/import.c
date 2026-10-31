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
 * since multi-stage COPY --from has no (build ...) shape. The foreign-image
 * import the CLI sketch names is not written.
 *
 * A compose file is translated the same way to a (machines ...) form, and
 * is also section 9's compose dialect: hull_import_compose() gives the
 * form to the planner in place of a file, each note said on stderr as
 * well, so that `hull up` and `-f compose.yaml` run it with no step
 * between and nothing it leaves out goes unsaid.
 */
#define _GNU_SOURCE

#include <ctype.h>
#include <arpa/inet.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "import.h"

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

static int import_dockerfile(FILE *file)
{
	build  b = { { NULL, 0u, 0u, 0 }, { { 0 } }, { { 0 } }, 0u, "", 0, 0, 0u };
	sb     rt = { NULL, 0u, 0u, 0 };
	sb     after = { NULL, 0u, 0u, 0 };   /* CMD/ENTRYPOINT/etc. order kept */
	char  *base = NULL;
	char   line[LINE_CAP];
	char   logical[LINE_CAP];
	size_t at = 0u;
	int    status = 0;

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
	if (base == NULL) {
		fprintf(stderr, "hull: import: the Dockerfile has no FROM\n");
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

/* --- compose ------------------------------------------------------------- */

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
static void compose_volume(svc *s, char *spec)
{
	char *p = scalar(spec);
	char *c1 = strchr(p, ':');
	char *c2;
	int   ro;

	if (c1 == NULL) {
		svc_note(s, "anonymous volume not translated", p);
		return;
	}
	*c1 = '\0';
	if (p[0] != '/' && p[0] != '.') {
		svc_note(s, "named volume not translated", p);
		return;
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

static void svc_reset(svc *s)
{
	free(s->after.data);
	free(s->command.data);
	free(s->env.data);
	free(s->ports.data);
	free(s->volumes.data);
	free(s->notes.data);
	memset(s, 0, sizeof *s);
	s->dep_indent  = -1;
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
	as_label(name, sizeof name, project);

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
			} else if (colon != NULL) {
				*colon = '\0';
				if (strcmp(content, "version") != 0) {
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
			continue;
		}
		if (field_indent < 0)
			field_indent = ind;
		if (ind > field_indent && field[0] != '\0') {   /* a map entry of `field` */
			if (strcmp(field, "environment") == 0)
				compose_env(&s, content, 1);
			else if (strcmp(field, "networks") == 0) {
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
		if (value[0] == '\0')
			continue;             /* a block list or map follows */
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
		} else if (strcmp(key, "build") == 0)
			svc_note(&s, "build not translated", "hull import the Dockerfile, then name the ref");
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
		else if (strcmp(key, "user") == 0)
			svc_note(&s, "user not translated", scalar(value));
		else if (strcmp(key, "network_mode") == 0) {
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
	char  project[256];

	if (file == NULL) {
		perror(path);
		return NULL;
	}
	project_of(path, project, sizeof project);
	warnings = notes;
	warned_path = path;
	unrunnable = 0;
	text = compose_text(file, project);
	fclose(file);
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
int hull_import(const char *path)
{
	const char *base = strrchr(path, '/');
	FILE       *file;
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

		project_of(path, project, sizeof project);
		status = import_compose(file, project);
	} else {
		status = import_dockerfile(file);
	}
	fclose(file);
	return status;
}
