/*
 * vbox_fetch.c -- `hull virtualbox fetch` (project.md section 12, phase
 * 2): Oracle's VirtualBox package, pinned in engine/vbox.h, downloaded
 * over https and checked against its sha256, its data unpacked as the
 * user into $XDG_CACHE_HOME/hull/virtualbox-VERSION. The virtualbox
 * engine's outer vm runs it from there, made root's in the guest, the
 * host's own install being unreadable to a share (vbox.h). Built beside
 * where it goes and renamed in, so a fetch cut short leaves nothing that
 * looks whole.
 */

#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "kernel_fetch.h"
#include "store.h"
#include "vbox.h"
#include "vbox_fetch.h"

int hull_vbox_fetch(char *why, size_t cap)
{
	char         dir[4096];
	char         work[4200];
	char         deb[4300], tar[4300], tree[4300], made[4400];
	uint64_t     skipped = 0u;
	hull_unpack *u = NULL;
	int          fd;
	int          status = -1;

	if (hull_vbox_dir(dir, sizeof dir) != 0) {
		snprintf(why, cap, "neither XDG_CACHE_HOME nor HOME names a place for VirtualBox");
		return -1;
	}
	snprintf(made, sizeof made, "%s/%s", dir, HULL_VBOX_HEADLESS);
	if (access(made, R_OK) == 0) {
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
	snprintf(deb, sizeof deb, "%s/package.deb", work);
	snprintf(tar, sizeof tar, "%s/data.tar", work);
	snprintf(tree, sizeof tree, "%s/tree", work);
	if (mkdir(tree, 0755) != 0) {
		snprintf(why, cap, "%s: %s", tree, strerror(errno));
		goto done;
	}
	fprintf(stderr, "hull: fetching VirtualBox %s, about 110 MB, from download.virtualbox.org\n",
	        HULL_VBOX_VERSION);
	if (hull_fetch_deb(HULL_VBOX_URL, HULL_VBOX_SHA256, deb, tar, why, cap) != 0)
		goto done;
	if ((u = hull_unpack_begin(tree, why, cap)) == NULL)
		goto done;
	if ((fd = open(tar, O_RDONLY | O_CLOEXEC)) < 0) {
		snprintf(why, cap, "%s: %s", tar, strerror(errno));
		goto done;
	}
	{
		int applied = hull_unpack_layer(u, fd, &skipped, why, cap);

		close(fd);
		unlink(tar);
		if (applied != 0)
			goto done;
	}
	{
		hull_unpack *ending = u;

		u = NULL;
		if (hull_unpack_end(ending, why, cap) != 0)
			goto done;
	}
	snprintf(made, sizeof made, "%s/%s", tree, HULL_VBOX_HEADLESS);
	if (access(made, R_OK) != 0) {
		snprintf(why, cap, "the package holds no %s", HULL_VBOX_HEADLESS);
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
	unlink(deb);
	unlink(tar);
	{
		int parent = open(work, O_RDONLY | O_DIRECTORY | O_CLOEXEC);

		if (parent >= 0) {
			hull_store_remove_tree(parent, "tree");
			close(parent);
		}
	}
	rmdir(work);
	return status;
}
