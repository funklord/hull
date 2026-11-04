/*
 * vbox.h -- the VirtualBox the virtualbox engine runs inside a hull vm
 * (project.md section 12, phase 2): Oracle's Debian package, pinned by
 * version and sha256, fetched once by `hull virtualbox fetch` into the
 * user's cache and unpacked there as the user. The host's own install is
 * not used: its setuid binaries are --x to others, which a machine's share,
 * read as the user, cannot open. The kernel module is the host's, built
 * for the kernel a vm boots, so the version here must be the module's.
 */

#ifndef HULL_ENGINE_VBOX_H
#define HULL_ENGINE_VBOX_H

#include <stdio.h>
#include <stdlib.h>

#define HULL_VBOX_VERSION "7.1.18"
#define HULL_VBOX_URL                                                                     \
	"https://download.virtualbox.org/virtualbox/debian/pool/contrib/v/virtualbox-7.1/" \
	"virtualbox-7.1_7.1.18-173720~Debian~trixie_amd64.deb"
#define HULL_VBOX_SHA256 "b2b0837fc5fb4e9e812aa798c7ed0f00cee21e507aa0762c1257059ffa17ea15"
/* What a fetch made is whole once this is there. */
#define HULL_VBOX_HEADLESS "usr/lib/virtualbox/VBoxHeadless"

/* Where it is kept, $XDG_CACHE_HOME/hull/virtualbox-VERSION, into `out`. */
static inline int hull_vbox_dir(char *out, size_t cap)
{
	const char *cache = getenv("XDG_CACHE_HOME");
	const char *home = getenv("HOME");

	if (cache != NULL && cache[0] == '/')
		snprintf(out, cap, "%s/hull/virtualbox-%s", cache, HULL_VBOX_VERSION);
	else if (home != NULL && home[0] == '/')
		snprintf(out, cap, "%s/.cache/hull/virtualbox-%s", home, HULL_VBOX_VERSION);
	else
		return -1;
	return 0;
}

#endif /* HULL_ENGINE_VBOX_H */
