/*
 * hull.c -- the command line, as far as planning reaches (project.md
 * section 10).
 *
 *   hull plan [-f FILE] [--backend B] [--accept TOKEN]... [--explain]
 *   hull check [-f FILE] --backend B[,B...] [--accept TOKEN]...
 *   hull engines
 *
 * `plan` resolves each machine for the backend it names, or for --backend;
 * `check` does so for every backend listed, and is the CI gate (J.8).
 * Both print what was rejected, emulated or ignored, and --explain prints
 * every field. Messages name the file, line and column of the entry that
 * asked, as a compiler would.
 *
 * Exit status: 0 when nothing was rejected, 1 when something was, 2 when
 * the file could not be read or checked, or the command was wrong.
 */

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/utsname.h>

#include "builtin.h"
#include "matrix.h"
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
	fputs("usage: hull plan [-f FILE] [--backend B] [--accept TOKEN]... [--explain]\n"
	      "       hull check [-f FILE] --backend B[,B...] [--accept TOKEN]...\n"
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
	printf("%s:%u:%u: %s on %s (%s): %s: %s", r->options->file, line, column, r->machine,
	       r->choice->engine->caps->engine_name, r->choice->class_name, resolution->id,
	       outcome_name(resolution->outcome));
	if (resolution->note != NULL)
		printf(": %s", resolution->note);
	if (resolution->outcome == HULL_OUTCOME_REJECTED && resolution->token != NULL)
		printf(" (--accept %s honours it)", resolution->token);
	printf("\n");
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
 * not, and 2 if the backend could not be chosen. */
static int resolve_one(const options *o, const hull_registry *registry, const hull_spec *spec,
                       const hull_sexpr_form *machine, const char *name, const char *backend)
{
	hull_choice          choice;
	const char          *reason = NULL;
	report               r;
	hull_registry_status status;

	status = hull_registry_choose(registry, spec, machine, backend, &choice, &reason);
	if (status != HULL_REGISTRY_OK) {
		fprintf(stderr, "%s: %s on %s: %s\n", o->file, name,
		        backend != NULL ? backend : "its own backend", reason);
		return 2;
	}
	r.options  = o;
	r.spec     = spec;
	r.machine  = name;
	r.choice   = &choice;
	r.rejected = 0u;
	hull_matrix_resolve(spec, machine, choice.engine->caps, choice.class_name,
	                    registry->host_arch, o->tokens, print_resolution, &r);
	if (r.rejected != 0u)
		printf("%s: %s on %s (%s): rejected, %u field%s\n", o->file, name,
		       choice.engine->caps->engine_name, choice.class_name, r.rejected,
		       r.rejected == 1u ? "" : "s");
	else
		printf("%s: %s on %s (%s): ok\n", o->file, name, choice.engine->caps->engine_name,
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
			result = resolve_one(o, registry, &spec, &spec.machine, "machine", backend);
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

	memset(&o, 0, sizeof o);
	o.file = "hull.conf";
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
	if (hull_registry_add(&registry, hull_engine_docker(), &reason) != HULL_REGISTRY_OK ||
	    hull_registry_add(&registry, hull_engine_qemu(), &reason) != HULL_REGISTRY_OK) {
		fprintf(stderr, "hull: a built-in engine is refused: %s\n", reason);
		return 2;
	}

	if (strcmp(command, "engines") == 0 && argc == 2) {
		list_engines(&registry);
		return 0;
	}
	if (strcmp(command, "plan") != 0 && strcmp(command, "check") != 0) {
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
		} else {
			usage(stderr);
			return 2;
		}
	}
	if (strcmp(command, "plan") == 0 && o.backend_count > 1u) {
		fprintf(stderr, "hull: plan takes one backend; check takes several\n");
		return 2;
	}
	if (strcmp(command, "check") == 0 && o.backend_count == 0u) {
		fprintf(stderr, "hull: check needs --backend, the backends to hold the spec to\n");
		return 2;
	}
	return resolve_all(&o, &registry);
}
