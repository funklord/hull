/*
 * vagrant.c -- a Vagrantfile's declarative subset (vagrant.h, project.md
 * section 9).
 *
 * A Vagrantfile is executable Ruby and hull embeds no interpreter, so this
 * reads it a line at a time against the declarations real files are made
 * of: Vagrant.configure and its blocks, config.vm.box, hostname, network,
 * synced_folder, provision, provider and define. Everything else is one of
 * two things. A setting of Vagrant's own with no meaning here, config.ssh
 * or vb.customize, is noted and passed over; a line that is not a
 * declaration at all -- a loop, a condition, interpolation, ENV -- is
 * refused with its line number, since a plan that guessed at it could
 * differ from what the file means, and the spec's rule is that nothing is
 * silently ignored.
 *
 * A box is the store's ref vagrant/NAME, which `hull vagrant box add` makes.
 * A machine with shell provisioners runs from what building them on the
 * box gives, as a compose service's build: does, and so is cached.
 */

#define _GNU_SOURCE

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "vagrant.h"

#define MAX_PORTS   32u
#define MAX_FOLDERS 16u
#define MAX_SCRIPTS 8u
#define MAX_NESTING 8u
#define LINE_BYTES  4096u

typedef struct port {
	long host;
	long guest;
	int  udp;
} port;

typedef struct folder {
	char host[1024];
	char guest[1024];
} folder;

typedef struct machine {
	char     name[128];
	char     box[256];
	char     hostname[128];
	char     backend[32];
	long     memory;
	long     cpus;
	port     ports[MAX_PORTS];
	unsigned port_count;
	folder   folders[MAX_FOLDERS];
	unsigned folder_count;
	int      no_default_folder;
	char    *provision;      /* the shell provisioners, in order */
} machine;

typedef enum kind {
	BLOCK_CONFIG, BLOCK_MACHINE, BLOCK_PROVIDER, BLOCK_PROVISION, BLOCK_IGNORED
} kind;

typedef struct block {
	kind     what;
	char     variable[64];
	machine *target;
} block;

typedef struct script {
	char  name[64];
	char *text;
} script;

typedef struct reader {
	FILE       *file;
	FILE       *notes;
	const char *path;
	unsigned    line;
	block       stack[MAX_NESTING];
	unsigned    depth;
	machine     all;                 /* what config. says, every machine's */
	machine     machines[MAX_VAGRANT_MACHINES];
	unsigned    machine_count;
	script      scripts[MAX_SCRIPTS];
	unsigned    script_count;
	int         failed;
} reader;

static hull_vagrant_build builds[MAX_VAGRANT_MACHINES];
static unsigned           build_count;

unsigned hull_vagrant_builds(const hull_vagrant_build **list)
{
	*list = builds;
	return build_count;
}

int hull_vagrant_is_file(const char *path)
{
	const char *slash = strrchr(path, '/');

	return strcmp(slash != NULL ? slash + 1 : path, "Vagrantfile") == 0;
}

static void refuse(reader *r, const char *why, const char *what)
{
	fprintf(r->notes != NULL ? r->notes : stderr, "%s:%u: %s: %s\n", r->path, r->line, why, what);
	r->failed = 1;
}

static void note(reader *r, const char *why, const char *what)
{
	if (r->notes != NULL)
		fprintf(r->notes, "%s:%u: %s: %s\n", r->path, r->line, why, what);
}

static char *trim(char *s)
{
	char *end;

	while (isspace((unsigned char)*s))
		s++;
	end = s + strlen(s);
	while (end > s && isspace((unsigned char)end[-1]))
		*--end = '\0';
	return s;
}

/* A comment off the line, one outside a string. */
static void strip_comment(char *s)
{
	char quote = '\0';

	for (; *s != '\0'; s++) {
		if (quote != '\0') {
			if (*s == '\\' && s[1] != '\0')
				s++;
			else if (*s == quote)
				quote = '\0';
		} else if (*s == '"' || *s == '\'') {
			quote = *s;
		} else if (*s == '#') {
			*s = '\0';
			return;
		}
	}
}

