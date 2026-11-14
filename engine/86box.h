/*
 * 86box.h -- 86Box, the pc class's engine for a period's exact PC
 * (project.md section 5.0): its release for Linux x86_64 and the ROM set
 * of the same tag, pinned by sha256, which `hull 86box fetch` puts in the
 * user's cache and a machine runs from. The ROMs are its machines', video
 * cards' and sound cards' firmware, fetched as the holder asked
 * (2026-11-14, "we fetch what ever we can, such as BIOS roms").
 */

#ifndef HULL_ENGINE_86BOX_H
#define HULL_ENGINE_86BOX_H

#include <stdio.h>
#include <stdlib.h>

#define HULL_86BOX_VERSION "6.0"

/* The release's AppImage, build 9001, and the ROM set tagged v6.0. A pin
 * moved is both moved together: each 86Box build reads its own tag's
 * ROMs. */
#define HULL_86BOX_APPIMAGE_URL \
	"https://github.com/86Box/86Box/releases/download/v6.0/86Box-Linux-x86_64-b9001.AppImage"
#define HULL_86BOX_APPIMAGE_SHA256 \
	"535d3c329f8c32b601103970c86d80aa80298f345452c3524ea2f433624727a4"
#define HULL_86BOX_ROMS_URL "https://codeload.github.com/86Box/roms/tar.gz/refs/tags/v6.0"
#define HULL_86BOX_ROMS_SHA256 \
	"38fccaf4a7b48e581ccdfee2dc3cc0545e7d3a778bc25611e902a117eec571e6"

/* Where it is kept, $XDG_CACHE_HOME/hull/86box-VERSION, into `out`: 0, or
 * -1 where neither XDG_CACHE_HOME nor HOME is set. */
static inline int hull_86box_dir(char *out, size_t cap)
{
	const char *cache = getenv("XDG_CACHE_HOME");
	const char *home = getenv("HOME");

	if (cache != NULL && cache[0] == '/')
		snprintf(out, cap, "%s/hull/86box-%s", cache, HULL_86BOX_VERSION);
	else if (home != NULL && home[0] == '/')
		snprintf(out, cap, "%s/.cache/hull/86box-%s", home, HULL_86BOX_VERSION);
	else
		return -1;
	return 0;
}

#endif
