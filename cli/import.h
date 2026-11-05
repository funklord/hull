#ifndef HULL_IMPORT_H
#define HULL_IMPORT_H

#include <stdio.h>

/* `hull import FILE`: a Dockerfile to a (build ...) form on stdout, section
 * 9's one-way best-effort generator. Returns a process status: 0, or 2 when
 * the file cannot be read or holds no FROM. */
int hull_import(const char *path);

/* A build's ARG given from outside. */
#define HULL_MAX_BUILD_ARGS 16u

typedef struct hull_build_arg {
	char name[128];
	char value[512];
} hull_build_arg;

/* hull_import(), `args` given to its ARGs, and with a `target` the image
 * the stage of that name, what follows it left out. */
int hull_import_build(const char *path, const hull_build_arg *args, unsigned count,
                      const char *target);

/* Whether a spec reader takes `path` as a compose file: named .yml or
 * .yaml, as every compose file a directory's `hull up` finds is. */
int hull_import_is_compose(const char *path);

/* The compose dialect (section 9): the compose file at `path` as the
 * (machines ...) form `hull import` prints, allocated, each thing it does
 * not translate said on `notes` as `PATH: SERVICE: what`. NULL, having
 * said why, where it cannot be read, has no services, or has a service
 * with no image, which no engine could run. */
char *hull_import_compose(const char *path, FILE *notes);

/* What the last translation's services build: each a Dockerfile, its
 * context relative to the file and its path to the context, and the ref
 * the service runs from. How many, `list` their array. */
#define MAX_COMPOSE_BUILDS 32u

typedef struct hull_compose_build {
	char ref[256];
	char context[512];
	char dockerfile[256];
	char target[128];
	hull_build_arg args[HULL_MAX_BUILD_ARGS];
	unsigned arg_count;
} hull_compose_build;

unsigned hull_compose_builds(const hull_compose_build **list);

#endif
