/*
 * collection_fuzz.c -- libFuzzer over engine/media.c's reading of a
 * collection, which a tar: or pack: source makes somebody else's input:
 * media.conf, layout.conf, regions.conf, a file under files/ and the runs
 * under quirks/sectors, each a part of the input, the parts split at a
 * line of "--" alone, the file's and a run's name the first line of its
 * part. The
 * collection is opened as a map and read, verified, reassembled, and,
 * where it reassembles, taken again from its own disk, the input's last
 * byte saying which blocks to call written -- more than changed, which a
 * taking must bear: it must succeed and give the same disk, the map and the reassembly
 * agreeing on whether it is whole. Under $FUZZ_DIR/collection-work.
 * `make check-fuzz` runs it.
 */

#define _GNU_SOURCE
#include <fcntl.h>
#include <ftw.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "media.h"

#define PARTS 16u
#define MOST (4u << 20)  /* a disc larger is not reassembled: time, not reach */

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
	snprintf(base, sizeof base, "%s/collection-work", dir != NULL ? dir : ".");
	nftw(base, gone, 16, FTW_DEPTH | FTW_PHYS);
	mkdir(base, 0700);
	return 0;
}

static void put(const char *path, const uint8_t *data, size_t n)
{
	int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);

	if (fd < 0)
		abort();
	if (n != 0u && write(fd, data, n) != (ssize_t)n)
		abort();
	close(fd);
}

static int image_read(void *context, void *out, size_t n, uint64_t at)
{
	return pread(*(int *)context, out, n, (off_t)at) == (ssize_t)n ? 0 : -1;
}

static uint64_t image_data(void *context, uint64_t at, uint64_t bytes)
{
	(void)context;
	(void)bytes;
	return at;
}

/* Written, by the mask: block b where bit b % 8 is set. */
static int said_written(void *context, uint64_t at, uint64_t n)
{
	unsigned mask = *(const unsigned *)context;

	for (uint64_t b = at / 4096u; b * 4096u < at + n; b++)
		if ((mask >> (b % 8u)) & 1u)
			return 1;
	return 0;
}

