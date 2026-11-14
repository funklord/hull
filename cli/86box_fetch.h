/* 86box_fetch.h -- `hull 86box fetch`; see 86box_fetch.c. */

#ifndef HULL_CLI_86BOX_FETCH_H
#define HULL_CLI_86BOX_FETCH_H

#include <stddef.h>

/* 86Box and its ROM set into the user's cache, its directory printed: 0,
 * or -1 with `why`. Nothing is done where they are there already. */
int hull_86box_fetch(char *why, size_t cap);

#endif
