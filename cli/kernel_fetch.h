#ifndef HULL_KERNEL_FETCH_H
#define HULL_KERNEL_FETCH_H

#include <stddef.h>

/* `hull kernel fetch` (Appendix F): the pinned kernel of engine/kernel.h
 * downloaded, checked and unpacked into the user's cache, its directory
 * printed; at once where it is there already. 0, or -1 with `why`. */
int hull_kernel_fetch(char *why, size_t cap);

/* The Debian package at `url`, downloaded to `deb` and checked against
 * `sha256`, its data.tar.xz decompressed into `tar`, a new file, and `deb`
 * removed whatever the outcome. 0, or -1 with `why` and no `tar` left. */
int hull_fetch_deb(const char *url, const char *sha256, const char *deb, const char *tar,
                   char *why, size_t cap);

#endif
