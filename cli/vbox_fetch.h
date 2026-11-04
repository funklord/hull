/*
 * vbox_fetch.h -- `hull virtualbox fetch`: VirtualBox's pinned package,
 * engine/vbox.h's, downloaded once and unpacked into the user's cache.
 */

#ifndef HULL_VBOX_FETCH_H
#define HULL_VBOX_FETCH_H

#include <stddef.h>

/* Fetch and unpack it where it is not yet, printing where it is. 0, or
 * -1 with `why`. */
int hull_vbox_fetch(char *why, size_t cap);

#endif
