/*
 * unpack_fuzz.c -- libFuzzer over store/unpack.c, which applies a layer a
 * registry sends: each input unpacked into a tree of its own under
 * $FUZZ_DIR/work, the tree removed after, and the run stopped where
 * anything was made beside it, the unpacker's whole job being to keep
 * every path beneath. `make check-fuzz` runs it.
 */

#define _GNU_SOURCE
#include <dirent.h>
#include <fcntl.h>
#include <ftw.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include "store.h"

static char base[4096];

static int gone(const char *path, const struct stat *st, int flag, struct FTW *f)
{
	(void)st;
	(void)f;
	if (flag == FTW_DP || flag == FTW_D)
		rmdir(path);
	else
		unlink(path);
	return 0;
}

static int writable(const char *path, const struct stat *st, int flag, struct FTW *f)
{
	(void)f;
	(void)st;
	if (flag == FTW_D || flag == FTW_DNR)
		chmod(path, 0700);
	return 0;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	char           dir[4200];
	char           why[512];
	uint64_t       skipped = 0;
	int            fd = memfd_create("layer", MFD_CLOEXEC);
	hull_unpack   *u;
	DIR           *list;
	struct dirent *e;
	int            entries = 0;

	if (base[0] == '\0') {
		snprintf(base, sizeof base, "%s/work", getenv("FUZZ_DIR"));
		mkdir(base, 0700);
	}
	snprintf(dir, sizeof dir, "%s/tree", base);
	mkdir(dir, 0700);
	if (fd < 0 || write(fd, data, size) != (ssize_t)size || lseek(fd, 0, SEEK_SET) != 0)
		abort();
	u = hull_unpack_begin(dir, why, sizeof why);
	if (u != NULL) {
		hull_owner *owners;
		size_t      count;

		(void)hull_unpack_layer(u, fd, &skipped, why, sizeof why);
		hull_unpack_owners(u, &owners, &count);
		hull_owners_free(owners, count);
		(void)hull_unpack_end(u, why, sizeof why);
	}
	close(fd);
	/* Nothing beside the tree: every path is made beneath it. */
	list = opendir(base);
	while (list != NULL && (e = readdir(list)) != NULL)
		if (strcmp(e->d_name, ".") != 0 && strcmp(e->d_name, "..") != 0)
			entries++;
	if (list != NULL)
		closedir(list);
	if (entries != 1) {
		fprintf(stderr, "ESCAPE: %d entries beside the tree\n", entries);
		abort();
	}
	nftw(dir, writable, 64, FTW_PHYS);
	nftw(dir, gone, 64, FTW_DEPTH | FTW_PHYS);
	return 0;
}