/* The string opening `s`, into `out`, and where it ended: NULL where `s`
 * is no plain string. Interpolation is code, so it is not one. */
static const char *string_at(const char *s, char *out, size_t cap)
{
	char   quote = *s;
	size_t n = 0u;

	if (quote != '"' && quote != '\'')
		return NULL;
	for (s++; *s != '\0' && *s != quote; s++) {
		if (quote == '"' && s[0] == '#' && s[1] == '{')
			return NULL;
		if (*s == '\\' && s[1] != '\0')
			s++;
		if (n + 1u < cap)
			out[n++] = *s;
	}
	if (*s != quote)
		return NULL;
	out[n] = '\0';
	return s + 1;
}

/* After `key:` or `:key =>`, in the argument list `args`: where its value
 * opens, or NULL. */
static const char *argument(const char *args, const char *key)
{
	size_t n = strlen(key);

	for (const char *at = args; (at = strstr(at, key)) != NULL; at += n) {
		const char *after = at + n;

		if (at != args && (isalnum((unsigned char)at[-1]) || at[-1] == '_'))
			continue;
		if (*after == ':' && after[1] != ':') {
			after++;
		} else if (at != args && at[-1] == ':') {
			while (*after == ' ')
				after++;
			if (strncmp(after, "=>", 2u) != 0)
				continue;
			after += 2;
		} else {
			continue;
		}
		while (*after == ' ')
			after++;
		return after;
	}
	return NULL;
}

/* A value that is a number or a string of one. */
static int number_of(const char *value, long *out)
{
	char  text[32];
	char *end;

	if (string_at(value, text, sizeof text) != NULL)
		value = text;
	*out = strtol(value, &end, 10);
	return end != value && (*end == '\0' || *end == ',' || isspace((unsigned char)*end));
}

static void append(char **to, const char *text)
{
	size_t had = *to != NULL ? strlen(*to) : 0u;
	char  *grown = realloc(*to, had + strlen(text) + 2u);

	if (grown == NULL)
		return;
	memcpy(grown + had, text, strlen(text));
	grown[had + strlen(text)] = '\n';
	grown[had + strlen(text) + 1u] = '\0';
	*to = grown;
}

/* A heredoc's lines, to its terminator, opened by `<<-X`, `<<~X` or `<<X`
 * at `opening`. NULL where `opening` is none or it does not end. */
static char *heredoc(reader *r, const char *opening)
{
	char  word[64];
	char  line[LINE_BYTES];
	char *text = NULL;
	size_t n = 0u;

	if (strncmp(opening, "<<", 2u) != 0)
		return NULL;
	opening += 2;
	if (*opening == '-' || *opening == '~')
		opening++;
	if (*opening == '"' || *opening == '\'')
		opening++;
	while ((isalnum((unsigned char)opening[n]) || opening[n] == '_') && n + 1u < sizeof word) {
		word[n] = opening[n];
		n++;
	}
	word[n] = '\0';
	if (n == 0u)
		return NULL;
	while (fgets(line, sizeof line, r->file) != NULL) {
		r->line++;
		line[strcspn(line, "\n")] = '\0';
		if (strcmp(trim(line), word) == 0)
			return text != NULL ? text : strdup("");
		append(&text, line);
	}
	free(text);
	return NULL;
}

/* The script `value` gives: a string, a heredoc, or a $name of the file. */
static char *script_of(reader *r, const char *value)
{
	char text[LINE_BYTES];

	if (string_at(value, text, sizeof text) != NULL)
		return strdup(text);
	if (value[0] == '$')
		for (unsigned i = 0u; i < r->script_count; i++) {
			size_t n = strlen(r->scripts[i].name);

			if (strncmp(value + 1, r->scripts[i].name, n) == 0 &&
			    !isalnum((unsigned char)value[1 + n]) && value[1 + n] != '_')
				return strdup(r->scripts[i].text);
		}
	return heredoc(r, value);
}

static void add_provision(reader *r, machine *m, char *text)
{
	if (text == NULL) {
		refuse(r, "not a script hull can read without running Ruby", "inline");
		return;
	}
	if (strstr(text, "#{") != NULL) {
		refuse(r, "interpolation is code, and hull runs none", "#{...}");
		free(text);
		return;
	}
	append(&m->provision, text);
	free(text);
}

