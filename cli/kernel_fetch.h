#ifndef HULL_KERNEL_FETCH_H
#define HULL_KERNEL_FETCH_H

#include <stddef.h>

/* `hull kernel fetch` (Appendix F): the pinned kernel of engine/kernel.h
 * downloaded, checked and unpacked into the user's cache, its directory
 * printed; at once where it is there already. 0, or -1 with `why`. */
int hull_kernel_fetch(char *why, size_t cap);

#endif
