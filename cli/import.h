#ifndef HULL_IMPORT_H
#define HULL_IMPORT_H

/* `hull import FILE`: a Dockerfile to a (build ...) form on stdout, section
 * 9's one-way best-effort generator. Returns a process status: 0, or 2 when
 * the file cannot be read or holds no FROM. */
int hull_import(const char *path);

#endif
