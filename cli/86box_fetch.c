/*
 * 86box_fetch.c -- `hull 86box fetch` (project.md section 5.0): 86Box's
 * AppImage for Linux x86_64 and the ROM set of its tag, pinned in
 * engine/86box.h, downloaded over https, each checked against its sha256,
 * into $XDG_CACHE_HOME/hull/86box-VERSION as 86Box.AppImage and roms/.
 * The AppImage is kept whole and nothing of it is run here. Built beside
 * where it goes and renamed in, so a fetch cut short leaves nothing that
 * looks whole.
 */

#define _GNU_SOURCE

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <unistd.h>

#include "86box.h"
#include "86box_fetch.h"
#include "kernel_fetch.h"
#include "store.h"

/* The one directory the ROM tarball's tree is under, roms-6.0, named
 * into `out`: 0, or -1 where there is not exactly one. */
static int only_entry(const char *dir, char *out, size_t cap)
{
	DIR           *list = opendir(dir);
	struct dirent *entry;
	int            found = 0;

	if (list == NULL)
		return -1;
	while ((entry = readdir(list)) != NULL) {
		if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
			continue;
		snprintf(out, cap, "%s", entry->d_name);
		found++;
	}
	closedir(list);
	return found == 1 ? 0 : -1;
}

int hull_86box_fetch(char *why, size_t cap)
{
	struct utsname host;
	char           dir[4096];
	char           work[4200];
	char           image[4400], roms[4400], unpacked[4400], tree[4300], made[4700], top[256];
	uint64_t       skipped = 0u;
	hull_unpack   *u = NULL;
	int            fd;
	int            status = -1;

	if (uname(&host) != 0 || strcmp(host.machine, "x86_64") != 0) {
		snprintf(why, cap, "86Box is fetched for an x86_64 Linux host, not %s",
		         uname(&host) == 0 ? host.machine : "this one");
		return -1;
	}
	if (hull_86box_dir(dir, sizeof dir) != 0) {
		snprintf(why, cap, "neither XDG_CACHE_HOME nor HOME names a place for 86Box");
		return -1;
	}
	snprintf(made, sizeof made, "%s/86Box.AppImage", dir);
	snprintf(roms, sizeof roms, "%s/roms/machines", dir);
	if (access(made, X_OK) == 0 && access(roms, R_OK) == 0) {
		printf("%s\n", dir);
		return 0;
	}
	for (char *slash = strchr(dir + 1, '/'); slash != NULL; slash = strchr(slash + 1, '/')) {
		*slash = '\0';
		if (mkdir(dir, 0755) != 0 && errno != EEXIST) {
			snprintf(why, cap, "%s: %s", dir, strerror(errno));
			return -1;
		}
		*slash = '/';
	}
	snprintf(work, sizeof work, "%s.part-XXXXXX", dir);
	if (mkdtemp(work) == NULL) {
		snprintf(why, cap, "%s: %s", work, strerror(errno));
		return -1;
	}
	snprintf(tree, sizeof tree, "%s/tree", work);
	snprintf(image, sizeof image, "%s/86Box.AppImage", tree);
	snprintf(roms, sizeof roms, "%s/roms.tar.gz", work);
	snprintf(unpacked, sizeof unpacked, "%s/unpacked", work);
	if (mkdir(tree, 0755) != 0 || mkdir(unpacked, 0755) != 0) {
		snprintf(why, cap, "%s: %s", work, strerror(errno));
		goto done;
	}
	fprintf(stderr, "hull: fetching 86Box %s, about 94 MB: %s\n", HULL_86BOX_VERSION,
	        HULL_86BOX_APPIMAGE_URL);
	if (hull_fetch_file(HULL_86BOX_APPIMAGE_URL, HULL_86BOX_APPIMAGE_SHA256, image, why, cap) != 0)
		goto done;
	if (chmod(image, 0755) != 0) {
		snprintf(why, cap, "%s: %s", image, strerror(errno));
		goto done;
	}
	fprintf(stderr, "hull: fetching its ROM set, about 73 MB: %s\n", HULL_86BOX_ROMS_URL);
	if (hull_fetch_file(HULL_86BOX_ROMS_URL, HULL_86BOX_ROMS_SHA256, roms, why, cap) != 0)
		goto done;
	if ((u = hull_unpack_begin(unpacked, why, cap)) == NULL)
		goto done;
	if ((fd = open(roms, O_RDONLY | O_CLOEXEC)) < 0) {
		snprintf(why, cap, "%s: %s", roms, strerror(errno));
		goto done;
	}
	{
		int applied = hull_unpack_layer(u, fd, &skipped, why, cap);
		hull_unpack *ending = u;

		close(fd);
		u = NULL;
		if (applied != 0 || hull_unpack_end(ending, why, cap) != 0)
			goto done;
	}
	/* The tarball's tree is under roms-TAG/, which becomes roms/. */
	if (only_entry(unpacked, top, sizeof top) != 0) {
		snprintf(why, cap, "the ROM set's tarball is not one directory");
		goto done;
	}
	snprintf(made, sizeof made, "%s/%s", unpacked, top);
	snprintf(roms, sizeof roms, "%s/roms", tree);
	if (rename(made, roms) != 0) {
		snprintf(why, cap, "%s: %s", roms, strerror(errno));
		goto done;
	}
	snprintf(made, sizeof made, "%s/machines", roms);
	if (access(made, R_OK) != 0) {
		snprintf(why, cap, "the ROM set holds no machines/");
		goto done;
	}
	if (rename(tree, dir) != 0) {
		snprintf(why, cap, "%s: %s", dir, strerror(errno));
		goto done;
	}
	printf("%s\n", dir);
	status = 0;
done:
	if (u != NULL) {
		char ignored[200];

		hull_unpack_end(u, ignored, sizeof ignored);
	}
	snprintf(roms, sizeof roms, "%s/roms.tar.gz", work);
	unlink(roms);
	{
		int parent = open(work, O_RDONLY | O_DIRECTORY | O_CLOEXEC);

		if (parent >= 0) {
			hull_store_remove_tree(parent, "tree");
			hull_store_remove_tree(parent, "unpacked");
			close(parent);
		}
	}
	rmdir(work);
	return status;
}
