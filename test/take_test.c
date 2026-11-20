/*
 * take_test.c -- a taking that reads only what changed (engine/media.c,
 * hull_media_take; project.md section 3.13): a FAT disk imported, files
 * then written, changed and deleted on a copy of it by mtools, and the
 * copy taken into a new collection from the first, told which 4 KiB
 * blocks differ as the NBD server's bitmap would. The new collection must
 * give the copy byte for byte, have read a small part of the disk to do
 * so, and hold the file nobody wrote as the first's own, linked. Its tools
 * are mkfs.vfat and mtools, which share no code with hull. It writes only
 * under $BUILD_DIR/test/take, cleared at its start.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "media.h"

#define DISK   (256ull << 20)
#define BLOCK  4096u

static int  cases;
static int  failures;
static char root[4096];

static void check(const char *name, int ok, const char *detail)
{
	cases++;
	if (ok)
		return;
	failures++;
	printf("FAIL: %s%s%s\n", name, detail != NULL && detail[0] != '\0' ? ": " : "",
	       detail != NULL ? detail : "");
}

static int run(const char *format, ...) __attribute__((format(printf, 1, 2)));
static int run(const char *format, ...)
{
	char    command[16384];
	va_list args;

	va_start(args, format);
	vsnprintf(command, sizeof command, format, args);
	va_end(args);
	return system(command);
}

/* A file of `bytes` whose content says `seed`, onto the disk as NAME. */
static int put(const char *image, const char *name, size_t bytes, unsigned seed)
{
	char  path[4400];
	FILE *f;

	snprintf(path, sizeof path, "%s/src.bin", root);
	if ((f = fopen(path, "w")) == NULL)
		return -1;
	for (size_t i = 0u; i < bytes; i++)
		fputc((int)((i * 31u + seed * 7u + i / 4096u) & 0xffu), f);
	fclose(f);
	return run("MTOOLS_SKIP_CHECK=1 mcopy -o -i '%s' '%s' '::%s'", image, path, name);
}

typedef struct counted {
	int      fd;
	uint64_t read;
} counted;

static int counted_read(void *context, void *out, size_t n, uint64_t at)
{
	counted *c = context;
	uint8_t *p = out;

	c->read += n;
	while (n > 0u) {
		ssize_t got = pread(c->fd, p, n, (off_t)at);

		if (got <= 0)
			return -1;
		p += got;
		at += (uint64_t)got;
		n -= (size_t)got;
	}
	return 0;
}

/* As the NBD server's view: it cannot say where the zeros are. */
static uint64_t counted_data(void *context, uint64_t at, uint64_t bytes)
{
	(void)context;
	(void)bytes;
	return at;
}

typedef struct differs {
	uint8_t *bits;
} differs;

static int differs_written(void *context, uint64_t at, uint64_t n)
{
	const differs *d = context;

	for (uint64_t b = at / BLOCK; b * BLOCK < at + n; b++)
		if ((d->bits[b / 8u] >> (b % 8u)) & 1u)
			return 1;
	return 0;
}

/* Each 4 KiB block where `a` and `b` differ, as the guest's writes. */
static uint8_t *diff(const char *a, const char *b)
{
	uint8_t *bits = calloc(DISK / BLOCK / 8u, 1u);
	int      x = open(a, O_RDONLY | O_CLOEXEC);
	int      y = open(b, O_RDONLY | O_CLOEXEC);
	uint8_t  p[BLOCK], q[BLOCK];

	for (uint64_t i = 0u; bits != NULL && x >= 0 && y >= 0 && i < DISK / BLOCK; i++) {
		if (pread(x, p, BLOCK, (off_t)(i * BLOCK)) != BLOCK ||
		    pread(y, q, BLOCK, (off_t)(i * BLOCK)) != BLOCK) {
			free(bits);
			bits = NULL;
			break;
		}
		if (memcmp(p, q, BLOCK) != 0)
			bits[i / 8u] |= (uint8_t)(1u << (i % 8u));
	}
	if (x >= 0)
		close(x);
	if (y >= 0)
		close(y);
	return bits;
}

/* Whether the collection's file NAME holds what put() wrote with `seed`. */
static int holds(const char *collection, const char *name, size_t bytes, unsigned seed)
{
	char  path[4400];
	FILE *f;
	int   same = 1;

	snprintf(path, sizeof path, "%s/%s/files/%s", root, collection, name);
	if ((f = fopen(path, "re")) == NULL)
		return 0;
	for (size_t i = 0u; i < bytes && same; i++)
		same = fgetc(f) == (int)((i * 31u + seed * 7u + i / 4096u) & 0xffu);
	same = same && fgetc(f) == EOF;
	fclose(f);
	return same;
}

/* Whether any block of the region holding the layout line of NAME in the
 * collection differs between images `a` and `b`. */