/* `network "forwarded_port", guest: 80, host: 8080`. */
static void read_network(reader *r, machine *m, const char *args)
{
	char        type[64];
	const char *rest = string_at(args, type, sizeof type);
	const char *guest;
	const char *host;
	const char *proto;
	char        word[16] = "";
	port        p = { 0 };

	if (rest == NULL) {
		if (args[0] == ':' && strncmp(args + 1, "forwarded_port", 14u) == 0) {
			snprintf(type, sizeof type, "forwarded_port");
			rest = args + 15;
		} else {
			note(r, "network not translated", args);
			return;
		}
	}
	if (strcmp(type, "forwarded_port") != 0) {
		note(r, "network not translated, a machine having hull's own", type);
		return;
	}
	guest = argument(rest, "guest");
	host  = argument(rest, "host");
	proto = argument(rest, "protocol");
	if (guest == NULL || host == NULL || !number_of(guest, &p.guest) || !number_of(host, &p.host)) {
		refuse(r, "a forwarded port needs a guest and a host that are numbers", rest);
		return;
	}
	if (proto != NULL && string_at(proto, word, sizeof word) != NULL)
		p.udp = strcmp(word, "udp") == 0;
	if (m->port_count == MAX_PORTS) {
		refuse(r, "too many ports", rest);
		return;
	}
	m->ports[m->port_count++] = p;
}

/* `synced_folder ".", "/vagrant", disabled: true`. */
static void read_folder(reader *r, machine *m, const char *args)
{
	folder      f;
	const char *rest = string_at(args, f.host, sizeof f.host);
	const char *disabled;

	while (rest != NULL && (*rest == ',' || *rest == ' '))
		rest++;
	if (rest == NULL || (rest = string_at(rest, f.guest, sizeof f.guest)) == NULL) {
		refuse(r, "a synced folder is two strings", args);
		return;
	}
	if ((disabled = argument(rest, "disabled")) != NULL && strncmp(disabled, "true", 4u) == 0) {
		if (strcmp(f.guest, "/vagrant") == 0)
			m->no_default_folder = 1;
		return;
	}
	if (m->folder_count == MAX_FOLDERS) {
		refuse(r, "too many synced folders", args);
		return;
	}
	m->folders[m->folder_count++] = f;
}

static void provider_of(machine *m, const char *name)
{
	if (strcmp(name, "virtualbox") == 0)
		snprintf(m->backend, sizeof m->backend, "virtualbox");
	else if (strcmp(name, "libvirt") == 0 || strcmp(name, "qemu") == 0)
		snprintf(m->backend, sizeof m->backend, "vm");
	else if (strcmp(name, "docker") == 0)
		snprintf(m->backend, sizeof m->backend, "docker");
}

/* `do |x|` at the line's end: the variable into `variable`, 1; 0 where
 * there is no block. */
static int block_of(const char *line, char *variable, size_t cap)
{
	const char *at = strstr(line, " do |");
	size_t      n = 0u;

	if (at == NULL)
		return 0;
	at += 5;
	while (at[n] != '\0' && at[n] != '|' && at[n] != ',' && n + 1u < cap) {
		variable[n] = at[n];
		n++;
	}
	variable[n] = '\0';
	return 1;
}

static void push(reader *r, kind what, const char *variable, machine *target)
{
	if (r->depth == MAX_NESTING) {
		refuse(r, "blocks nested too deep", variable);
		return;
	}
	r->stack[r->depth].what = what;
	r->stack[r->depth].target = target;
	snprintf(r->stack[r->depth].variable, sizeof r->stack[0].variable, "%s", variable);
	r->depth++;
}

/* The block whose variable opens `line` with a `.`, the rest in `*rest`. */
static block *block_named(reader *r, const char *line, const char **rest)
{
	for (unsigned i = r->depth; i > 0u; i--) {
		size_t n = strlen(r->stack[i - 1u].variable);

		if (n != 0u && strncmp(line, r->stack[i - 1u].variable, n) == 0 && line[n] == '.') {
			*rest = line + n + 1;
			return &r->stack[i - 1u];
		}
	}
	return NULL;
}

