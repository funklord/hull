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

/* Where a pinned package is fetched from: Debian's pool, which keeps only
 * the versions a release still has, then the snapshot of the archive as
 * it stood when the pins were taken, which keeps every version for good.
 * A point release moves libc6 or the kernel on, and the pool's copy goes;
 * the snapshot's does not. Every pin is in this snapshot, checked when it
 * was set; a pin moved later is checked against a newer one. */
#define HULL_DEBIAN_POOL     "https://deb.debian.org/debian/pool/main/"
#define HULL_DEBIAN_SNAPSHOT "https://snapshot.debian.org/archive/debian/20261102T000000Z/pool/main/"

#define HULL_KERNEL_VERSION "6.12.107-1"
#define HULL_KERNEL_RELEASE "6.12.107+deb13-amd64"
#define HULL_KERNEL_PATH    "l/linux/linux-image-6.12.107+deb13-amd64-unsigned_6.12.107-1_amd64.deb"
#define HULL_KERNEL_SHA256  "7d13cd82a1d377e40826c313f5dbbdb2edf1b48c713a375e4cfe5e207749cb4e"

/* Each arch's kernel, the same Debian version: the host's own, amd64,
 * and arm64 for an emu machine of that arch (section 5.3), which boots
 * under qemu-system-aarch64's virt machine. */
typedef struct hull_kernel_pin {
	const char *arch;
	const char *release;   /* its modules' directory and its vmlinuz's suffix */
	const char *path;      /* under HULL_DEBIAN_POOL and _SNAPSHOT */
	const char *sha256;    /* the .deb's */
	unsigned    megabytes; /* about, for the fetch to say */
} hull_kernel_pin;

static const hull_kernel_pin hull_kernel_pins[] = {
	{ "amd64", HULL_KERNEL_RELEASE, HULL_KERNEL_PATH, HULL_KERNEL_SHA256, 108u },
	{ "arm64", "6.12.107+deb13-arm64",
	  "l/linux/linux-image-6.12.107+deb13-arm64-unsigned_6.12.107-1_arm64.deb",
	  "2f986ade1a1336423adf2316ea89c3525dd4c05a646b2141626490ca2f388339", 93u },
};

/* `arch`'s pin, or NULL where hull fetches no kernel for it. */
static inline const hull_kernel_pin *hull_kernel_pin_of(const char *arch)
{
	for (size_t i = 0u; i < sizeof hull_kernel_pins / sizeof hull_kernel_pins[0]; i++) {
		const char *a = hull_kernel_pins[i].arch;
		size_t      k = 0u;

		while (a[k] != '\0' && a[k] == arch[k])
			k++;
		if (a[k] == '\0' && arch[k] == '\0')
			return &hull_kernel_pins[i];
	}
	return NULL;
}

/* The fetched kernel's directory for `arch`, $XDG_CACHE_HOME/hull/
 * kernel-VERSION for amd64, as it always was, and kernel-ARCH-VERSION for
 * another, or under ~/.cache: its boot/vmlinuz-RELEASE and
 * lib/modules/RELEASE. 0, or -1 where neither variable names a place. */
static inline int hull_kernel_dir_of(const char *arch, char *out, size_t cap)
{
	const char *cache = getenv("XDG_CACHE_HOME");
	const char *home = getenv("HOME");
	char        name[64];
	int         host = arch[0] == 'a' && arch[1] == 'm' && arch[2] == 'd' && arch[3] == '6' &&
	                   arch[4] == '4' && arch[5] == '\0';

	if (host)
		snprintf(name, sizeof name, "kernel-%s", HULL_KERNEL_VERSION);
	else
		snprintf(name, sizeof name, "kernel-%s-%s", arch, HULL_KERNEL_VERSION);
	if (cache != NULL && cache[0] == '/')
		snprintf(out, cap, "%s/hull/%s", cache, name);
	else if (home != NULL && home[0] == '/')
		snprintf(out, cap, "%s/.cache/hull/%s", home, name);
	else
		return -1;
	return 0;
}

/* The host's, amd64's. */
static inline int hull_kernel_dir(char *out, size_t cap)
{
	return hull_kernel_dir_of("amd64", out, cap);
}

#endif
