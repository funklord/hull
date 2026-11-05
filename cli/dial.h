/*
 * dial.h -- a core on another host, reached over ssh (project.md
 * section 4.10): `hull -H ssh://[USER@]HOST[:PORT][/DIR] COMMAND`.
 */

#ifndef HULL_DIAL_H
#define HULL_DIAL_H

#include <stddef.h>

typedef struct hull_dial_target {
	char destination[320];   /* USER@HOST, as ssh takes it */
	char port[8];            /* or "" */
	char dir[1024];          /* what relative paths in a plan mean there, or "" */
} hull_dial_target;

/* `url` read into `out`: 0, or -1 with why in `why`. */
int hull_dial_parse(const char *url, hull_dial_target *out, char *why, size_t cap);

/* A socket that is the remote core's, by `ssh DESTINATION hull dial`:
 * a descriptor, or -1. ssh's own messages go to this stderr, and it ends
 * when the socket does, this process's end closing with it. */
int hull_dial_host(const hull_dial_target *remote);

/* `hull dial`'s body: stdin and stdout joined to `fd`, until the core or
 * the far end closes. A process status. */
int hull_dial_pump(int fd);

#endif
