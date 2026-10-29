/*
 * iso.c -- a directory as an ISO9660 image, the same bytes for the same
 * tree (iso.h).
 *
 * ECMA-119 at level 1, as a DOS-era disc is: sixteen empty system
 * sectors, the primary volume descriptor and its terminator, the path
 * table both ways round, every directory, then every file, each
 * contiguous. The tree is read breadth first with each directory's
 * entries sorted, which is the path table's own order, so a directory's
 * number is its place in that walk. Every date is 1980-01-01, so nothing
 * of the host's clock reaches the image.
 */

#define _GNU_SOURCE

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "iso.h"

#define SECTOR    2048u
#define MAX_DEPTH 8u          /* ECMA-119's, the root one of them */

typedef struct node {
	char     id[16];      /* NAME or NAME.EXT, without ";1" */
	char    *path;        /* on the host */
	int      dir;
	uint64_t size;        /* a file's bytes; a directory's extent, written */
	unsigned parent;      /* a directory's index; the root its own */
	unsigned depth;
	unsigned first;       /* a directory's children, nodes[first..first+count) */
	unsigned count;
	unsigned number;      /* a directory's place in the path table, from 1 */
	uint32_t extent;      /* its first sector */
} node;

typedef struct image {
	node    *nodes;
	unsigned count;
	unsigned cap;
	char    *detail;
	size_t   detail_cap;
	int      fd;
	uint8_t  sector[SECTOR];
} image;

static int fail(image *im, const char *what, const char *path)
{
	snprintf(im->detail, im->detail_cap, "%s%s%s", path != NULL ? path : "",
	         path != NULL ? ": " : "", what);
	return -1;
}

static void both16(uint8_t *at, uint32_t value)
{
	at[0] = (uint8_t)value;
	at[1] = (uint8_t)(value >> 8);
	at[2] = (uint8_t)(value >> 8);
	at[3] = (uint8_t)value;
}

static void both32(uint8_t *at, uint32_t value)
{
	for (unsigned i = 0u; i < 4u; i++) {
		at[i]     = (uint8_t)(value >> (8u * i));
		at[7u - i] = (uint8_t)(value >> (8u * i));
	}
}

/* `name` as a level-1 identifier into `id`: 1 to 8 d-characters, then a
 * dot and at most 3 more for a file; capitals. 0 where it cannot be. */
static int identifier(const char *name, int dir, char id[16])
{
	const char *dot = strchr(name, '.');
	size_t      base = dot != NULL ? (size_t)(dot - name) : strlen(name);
	size_t      ext = dot != NULL ? strlen(dot + 1) : 0u;
	size_t      n = 0u;

	if (base == 0u || base > 8u || ext > 3u || (dir && dot != NULL) ||
	    (dot != NULL && strchr(dot + 1, '.') != NULL))
		return 0;
	for (const char *c = name; *c != '\0'; c++) {
		char up = (char)toupper((unsigned char)*c);

		if (*c != '.' && !((up >= 'A' && up <= 'Z') || (up >= '0' && up <= '9') || up == '_'))
			return 0;
		id[n++] = up;
	}
	id[n] = '\0';
	return 1;
}

static int by_id(const void *a, const void *b)
{
	return strcmp(((const node *)a)->id, ((const node *)b)->id);
}

static int add(image *im, const node *n)
{
	if (im->count == im->cap) {
		unsigned cap = im->cap != 0u ? im->cap * 2u : 64u;
		node    *grown = realloc(im->nodes, cap * sizeof *grown);

		if (grown == NULL)
			return -1;
		im->nodes = grown;
		im->cap   = cap;
	}
	im->nodes[im->count++] = *n;
	return 0;
}

