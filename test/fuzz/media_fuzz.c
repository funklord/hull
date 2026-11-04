/*
 * media_fuzz.c -- libFuzzer over engine/media.c, which reads somebody
 * else's disc. Each input is a disc from sector 16 on, the sixteen before
 * being empty, or one opening 0xEB a FAT disk whole, imported into a
 * collection of its own under $FUZZ_DIR/media-work,
 * and the run stopped where the import fails, which reassembling it exactly
 * inside it would show, or where anything was made beside the collection.
 * `make check-fuzz` runs it.
 */

#define _GNU_SOURCE
#include <dirent.h>
#include <fcntl.h>
#include <ftw.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "media.h"

static char base[3900];

static int gone(const char *path, const struct stat *st, int flag, struct FTW *f)
{
	(void)st;
	(void)flag;
	(void)f;
	remove(path);
	return 0;
}

int LLVMFuzzerInitialize(int *argc, char ***argv)
{
	const char *dir = getenv("FUZZ_DIR");

	(void)argc;
	(void)argv;
	snprintf(base, sizeof base, "%s/media-work", dir != NULL ? dir : ".");
	nftw(base, gone, 16, FTW_DEPTH | FTW_PHYS);
	mkdir(base, 0700);
	return 0;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	static const uint8_t nothing[2048];
	char                 image[4000];
	char                 collection[4000];
	char                 detail[600];
	hull_media_report    report;
	struct dirent       *entry;
	DIR                 *dir;
	int                  fd;

	snprintf(image, sizeof image, "%s/in.iso", base);
	snprintf(collection, sizeof collection, "%s/out", base);
	if ((fd = open(image, O_WRONLY | O_CREAT | O_TRUNC, 0600)) < 0)
		abort();
	for (unsigned i = 0u; (size == 0u || data[0] != 0xEBu) && i < 16u; i++)
		if (write(fd, nothing, sizeof nothing) != (ssize_t)sizeof nothing)
			abort();
	if (size != 0u && write(fd, data, size) != (ssize_t)size)
		abort();
	close(fd);
	if (hull_media_import(image, collection, &report, detail, sizeof detail) != 0) {
		fprintf(stderr, "media_fuzz: %s\n", detail);
		abort();
	}
	if ((dir = opendir(base)) == NULL)
		abort();
	while ((entry = readdir(dir)) != NULL)
		if (strcmp(entry->d_name, ".") != 0 && strcmp(entry->d_name, "..") != 0 &&
		    strcmp(entry->d_name, "in.iso") != 0 && strcmp(entry->d_name, "out") != 0)
			abort();
	closedir(dir);
	nftw(collection, gone, 16, FTW_DEPTH | FTW_PHYS);
	return 0;
}