/* A part `name\ncontent` written as DIR/name, a plain name. */
static void named(const char *dir, const uint8_t *part, size_t length)
{
	const uint8_t *eol = memchr(part, '\n', length);
	size_t         name = eol != NULL ? (size_t)(eol - part) : length;
	char           file[64];
	char           path[4400];

	if (name == 0u || name >= sizeof file || memchr(part, '/', name) != NULL ||
	    memchr(part, '\0', name) != NULL)
		return;
	memcpy(file, part, name);
	file[name] = '\0';
	if (strcmp(file, ".") == 0 || strcmp(file, "..") == 0)
		return;
	snprintf(path, sizeof path, "%s/%s", dir, file);
	put(path, eol != NULL ? eol + 1 : part + name, eol != NULL ? length - name - 1u : 0u);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	static const char *const names[] = { "media.conf", "layout.conf", "regions.conf" };
	const uint8_t     *part[PARTS] = { NULL };
	size_t             length[PARTS] = { 0 };
	unsigned           parts = 0u;
	char               dir[4000], path[4400], image[4000], again[4000], again_image[4000];
	char               detail[600];
	hull_media_map    *map;
	uint64_t           bytes = 0u;
	int                verified = -1, written = -1;
	size_t             start = 0u;

	/* The parts, at each "\n--\n". */
	for (size_t i = 0u; i <= size && parts < PARTS; i++) {
		if (i == size || (i + 4u <= size && memcmp(data + i, "\n--\n", 4u) == 0)) {
			part[parts] = data + start;
			length[parts++] = i - start;
			start = i + 4u;
			i += 3u;
		}
	}
	snprintf(dir, sizeof dir, "%s/c", base);
	snprintf(image, sizeof image, "%s/c.img", base);
	snprintf(again, sizeof again, "%s/again", base);
	snprintf(again_image, sizeof again_image, "%s/again.img", base);
	nftw(dir, gone, 16, FTW_DEPTH | FTW_PHYS);
	nftw(again, gone, 16, FTW_DEPTH | FTW_PHYS);
	unlink(image);
	unlink(again_image);
	if (mkdir(dir, 0700) != 0)
		abort();
	snprintf(path, sizeof path, "%s/files", dir);
	mkdir(path, 0700);
	snprintf(path, sizeof path, "%s/quirks", dir);
	mkdir(path, 0700);
	snprintf(path, sizeof path, "%s/quirks/sectors", dir);
	mkdir(path, 0700);
	for (unsigned i = 0u; i < 3u && i < parts; i++) {
		snprintf(path, sizeof path, "%s/%s", dir, names[i]);
		put(path, part[i], length[i]);
	}
	snprintf(path, sizeof path, "%s/files", dir);
	if (parts > 3u)
		named(path, part[3], length[3]);
	snprintf(path, sizeof path, "%s/quirks/sectors", dir);
	for (unsigned k = 4u; k < parts; k++)
		named(path, part[k], length[k]);

	if ((map = hull_media_map_open(dir, &bytes, detail, sizeof detail)) != NULL) {
		uint8_t buffer[4096];

		if (hull_media_map_size(map) != bytes)
			abort();
		for (uint64_t at = 0u; at < bytes && at < (64u << 10); at += 1000u)
			(void)hull_media_map_read(map, at, buffer, bytes - at < sizeof buffer ?
			                          (size_t)(bytes - at) : sizeof buffer);
		if (bytes != 0u)
			(void)hull_media_map_read(map, bytes - 1u, buffer, 1u);
		if (hull_media_map_read(map, bytes, buffer, 1u) == 0)
			abort();  /* past the end */
		if (bytes <= MOST)
			verified = hull_media_map_verify(map, detail, sizeof detail);
		hull_media_map_close(map);
	}
	if (bytes <= MOST && map != NULL) {
		int fd = open(image, O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0600);

		if (fd < 0)
			abort();
		written = hull_media_write(dir, fd, detail, sizeof detail);
		/* Two readings of one collection: whole by one is whole by both. */
		if ((written == 0) != (verified == 0)) {
			fprintf(stderr, "collection_fuzz: verify %d, write %d: %s\n", verified, written, detail);
			abort();
		}
		if (written == 0) {
			hull_media_reader  reader = { image_read, image_data, &fd };
			unsigned           mask = size != 0u ? data[size - 1u] : 0u;
			hull_media_changes changes = { said_written, &mask };
			hull_media_report  report;
			int                out;

			/* Its own disk, nothing written: a taking from it is whole. */
			if (hull_media_take(&reader, bytes, 0, dir, &changes, again, &report, detail,
			                    sizeof detail) != 0) {
				fprintf(stderr, "collection_fuzz: a taking of its own disk: %s\n", detail);
				abort();
			}
			if ((out = open(again_image, O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0600)) < 0)
				abort();
			if (hull_media_write(again, out, detail, sizeof detail) != 0) {
				fprintf(stderr, "collection_fuzz: the taking does not reassemble: %s\n", detail);
				abort();
			}
			for (uint64_t at = 0u; at < bytes; at += 4096u) {
				uint8_t a[4096], b[4096];
				size_t  n = bytes - at < 4096u ? (size_t)(bytes - at) : 4096u;

				if (pread(fd, a, n, (off_t)at) != (ssize_t)n ||
				    pread(out, b, n, (off_t)at) != (ssize_t)n || memcmp(a, b, n) != 0)
					abort();
			}
			close(out);
		}
		close(fd);
	}
	nftw(dir, gone, 16, FTW_DEPTH | FTW_PHYS);
	nftw(again, gone, 16, FTW_DEPTH | FTW_PHYS);
	unlink(image);
	unlink(again_image);
	return 0;
}