static int region_written(const char *collection, const char *name, const char *a, const char *b)
{
	char     path[4400];
	char     line[4400];
	FILE    *f;
	uint64_t sector = UINT64_MAX;
	uint8_t *bits = diff(a, b);
	int      any = 1;

	snprintf(path, sizeof path, "%s/%s/layout.conf", root, collection);
	if (bits == NULL || (f = fopen(path, "re")) == NULL) {
		free(bits);
		return 1;
	}
	while (fgets(line, sizeof line, f) != NULL) {
		uint64_t s, n;
		char     file[256];

		if (sscanf(line, "%" SCNu64 " %" SCNu64 " %255s", &s, &n, file) == 3 &&
		    strcmp(file, name) == 0)
			sector = s;
	}
	fclose(f);
	if (sector != UINT64_MAX) {
		uint64_t first = sector * 512u / (1u << 20) * (1u << 20) / BLOCK;

		any = 0;
		for (uint64_t k = first; k < first + (1u << 20) / BLOCK; k++)
			if ((bits[k / 8u] >> (k % 8u)) & 1u)
				any = 1;
	}
	free(bits);
	return any;
}

static ino_t inode(const char *collection, const char *name)
{
	char        path[4400];
	struct stat st;

	snprintf(path, sizeof path, "%s/%s/files/%s", root, collection, name);
	return stat(path, &st) == 0 ? st.st_ino : 0;
}

static int conf_has(const char *collection, const char *what)
{
	char  path[4400];
	char  text[4096] = "";
	FILE *f;

	snprintf(path, sizeof path, "%s/%s/media.conf", root, collection);
	if ((f = fopen(path, "re")) == NULL)
		return 0;
	(void)!fread(text, 1u, sizeof text - 1u, f);
	fclose(f);
	return strstr(text, what) != NULL;
}

/* The collection written out, compared with `image` byte for byte. */
static int gives(const char *collection, const char *image, char *detail, size_t cap)
{
	char path[4400];
	char out[4400];
	int  fd;
	int  status;

	snprintf(path, sizeof path, "%s/%s", root, collection);
	snprintf(out, sizeof out, "%s/%s.out", root, collection);
	unlink(out);
	if ((fd = open(out, O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0644)) < 0)
		return -1;
	status = hull_media_write(path, fd, detail, cap);
	close(fd);
	if (status != 0)
		return -1;
	status = run("cmp -s '%s' '%s'", out, image);
	unlink(out);
	if (status != 0)
		snprintf(detail, cap, "%s does not give %s", collection, image);
	return status == 0 ? 0 : -1;
}

/* `image` taken from the collection `from` into `to`, the blocks it
 * differs from `before` by its writes; the bytes read into `*read`. */
static int take(const char *image, const char *before, const char *from, const char *to,
                uint64_t *read, char *detail, size_t cap)
{
	counted            c = { open(image, O_RDONLY | O_CLOEXEC), 0u };
	hull_media_reader  reader = { counted_read, counted_data, &c };
	differs            d = { diff(before, image) };
	hull_media_changes changes = { differs_written, &d };
	hull_media_report  report;
	char               prior[4400];
	char               dir[4400];
	int                status;

	snprintf(prior, sizeof prior, "%s/%s", root, from);
	snprintf(dir, sizeof dir, "%s/%s", root, to);
	if (c.fd < 0 || d.bits == NULL) {
		snprintf(detail, cap, "%s cannot be read", image);
		status = -1;
	} else {
		status = hull_media_take(&reader, DISK, 0, from != NULL ? prior : NULL,
		                         from != NULL ? &changes : NULL, dir, &report, detail, cap);
	}
	if (c.fd >= 0)
		close(c.fd);
	free(d.bits);
	*read = c.read;
	return status;
}