/* Directory `at`'s entries, sorted, appended as its children. */
static int read_dir(image *im, unsigned at)
{
	DIR           *d = opendir(im->nodes[at].path);
	struct dirent *e;
	unsigned       first = im->count;

	if (d == NULL)
		return fail(im, strerror(errno), im->nodes[at].path);
	while ((e = readdir(d)) != NULL) {
		node        n;
		struct stat st;
		size_t      length;

		if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
			continue;
		memset(&n, 0, sizeof n);
		length = strlen(im->nodes[at].path) + strlen(e->d_name) + 2u;
		if ((n.path = malloc(length)) == NULL) {
			closedir(d);
			return fail(im, "out of memory", NULL);
		}
		snprintf(n.path, length, "%s/%s", im->nodes[at].path, e->d_name);
		if (lstat(n.path, &st) != 0 || !(S_ISDIR(st.st_mode) || S_ISREG(st.st_mode))) {
			fail(im, "a disc holds directories and files only, not a link or a device", n.path);
			free(n.path);
			closedir(d);
			return -1;
		}
		n.dir    = S_ISDIR(st.st_mode);
		n.size   = n.dir ? 0u : (uint64_t)st.st_size;
		n.parent = at;
		n.depth  = im->nodes[at].depth + 1u;
		if (!identifier(e->d_name, n.dir, n.id)) {
			fail(im, "a name on a disc is DOS's 8.3, of letters, digits and _", n.path);
			free(n.path);
			closedir(d);
			return -1;
		}
		if (n.dir && n.depth > MAX_DEPTH) {
			fail(im, "a disc's directories are at most eight deep", n.path);
			free(n.path);
			closedir(d);
			return -1;
		}
		if (n.size > 0xffffffffu) {
			fail(im, "a file on a disc is under 4 GiB", n.path);
			free(n.path);
			closedir(d);
			return -1;
		}
		if (add(im, &n) != 0) {
			free(n.path);
			closedir(d);
			return fail(im, "out of memory", NULL);
		}
	}
	closedir(d);
	qsort(im->nodes + first, im->count - first, sizeof *im->nodes, by_id);
	for (unsigned i = first + 1u; i < im->count; i++)
		if (strcmp(im->nodes[i - 1u].id, im->nodes[i].id) == 0)
			return fail(im, "two names are one on a disc, which knows no case", im->nodes[i].path);
	im->nodes[at].first = first;
	im->nodes[at].count = im->count - first;
	return 0;
}

/* A directory record's length for `id` of `length` bytes: even. */
static uint32_t record_length(size_t length)
{
	return (uint32_t)(33u + length + ((33u + length) % 2u));
}

/* The bytes of `n`'s identifier as a record carries it. */
static size_t record_id(const node *n, char out[20])
{
	return (size_t)snprintf(out, 20, n->dir ? "%s" : "%s;1", n->id);
}

/* A directory record into `at` for extent `extent` of `size` bytes. */
static uint32_t put_record(uint8_t *at, uint32_t extent, uint32_t size, int dir, const char *id,
                           size_t id_length)
{
	static const uint8_t date[7] = { 80, 1, 1, 0, 0, 0, 0 };   /* 1980-01-01 */
	uint32_t             length = record_length(id_length);

	memset(at, 0, length);
	at[0] = (uint8_t)length;
	both32(at + 2, extent);
	both32(at + 10, size);
	memcpy(at + 18, date, sizeof date);
	at[25] = dir ? 2u : 0u;
	both16(at + 28, 1u);
	at[32] = (uint8_t)id_length;
	memcpy(at + 33, id, id_length);
	return length;
}

/* Directory `d`'s extent: its records packed, none across a sector. */
static uint32_t dir_bytes(const image *im, const node *d)
{
	uint32_t at = 2u * 34u;

	for (unsigned i = 0u; i < d->count; i++) {
		char     id[20];
		uint32_t length = record_length(record_id(&im->nodes[d->first + i], id));

		if (at % SECTOR + length > SECTOR)
			at += SECTOR - at % SECTOR;
		at += length;
	}
	return (at + SECTOR - 1u) / SECTOR * SECTOR;
}

static int put_sector(image *im)
{
	size_t at = 0u;

	while (at < SECTOR) {
		ssize_t n = write(im->fd, im->sector + at, SECTOR - at);

		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0)
			return fail(im, strerror(errno), "the image");
		at += (size_t)n;
	}
	memset(im->sector, 0, SECTOR);
	return 0;
}

