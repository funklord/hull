#ifndef HULL_KERNEL_FETCH_H
#define HULL_KERNEL_FETCH_H

#include <stddef.h>

/* `hull kernel fetch` (Appendix F): the pinned kernel of engine/kernel.h
 * downloaded, checked and unpacked into the user's cache, its directory
 * printed; at once where it is there already. 0, or -1 with `why`. */
int hull_kernel_fetch(char *why, size_t cap);

/* The same for `arch`'s pinned kernel, amd64's or arm64's, into its own
 * directory (engine/kernel.h). */
int hull_kernel_fetch_arch(const char *arch, char *why, size_t cap);

/* The Debian package at `path` under the pool, or where the pool has it
 * no longer under the snapshot (engine/kernel.h), downloaded to `deb` and
 * checked against `sha256`, its data.tar.xz decompressed into `tar`, a new
 * file, and `deb` removed whatever the outcome. 0, or -1 with `why` and no
 * `tar` left. */
int hull_fetch_deb(const char *path, const char *sha256, const char *deb, const char *tar,
                   char *why, size_t cap);

/* The .tar.xz at the https `url` downloaded to `file`, checked against
 * `sha256` and decompressed into `tar`, a new file; `file` removed
 * whatever the outcome. 0, or -1 with `why` and no `tar` left. */
int hull_fetch_tar_xz(const char *url, const char *sha256, const char *file, const char *tar,
                      char *why, size_t cap);

#endif