/* After `key =` in `text`, or NULL. */
static const char *assigned(const char *text, const char *key)
{
	size_t n = strlen(key);

	if (strncmp(text, key, n) != 0)
		return NULL;
	text += n;
	while (*text == ' ')
		text++;
	if (*text != '=' || text[1] == '=')
		return NULL;
	text++;
	while (*text == ' ')
		text++;
	return text;
}

static void read_vm(reader *r, block *b, const char *rest, const char *line)
{
	machine    *m = b->target;
	const char *value;
	char        text[512];
	char        variable[64];

	if ((value = assigned(rest, "box")) != NULL) {
		if (string_at(value, m->box, sizeof m->box) == NULL)
			refuse(r, "a box is a string", value);
	} else if ((value = assigned(rest, "hostname")) != NULL) {
		if (string_at(value, m->hostname, sizeof m->hostname) == NULL)
			refuse(r, "a hostname is a string", value);
	} else if (strncmp(rest, "network ", 8u) == 0) {
		read_network(r, m, trim((char *)rest + 8));
	} else if (strncmp(rest, "synced_folder ", 14u) == 0) {
		read_folder(r, m, trim((char *)rest + 14));
	} else if (strncmp(rest, "define ", 7u) == 0) {
		machine    *defined;
		const char *name = trim((char *)rest + 7);

		if (r->machine_count == MAX_VAGRANT_MACHINES || !block_of(line, variable, sizeof variable) ||
		    (string_at(name, text, sizeof text) == NULL &&
		     !(name[0] == ':' && sscanf(name + 1, "%127[a-zA-Z0-9_-]", text) == 1))) {
			refuse(r, "a define is a name and a block", name);
			return;
		}
		defined = &r->machines[r->machine_count++];
		*defined = r->all;
		defined->provision = r->all.provision != NULL ? strdup(r->all.provision) : NULL;
		snprintf(defined->name, sizeof defined->name, "%.120s", text);
		push(r, BLOCK_MACHINE, variable, defined);
	} else if (strncmp(rest, "provider ", 9u) == 0) {
		const char *name = trim((char *)rest + 9);

		if (string_at(name, text, sizeof text) != NULL ||
		    (name[0] == ':' && sscanf(name + 1, "%63[a-z_]", text) == 1))
			provider_of(m, text);
		if (block_of(line, variable, sizeof variable))
			push(r, BLOCK_PROVIDER, variable, m);
	} else if (strncmp(rest, "provision ", 10u) == 0) {
		const char *args = trim((char *)rest + 10);
		const char *inline_at = argument(args, "inline");
		const char *file_at = argument(args, "path");
		int         block = block_of(line, variable, sizeof variable);

		if ((string_at(args, text, sizeof text) == NULL && args[0] != ':') ||
		    (strncmp(args, "\"shell\"", 7u) != 0 && strncmp(args, "'shell'", 7u) != 0 &&
		     strncmp(args, ":shell", 6u) != 0)) {
			note(r, "provisioner not translated", args);
			if (block)
				push(r, BLOCK_IGNORED, variable, m);
			return;
		}
		if (inline_at != NULL)
			add_provision(r, m, script_of(r, inline_at));
		else if (file_at != NULL && string_at(file_at, text, sizeof text) != NULL)
			note(r, "a provisioner's script file not translated", text);
		else if (block)
			push(r, BLOCK_PROVISION, variable, m);
		else
			refuse(r, "a shell provisioner hull cannot read", args);
	} else {
		note(r, "not translated", line);
		if (block_of(line, variable, sizeof variable))
			push(r, BLOCK_IGNORED, variable, m);
	}
}

