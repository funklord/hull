#ifndef HULL_IMPORT_H
#define HULL_IMPORT_H

/* `hull import FILE`: a Dockerfile to a (build ...) form on stdout, section
 * 9's one-way best-effort generator. Returns a process status: 0, or 2 when
 * the file cannot be read or holds no FROM. */
int hull_import(const char *path);

/* Whether a spec reader takes `path` as a compose file: named .yml or
 * .yaml, as every compose file a directory's `hull up` finds is. */
int hull_import_is_compose(const char *path);

/* The compose dialect (section 9): the compose file at `path` as the
 * (machines ...) form `hull import` prints, allocated, each thing it does
 * not translate said on `notes` as `PATH: SERVICE: what`. NULL, having
 * said why, where it cannot be read, has no services, or has a service
 * with no image, which no engine could run. */
char *hull_import_compose(const char *path, FILE *notes);

#endif