int main(void)
{
	const char       *build = getenv("BUILD_DIR");
	char              first[4400], second[4400], third[4400];
	char              path[4400], detail[4400];
	hull_media_report report;
	uint64_t          read;
	hull_media_map   *map;
	uint64_t          bytes;
	int               fd;

	snprintf(root, sizeof root, "%s/test/take", build != NULL ? build : "build");
	if (strstr(root, "/test/take") == NULL || run("rm -rf '%s' && mkdir -p '%s'", root, root) != 0) {
		printf("take: cannot clear %s\n", root);
		return 1;
	}
	setenv("PATH", "/usr/local/bin:/usr/bin:/bin:/usr/sbin:/sbin", 1);
	snprintf(first, sizeof first, "%s/first.img", root);
	snprintf(second, sizeof second, "%s/second.img", root);
	snprintf(third, sizeof third, "%s/third.img", root);
	if ((fd = open(first, O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0644)) < 0 ||
	    ftruncate(fd, (off_t)DISK) != 0 || close(fd) != 0 ||
	    run("mkfs.vfat -n TAKE '%s' > /dev/null", first) != 0 ||
	    put(first, "KEPT.BIN", 40u << 20, 1u) != 0 || put(first, "CHANGED.TXT", 100000u, 2u) != 0 ||
	    put(first, "PAD1.BIN", 2u << 20, 6u) != 0 || put(first, "GONE.TXT", 70000u, 3u) != 0 ||
	    put(first, "PAD2.BIN", 2u << 20, 7u) != 0) {
		printf("take: the first disk cannot be made (mkfs.vfat, mtools)\n");
		return 1;
	}

	/* An import is format 2: regions beside the whole sha256. */
	snprintf(path, sizeof path, "%s/a", root);
	check("the disk imported", hull_media_import(first, path, &report, detail, sizeof detail) == 0,
	      detail);
	check("its media.conf has (regions)", conf_has("a", "(regions \""), NULL);
	check("and its whole (sha256)", conf_has("a", "(sha256 \""), NULL);
	check("and it gives the disk", gives("a", first, detail, sizeof detail) == 0, detail);

	/* A file changed, one written, one deleted. */
	if (run("cp --sparse=always '%s' '%s'", first, second) != 0 ||
	    put(second, "CHANGED.TXT", 100000u, 9u) != 0 || put(second, "NEW.TXT", 50000u, 4u) != 0 ||
	    run("MTOOLS_SKIP_CHECK=1 mdel -i '%s' ::GONE.TXT", second) != 0) {
		printf("take: the second disk cannot be made\n");
		return 1;
	}
	check("the changed disk taken from the first collection",
	      take(second, first, "a", "b", &read, detail, sizeof detail) == 0, detail);
	snprintf(detail, sizeof detail, "%" PRIu64 " bytes of %llu", read, DISK);
	check("reading under a tenth of the disk", read < DISK / 10u, detail);
	check("it gives the changed disk", gives("b", second, detail, sizeof detail) == 0, detail);
	check("the changed file holds its new bytes", holds("b", "CHANGED.TXT", 100000u, 9u), NULL);
	check("the written file holds its bytes", holds("b", "NEW.TXT", 50000u, 4u), NULL);
	check("the file nobody wrote holds its own", holds("b", "KEPT.BIN", 40u << 20, 1u), NULL);
	/* What makes the deletion a case of its own: its region written by
	 * nothing, only the lines saying what is there changed. */
	check("the deleted file's region was not written",
	      !region_written("a", "GONE.TXT", first, second), NULL);
	check("the file nobody wrote is the first's, linked",
	      inode("b", "KEPT.BIN") != 0 && inode("b", "KEPT.BIN") == inode("a", "KEPT.BIN"), NULL);
	check("the changed file is new", inode("b", "CHANGED.TXT") != inode("a", "CHANGED.TXT"), NULL);
	check("the written file is there", inode("b", "NEW.TXT") != 0, NULL);
	check("the deleted file is not", inode("b", "GONE.TXT") == 0, NULL);
	check("its media.conf has (regions)", conf_has("b", "(regions \""), NULL);
	check("and no whole (sha256), which it did not read", !conf_has("b", "(sha256 "), NULL);
	snprintf(path, sizeof path, "%s/b", root);
	map = hull_media_map_open(path, &bytes, detail, sizeof detail);
	check("it verifies where it lies", map != NULL &&
	      hull_media_map_verify(map, detail, sizeof detail) == 0, detail);
	hull_media_map_close(map);

	/* A taking from a taking: no sha256 to start from, the regions are. */
	if (run("cp --sparse=always '%s' '%s'", second, third) != 0 ||
	    put(third, "NEW.TXT", 50000u, 5u) != 0) {
		printf("take: the third disk cannot be made\n");
		return 1;
	}
	check("a taking taken again", take(third, second, "b", "c", &read, detail, sizeof detail) == 0,
	      detail);
	check("it gives the third disk", gives("c", third, detail, sizeof detail) == 0, detail);
	check("the file written again holds its new bytes", holds("c", "NEW.TXT", 50000u, 5u), NULL);
	check("the file nobody wrote still linked",
	      inode("c", "KEPT.BIN") != 0 && inode("c", "KEPT.BIN") == inode("a", "KEPT.BIN"), NULL);

	/* Whole where the prior cannot be trusted: its regions.conf edited. */
	snprintf(path, sizeof path, "%s/a/regions.conf", root);
	if (run("sed -i '1s/^0x0 ./0x0 x/' '%s'", path) == 0) {
		snprintf(path, sizeof path, "%s/a", root);
		map = hull_media_map_open(path, &bytes, detail, sizeof detail);
		check("an edited regions.conf refused", map == NULL, NULL);
		hull_media_map_close(map);
		check("and a taking from it is whole",
		      take(second, first, "a", "d", &read, detail, sizeof detail) == 0 && read >= DISK &&
		      conf_has("d", "(sha256 \""), detail);
		check("and gives the disk", gives("d", second, detail, sizeof detail) == 0, detail);
	}

	printf("take: %d cases, %d failed\n", cases, failures);
	if (failures == 0)
		run("rm -rf '%s'", root);
	return failures == 0 ? 0 : 1;
}