static void read_line(reader *r, char *line)
{
	const char *rest;
	const char *value;
	block      *b;
	char        variable[64];

	if (strcmp(line, "end") == 0) {
		if (r->depth == 0u)
			refuse(r, "an end with no block", line);
		else
			r->depth--;
		return;
	}
	if (strncmp(line, "Vagrant.configure", 17u) == 0 && block_of(line, variable, sizeof variable)) {
		push(r, BLOCK_CONFIG, variable, &r->all);
		return;
	}
	/* Vagrant.require_version and the like, and a `$name` script. */
	if (strncmp(line, "Vagrant.require_version", 23u) == 0)
		return;
	if (line[0] == '$' && strchr(line, '=') != NULL) {
		char  name[64];
		char *text;

		if (sscanf(line + 1, "%63[a-zA-Z0-9_]", name) != 1 || r->script_count == MAX_SCRIPTS ||
		    (value = assigned(line + 1, name)) == NULL ||
		    (text = script_of(r, value)) == NULL) {
			refuse(r, "not a script hull can read without running Ruby", line);
			return;
		}
		snprintf(r->scripts[r->script_count].name, sizeof r->scripts[0].name, "%s", name);
		r->scripts[r->script_count++].text = text;
		return;
	}
	/* A constant the file names, as VAGRANTFILE_API_VERSION = "2". */
	if (isupper((unsigned char)line[0]) && strchr(line, '=') != NULL &&
	    (line[strcspn(line, " =")] == ' ' || line[strcspn(line, " =")] == '=')) {
		size_t n = strcspn(line, " =");
		size_t k = 0u;

		while (k < n && (isupper((unsigned char)line[k]) || isdigit((unsigned char)line[k]) ||
		                 line[k] == '_'))
			k++;
		if (k == n)
			return;
	}
	if ((b = block_named(r, line, &rest)) == NULL) {
		if (r->depth != 0u && r->stack[r->depth - 1u].what == BLOCK_IGNORED)
			return;
		refuse(r, "not a declaration, and hull runs no Ruby", line);
		return;
	}
	switch (b->what) {
	case BLOCK_CONFIG:
	case BLOCK_MACHINE:
		if (strncmp(rest, "vm.", 3u) == 0)
			read_vm(r, b, rest + 3, line);
		else
			note(r, "not translated", line);
		break;
	case BLOCK_PROVIDER:
		if ((value = assigned(rest, "memory")) != NULL) {
			if (!number_of(value, &b->target->memory))
				refuse(r, "memory is a number", value);
		} else if ((value = assigned(rest, "cpus")) != NULL) {
			if (!number_of(value, &b->target->cpus))
				refuse(r, "cpus is a number", value);
		} else {
			note(r, "provider setting not translated", line);
		}
		break;
	case BLOCK_PROVISION:
		if ((value = assigned(rest, "inline")) != NULL)
			add_provision(r, b->target, script_of(r, value));
		else
			note(r, "not translated", line);
		break;
	case BLOCK_IGNORED:
		break;
	}
}

static void quoted(FILE *out, const char *text)
{
	fputc('"', out);
	for (; *text != '\0'; text++) {
		if (*text == '"' || *text == '\\')
			fputc('\\', out);
		if (*text == '\n')
			fputs("\\n", out);
		else
			fputc(*text, out);
	}
	fputc('"', out);
}

/* The directory holding `path`, by its last name, as Vagrant's project. */
static void project_of(const char *path, char *out, size_t cap)
{
	char        resolved[4096];
	const char *slash;

	if (realpath(path, resolved) == NULL)
		snprintf(resolved, sizeof resolved, "/machine/x");
	if ((slash = strrchr(resolved, '/')) != NULL)
		resolved[slash - resolved] = '\0';
	slash = strrchr(resolved, '/');
	snprintf(out, cap, "%.200s", slash != NULL ? slash + 1 : resolved);
	for (; *out != '\0'; out++)
		if (!isalnum((unsigned char)*out) && *out != '-' && *out != '_')
			*out = '-';
}

