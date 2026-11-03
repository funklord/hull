/*
 * kernel.h -- the kernel hull fetches for a vm where the host's own cannot
 * be read (Appendix F): Debian's generic amd64 kernel, pinned by version
 * and by its package's sha256, unpacked into the user's cache by `hull
 * kernel fetch` with its modules and their modules.dep beside it. The
 * qemu engine only looks there; the network is the command's, said.
 */
#ifndef HULL_ENGINE_KERNEL_H
#define HULL_ENGINE_KERNEL_H

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>

#define HULL_KERNEL_VERSION "6.12.107-1"
#define HULL_KERNEL_RELEASE "6.12.107+deb13-amd64"
#define HULL_KERNEL_URL     "https://deb.debian.org/debian/pool/main/l/linux/" \
                            "linux-image-6.12.107+deb13-amd64-unsigned_6.12.107-1_amd64.deb"
#define HULL_KERNEL_SHA256  "7d13cd82a1d377e40826c313f5dbbdb2edf1b48c713a375e4cfe5e207749cb4e"

/* The fetched kernel's directory, $XDG_CACHE_HOME/hull/kernel-VERSION or
 * under ~/.cache: its boot/vmlinuz-RELEASE and lib/modules/RELEASE. 0, or
 * -1 where neither variable names a place. */
static inline int hull_kernel_dir(char *out, size_t cap)
{
	const char *cache = getenv("XDG_CACHE_HOME");
	const char *home = getenv("HOME");

	if (cache != NULL && cache[0] == '/')
		snprintf(out, cap, "%s/hull/kernel-%s", cache, HULL_KERNEL_VERSION);
	else if (home != NULL && home[0] == '/')
		snprintf(out, cap, "%s/.cache/hull/kernel-%s", home, HULL_KERNEL_VERSION);
	else
		return -1;
	return 0;
}

#endif