/* The path table, little-endian or not, as sectors from here. */
static int put_path_table(image *im, uint32_t table_bytes, int big)
{
	uint32_t at = 0u;
	uint32_t written = 0u;

	for (unsigned i = 0u; i < im->count; i++) {
		const node *d = &im->nodes[i];
		size_t      length = i == 0u ? 1u : strlen(d->id);
		uint8_t     entry[8u + 8u + 1u];
		uint32_t    entry_length = (uint32_t)(8u + length + length % 2u);
		uint32_t    parent = im->nodes[d->parent].number;

		if (!d->dir)
			continue;
		memset(entry, 0, sizeof entry);
		entry[0] = (uint8_t)length;
		for (unsigned b = 0u; b < 4u; b++)
			entry[2u + b] = (uint8_t)(d->extent >> (8u * (big ? 3u - b : b)));
		entry[6] = (uint8_t)(parent >> (big ? 8u : 0u));
		entry[7] = (uint8_t)(parent >> (big ? 0u : 8u));
		if (i != 0u)
			memcpy(entry + 8, d->id, length);
		for (uint32_t b = 0u; b < entry_length; b++) {
			im->sector[at++] = entry[b];
			written++;
			if (at == SECTOR) {
				if (put_sector(im) != 0)
					return -1;
				at = 0u;
			}
		}
	}
	(void)table_bytes;
	return at != 0u || written == 0u ? put_sector(im) : 0;
}

static int put_file(image *im, const node *f)
{
	int      in = open(f->path, O_RDONLY | O_CLOEXEC);
	uint64_t left = f->size;

	if (in < 0)
		return fail(im, strerror(errno), f->path);
	while (left > 0u) {
		size_t  want = left < SECTOR ? (size_t)left : SECTOR;
		size_t  got = 0u;

		while (got < want) {
			ssize_t n = read(in, im->sector + got, want - got);

			if (n < 0 && errno == EINTR)
				continue;
			if (n <= 0) {
				close(in);
				return fail(im, "it changed while it was read", f->path);
			}
			got += (size_t)n;
		}
		left -= want;
		if (put_sector(im) != 0) {
			close(in);
			return -1;
		}
	}
	close(in);
	return 0;
}