static void machine_form(FILE *out, reader *r, machine *m, const char *project, const char *base_dir)
{
	char  source[512];

	if (m->box[0] == '\0') {
		fprintf(stderr, "%s: machine %s has no box\n", r->path, m->name);
		r->failed = 1;
		return;
	}
	snprintf(source, sizeof source, "vagrant/%s", m->box);
	if (m->provision != NULL && build_count < MAX_VAGRANT_MACHINES) {
		hull_vagrant_build *b = &builds[build_count++];
		char               *text = NULL;
		size_t              length = 0u;
		FILE               *form = open_memstream(&text, &length);

		snprintf(b->ref, sizeof b->ref, "vagrant/%.100s/%.120s", project, m->name);
		if (form != NULL) {
			fprintf(form, "(build (base ");
			quoted(form, source);
			fprintf(form, ") (steps (step (name \"provision\") (network true) (run ");
			quoted(form, m->provision);
			fprintf(form, "))))\n");
			fclose(form);
			b->form = text;
		}
		snprintf(source, sizeof source, "%s", b->ref);
	}
	fprintf(out, "\n  (%s (source ", m->name);
	quoted(out, source);
	fprintf(out, ") (mutable overlay) (ssh true) (init \"/sbin/init\")");
	if (m->backend[0] != '\0')
		fprintf(out, " (backend %s)", m->backend);
	if (m->hostname[0] != '\0') {
		fprintf(out, " (hostname ");
		quoted(out, m->hostname);
		fprintf(out, ")");
	}
	if (m->memory > 0)
		fprintf(out, " (memory \"%ldM\")", m->memory);
	if (m->cpus > 0)
		fprintf(out, " (cpus %ld)", m->cpus);
	if (m->port_count != 0u) {
		fprintf(out, " (ports");
		for (unsigned i = 0u; i < m->port_count; i++)
			fprintf(out, " (port (host %ld) (guest %ld)%s)", m->ports[i].host, m->ports[i].guest,
			        m->ports[i].udp ? " (proto udp)" : "");
		fprintf(out, ")");
	}
	if (m->folder_count != 0u || !m->no_default_folder) {
		fprintf(out, " (volumes");
		/* Vagrant shares the project at /vagrant unless told not to. */
		if (!m->no_default_folder) {
			fprintf(out, " (volume (host ");
			quoted(out, base_dir);
			fprintf(out, ") (guest \"/vagrant\"))");
		}
		for (unsigned i = 0u; i < m->folder_count; i++) {
			fprintf(out, " (volume (host ");
			quoted(out, m->folders[i].host);
			fprintf(out, ") (guest ");
			quoted(out, m->folders[i].guest);
			fprintf(out, "))");
		}
		fprintf(out, ")");
	}
	fprintf(out, ")");
}

char *hull_vagrant_form(const char *path, FILE *notes)
{
	static reader r;
	char          line[LINE_BYTES];
	char          project[256];
	char          base_dir[4096];
	char         *text = NULL;
	size_t        length = 0u;
	FILE         *out;
	const char   *slash = strrchr(path, '/');

	memset(&r, 0, sizeof r);
	for (unsigned i = 0u; i < build_count; i++)
		free(builds[i].form);
	build_count = 0u;
	r.path  = path;
	r.notes = notes;
	if ((r.file = fopen(path, "r")) == NULL) {
		perror(path);
		return NULL;
	}
	while (!r.failed && fgets(line, sizeof line, r.file) != NULL) {
		char *s;

		r.line++;
		line[strcspn(line, "\n")] = '\0';
		strip_comment(line);
		s = trim(line);
		if (*s != '\0')
			read_line(&r, s);
	}
	fclose(r.file);
	if (r.failed)
		return NULL;
	project_of(path, project, sizeof project);
	snprintf(base_dir, sizeof base_dir, "%.*s", slash != NULL ? (int)(slash - path) : 1,
	         slash != NULL ? path : ".");
	/* No define: the one machine is what config. says. */
	if (r.machine_count == 0u) {
		r.machines[0] = r.all;
		snprintf(r.machines[0].name, sizeof r.machines[0].name, "default");
		r.machine_count = 1u;
	}
	if ((out = open_memstream(&text, &length)) == NULL)
		return NULL;
	fprintf(out, "(machines");
	for (unsigned i = 0u; i < r.machine_count; i++)
		machine_form(out, &r, &r.machines[i], project, base_dir);
	fprintf(out, ")\n");
	fclose(out);
	if (r.failed) {
		free(text);
		return NULL;
	}
	return text;
}