int hull_iso_write(const char *root, const char *volume, int fd, char *detail, size_t cap)
{
	image    im;
	node     top;
	char     label[33];
	uint32_t table_bytes = 0u;
	uint32_t table_sectors;
	uint32_t next;
	unsigned numbered = 0u;
	int      result = -1;

	memset(&im, 0, sizeof im);
	im.detail     = detail;
	im.detail_cap = cap;
	im.fd         = fd;
	detail[0]     = '\0';
	if (strlen(volume) > 32u) {
		return fail(&im, "a disc's volume label is at most 32 characters", volume);
	}
	for (size_t i = 0u; volume[i] != '\0'; i++) {
		char up = (char)toupper((unsigned char)volume[i]);

		if (!((up >= 'A' && up <= 'Z') || (up >= '0' && up <= '9') || up == '_'))
			return fail(&im, "a disc's volume label is letters, digits and _", volume);
		label[i] = up;
	}
	memset(&top, 0, sizeof top);
	top.dir   = 1;
	top.depth = 1u;
	if ((top.path = strdup(root)) == NULL || add(&im, &top) != 0) {
		free(top.path);
		return fail(&im, "out of memory", NULL);
	}
	/* Breadth first, each directory's children sorted: the path table's
	 * order, and so each directory's number. */
	for (unsigned i = 0u; i < im.count; i++) {
		if (!im.nodes[i].dir)
			continue;
		im.nodes[i].number = ++numbered;
		if (read_dir(&im, i) != 0)
			goto done;
	}
	if (numbered > 0xffffu) {
		fail(&im, "a disc holds at most 65535 directories", NULL);
		goto done;
	}
	for (unsigned i = 0u; i < im.count; i++) {
		size_t length = i == 0u ? 1u : strlen(im.nodes[i].id);

		if (im.nodes[i].dir)
			table_bytes += (uint32_t)(8u + length + length % 2u);
	}
	table_sectors = (table_bytes + SECTOR - 1u) / SECTOR;
	/* The directories after the path tables, then the files. */
	next = 18u + 2u * table_sectors;
	for (unsigned i = 0u; i < im.count; i++) {
		if (im.nodes[i].dir) {
			im.nodes[i].extent = next;
			im.nodes[i].size   = dir_bytes(&im, &im.nodes[i]);
			next += (uint32_t)(im.nodes[i].size / SECTOR);
		}
	}
	for (unsigned i = 0u; i < im.count; i++) {
		if (!im.nodes[i].dir) {
			uint64_t sectors = (im.nodes[i].size + SECTOR - 1u) / SECTOR;

			if ((uint64_t)next + sectors > 0xffffffffu) {
				fail(&im, "the disc would pass 8 TiB", NULL);
				goto done;
			}
			im.nodes[i].extent = next;
			next += (uint32_t)sectors;
		}
	}

	for (unsigned i = 0u; i < 16u; i++)
		if (put_sector(&im) != 0)
			goto done;
	{
		/* The primary volume descriptor. */
		static const char date[] = "1980010100000000";
		uint8_t *p = im.sector;

		p[0] = 1u;
		memcpy(p + 1, "CD001", 5u);
		p[6] = 1u;
		memset(p + 8, ' ', 32u);
		memset(p + 40, ' ', 32u);
		memcpy(p + 40, label, strlen(volume));
		both32(p + 80, next);
		both16(p + 120, 1u);
		both16(p + 124, 1u);
		both16(p + 128, SECTOR);
		both32(p + 132, table_bytes);
		for (unsigned b = 0u; b < 4u; b++) {
			p[140u + b] = (uint8_t)(18u >> (8u * b));
			p[148u + b] = (uint8_t)((18u + table_sectors) >> (8u * (3u - b)));
		}
		put_record(p + 156, im.nodes[0].extent, (uint32_t)im.nodes[0].size, 1, "\0", 1u);
		memset(p + 190, ' ', 128u * 4u + 37u * 3u);
		for (unsigned d = 0u; d < 4u; d++)
			memcpy(p + 813u + 17u * d, d < 2u ? date : "0000000000000000", 16u);
		p[881] = 1u;
		if (put_sector(&im) != 0)
			goto done;
		p[0] = 255u;
		memcpy(p + 1, "CD001", 5u);
		p[6] = 1u;
		if (put_sector(&im) != 0)
			goto done;
	}
	if (put_path_table(&im, table_bytes, 0) != 0 || put_path_table(&im, table_bytes, 1) != 0)
		goto done;
	for (unsigned i = 0u; i < im.count; i++) {
		const node *d = &im.nodes[i];
		const node *up = &im.nodes[d->parent];
		uint32_t    at;
		uint32_t    written = 0u;

		if (!d->dir)
			continue;
		at  = put_record(im.sector, d->extent, (uint32_t)d->size, 1, "\0", 1u);
		at += put_record(im.sector + at, up->extent, (uint32_t)up->size, 1, "\1", 1u);
		for (unsigned k = 0u; k < d->count; k++) {
			const node *c = &im.nodes[d->first + k];
			char        id[20];
			size_t      length = record_id(c, id);
			uint32_t    record = record_length(length);

			if (at + record > SECTOR) {
				if (put_sector(&im) != 0)
					goto done;
				written += SECTOR;
				at = 0u;
			}
			at += put_record(im.sector + at, c->extent, (uint32_t)c->size, c->dir, id, length);
		}
		if (put_sector(&im) != 0)
			goto done;
		written += SECTOR;
		for (; written < d->size; written += SECTOR)
			if (put_sector(&im) != 0)
				goto done;
	}
	for (unsigned i = 0u; i < im.count; i++)
		if (!im.nodes[i].dir && put_file(&im, &im.nodes[i]) != 0)
			goto done;
	result = 0;
done:
	for (unsigned i = 0u; i < im.count; i++)
		free(im.nodes[i].path);
	free(im.nodes);
	return result;
}
