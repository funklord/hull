/*
 * media.c -- a disc as plain files and back (media.h, project.md section
 * 3.10). What is read here is somebody else's disc, so every offset in it
 * is checked against the image, a name becomes a path only if it is a
 * plain one, and what cannot be trusted is left for the diff to carry:
 * nothing about the image can make the result inexact, only larger.
 *
 * ISO9660's records are read by hand rather than by engine/disc.situ's
 * views, which hold the shape hull's own images have -- no extended
 * attributes, no interleave -- where a foreign disc may differ and is
 * still a disc.
 */

#define _GNU_SOURCE

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <ftw.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include "media.h"
#include "sha256.h"

#define SECTOR      2048u
#define DISK_UNIT   512u      /* a layout's sector on a disk, whatever its filesystem's */
#define MAX_DEPTH   32u
#define MAX_FILES   200000u
#define MAX_DIRS    50000u
#define COPY_BYTES  (1u << 20)

typedef struct placed {
	uint64_t sector;
	uint64_t size;
	uint64_t offset;   /* of this piece in its file, a fragmented one's */
	uint64_t total;    /* the file's size */
	char    *path;     /* under files/ */
} placed;

typedef struct walk {
	int      image;
	uint64_t bytes;
	char     root[4200];      /* DIR/files */
	placed  *files;
	size_t   count;
	size_t   cap;
	unsigned dirs;
	uint64_t dir_extents[MAX_DIRS];
	const char *extra;        /* more of media.conf, a raw dump's */
	unsigned unit;            /* the bytes a sector of the layout is */
} walk;

static int fail(char *detail, size_t cap, const char *what, const char *why)
{
	snprintf(detail, cap, "%s%s%s", what, why != NULL ? ": " : "", why != NULL ? why : "");
	return -1;
}

static uint32_t le32(const uint8_t *p)
{
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static int read_all(int fd, void *to, size_t n, uint64_t at)
{
	uint8_t *p = to;

	while (n > 0u) {
		ssize_t got = pread(fd, p, n, (off_t)at);

		if (got <= 0)
			return -1;
		p += got;
		at += (uint64_t)got;
		n -= (size_t)got;
	}
	return 0;
}

static int write_all(int fd, const void *from, size_t n, uint64_t at)
{
	const uint8_t *p = from;

	while (n > 0u) {
		ssize_t put = pwrite(fd, p, n, (off_t)at);

		if (put <= 0)
			return -1;
		p += put;
		at += (uint64_t)put;
		n -= (size_t)put;
	}
	return 0;
}

/* A path of plain names under files/, as a collection may hold it: nothing
 * that leaves the directory, whoever wrote layout.conf. */
static int plain_path(const char *path)
{
	const char *name = path;

	if (path[0] == '\0' || path[0] == '/')
		return 0;
	for (;;) {
		const char *end = strchr(name, '/');
		size_t      n = end != NULL ? (size_t)(end - name) : strlen(name);

		if (n == 0u || (n == 1u && name[0] == '.') ||
		    (n == 2u && name[0] == '.' && name[1] == '.'))
			return 0;
		if (end == NULL)
			return 1;
		name = end + 1;
	}
}

/* An identifier off the disc as a file's name: its `;1` and a bare trailing
 * dot gone, into `out`. 0 where it is not a plain name, whose bytes the
 * diff then carries. */
static int disc_name(const uint8_t *id, size_t length, char *out, size_t cap)
{
	size_t n = 0u;

	while (n < length && id[n] != ';')
		n++;
	if (n > 0u && id[n - 1u] == '.')
		n--;
	if (n == 0u || n >= cap)
		return 0;
	for (size_t i = 0u; i < n; i++) {
		char c = (char)id[i];

		if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
		      c == '_' || c == '-' || c == '.' || c == '~' || c == '$' || c == '!'))
			return 0;
		out[i] = c;
	}
	out[n] = '\0';
	return strcmp(out, ".") != 0 && strcmp(out, "..") != 0;
}

/* The file at `sector` of `size` bytes copied out to `path`. */
static int copy_out(walk *w, uint64_t sector, uint64_t size, const char *path)
{
	static uint8_t buffer[COPY_BYTES];
	int            fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0644);
	uint64_t       done = 0u;

	if (fd < 0)
		return -1;
	while (done < size) {
		size_t n = size - done < sizeof buffer ? (size_t)(size - done) : sizeof buffer;

		if (read_all(w->image, buffer, n, sector * SECTOR + done) != 0 ||
		    write_all(fd, buffer, n, done) != 0) {
			close(fd);
			unlink(path);
			return -1;
		}
		done += n;
	}
	return close(fd);
}

static int place_piece(walk *w, uint64_t sector, uint64_t size, uint64_t offset, uint64_t total,
                       const char *relative)
{
	if (w->count == w->cap) {
		size_t  cap = w->cap != 0u ? w->cap * 2u : 256u;
		placed *grown = realloc(w->files, cap * sizeof *grown);

		if (grown == NULL)
			return -1;
		w->files = grown;
		w->cap   = cap;
	}
	if ((w->files[w->count].path = strdup(relative)) == NULL)
		return -1;
	w->files[w->count].sector = sector;
	w->files[w->count].size   = size;
	w->files[w->count].offset = offset;
	w->files[w->count].total  = total;
	w->count++;
	return 0;
}

static int place(walk *w, uint64_t sector, uint64_t size, const char *relative)
{
	return place_piece(w, sector, size, 0u, size, relative);
}

/* The directory of `size` bytes at `sector`, `relative` its path under
 * files/ ("" for the root). A record that does not make sense is passed
 * over and left to the diff. -1 only where this host fails. */
static int read_directory(walk *w, uint64_t sector, uint64_t size, const char *relative,
                          unsigned depth)
{
	uint8_t *bytes;
	int      status = 0;

	if (depth > MAX_DEPTH || w->dirs == MAX_DIRS || size == 0u || size > (64u << 20) ||
	    sector * SECTOR + size > w->bytes)
		return 0;
	/* A directory that names one above it would be walked for ever. */
	for (unsigned i = 0u; i < w->dirs; i++)
		if (w->dir_extents[i] == sector)
			return 0;
	w->dir_extents[w->dirs++] = sector;
	if ((bytes = malloc((size_t)size)) == NULL)
		return -1;
	if (read_all(w->image, bytes, (size_t)size, sector * SECTOR) != 0) {
		free(bytes);
		return -1;
	}
	for (uint64_t at = 0u; at < size && status == 0;) {
		const uint8_t *record = bytes + at;
		size_t         length = record[0];
		uint64_t       extent;
		uint64_t       bytes_of;
		size_t         id_length;
		unsigned       flags;
		char           name[256];
		char           next[4096];
		char           path[8400];

		/* A record does not cross a sector: zero to the sector's end. */
		if (length == 0u) {
			at = (at / SECTOR + 1u) * SECTOR;
			continue;
		}
		if (length < 34u || at + length > size)
			break;
		at += length;
		id_length = record[32];
		if (33u + id_length > length)
			continue;
		extent   = (uint64_t)le32(record + 2) + record[1];
		bytes_of = le32(record + 10);
		flags    = record[25];
		if (id_length == 1u && record[33] <= 1u)
			continue;   /* . and .. */
		/* Interleaved or in several extents: not one run of bytes. */
		if ((flags & 0x80u) != 0u || record[26] != 0u || record[27] != 0u)
			continue;
		if (!disc_name(record + 33, id_length, name, sizeof name) ||
		    (size_t)snprintf(next, sizeof next, "%s%s%s", relative, relative[0] != '\0' ? "/" : "",
		                     name) >= sizeof next)
			continue;
		snprintf(path, sizeof path, "%s/%s", w->root, next);
		if ((flags & 0x02u) != 0u) {
			if (mkdir(path, 0755) == 0)
				status = read_directory(w, extent, bytes_of, next, depth + 1u);
			continue;
		}
		if (w->count >= MAX_FILES || extent * SECTOR + bytes_of > w->bytes)
			continue;
		if (copy_out(w, extent, bytes_of, path) == 0)
			status = place(w, extent, bytes_of, next);
	}
	free(bytes);
	return status;
}

/* The primary volume descriptor's tree into w->files, its label into
 * `volume`. An image with none has no files, and that is not a failure. */
static int read_iso(walk *w, char *volume, size_t cap)
{
	uint8_t pvd[SECTOR];
	size_t  n = 32u;

	if (w->bytes < 17u * SECTOR || read_all(w->image, pvd, sizeof pvd, 16u * SECTOR) != 0 ||
	    pvd[0] != 1u || memcmp(pvd + 1, "CD001", 5u) != 0)
		return 0;
	while (n > 0u && pvd[40u + n - 1u] == ' ')
		n--;
	for (size_t i = 0u; i < n && i + 1u < cap; i++) {
		char c = (char)pvd[40u + i];

		/* media.conf's own punctuation kept out of it: a label of
		 * `(bytes ` read as that field, which the fuzzer found. What the
		 * label was is in the patch. */
		volume[i]     = c >= ' ' && c <= '~' && strchr("\"\\()", c) == NULL ? c : '_';
		volume[i + 1] = '\0';
	}
	return read_directory(w, (uint64_t)le32(pvd + 156 + 2) + pvd[156 + 1], le32(pvd + 156 + 10),
	                      "", 0u);
}

/* --- FAT ------------------------------------------------------------------ */

/*
 * A floppy, or a volume with no partition table: what the boot sector's
 * parameters say, each checked, then the directories from the root.
 * Cluster chains may go anywhere, so a file is a piece a run of adjacent
 * clusters; a chain that loops, ends early or leaves the disk is not a
 * file, and its bytes are the diff's. Long names are the diff's too, the
 * short name being the one DOS software asks for.
 */
typedef struct fat {
	uint64_t bytes_per_sector;
	uint64_t sectors_per_cluster;
	uint64_t first_data;      /* in sectors, from the disk's start */
	uint64_t clusters;
	unsigned bits;            /* 12, 16 or 32 */
	uint8_t *table;
	size_t   table_bytes;
} fat;

static uint16_t le16(const uint8_t *p)
{
	return (uint16_t)(p[0] | p[1] << 8);
}

/* The cluster after `cluster`, or 0 where the chain ends or is not one. */
static uint32_t fat_next(const fat *f, uint32_t cluster)
{
	uint64_t at;
	uint32_t next;

	if (f->bits == 12u) {
		at = (uint64_t)cluster * 3u / 2u;
		if (at + 2u > f->table_bytes)
			return 0u;
		next = le16(f->table + at);
		next = (cluster & 1u) != 0u ? next >> 4 : next & 0xFFFu;
		return next >= 0xFF7u ? 0u : next;
	}
	if (f->bits == 16u) {
		at = (uint64_t)cluster * 2u;
		if (at + 2u > f->table_bytes)
			return 0u;
		next = le16(f->table + at);
		return next >= 0xFFF7u ? 0u : next;
	}
	at = (uint64_t)cluster * 4u;
	if (at + 4u > f->table_bytes)
		return 0u;
	next = le32(f->table + at) & 0x0FFFFFFFu;
	return next >= 0x0FFFFFF7u ? 0u : next;
}

static int fat_valid(const fat *f, uint32_t cluster)
{
	return cluster >= 2u && cluster - 2u < f->clusters;
}

static uint64_t fat_sector(const fat *f, uint32_t cluster)
{
	return f->first_data + (uint64_t)(cluster - 2u) * f->sectors_per_cluster;
}

/* The directory of the chain from `cluster` into a buffer of its own,
 * its length in `*length`; NULL where it is no chain. */
static uint8_t *fat_chain(walk *w, const fat *f, uint32_t cluster, size_t *length)
{
	uint64_t bytes = f->sectors_per_cluster * f->bytes_per_sector;
	size_t   used = 0u;
	uint8_t *all = NULL;

	for (uint64_t steps = 0u; fat_valid(f, cluster); cluster = fat_next(f, cluster)) {
		uint8_t *grown;

		if (steps++ > f->clusters || used + bytes > (16u << 20) ||
		    (grown = realloc(all, used + (size_t)bytes)) == NULL) {
			free(all);
			return NULL;
		}
		all = grown;
		if (read_all(w->image, all + used, (size_t)bytes,
		             fat_sector(f, cluster) * f->bytes_per_sector) != 0) {
			free(all);
			return NULL;
		}
		used += (size_t)bytes;
	}
	*length = used;
	return all;
}

/* The file of `size` bytes from `cluster` to `path`, each run of adjacent
 * clusters a piece. Nothing is left where its chain is not whole. */
static int fat_file(walk *w, const fat *f, uint32_t cluster, uint64_t size, const char *path,
                    const char *relative)
{
	static uint8_t buffer[COPY_BYTES];
	uint64_t       cluster_bytes = f->sectors_per_cluster * f->bytes_per_sector;
	size_t         first = w->count;
	uint64_t       done = 0u;
	uint64_t       steps = 0u;
	int            fd;

	if (size == 0u)
		return close(open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644)) == 0 ?
		       place(w, 0u, 0u, relative) : 0;
	/* The chain walked first: all of it, or none of it is a file. */
	for (uint32_t c = cluster; done < size; c = fat_next(f, c)) {
		if (!fat_valid(f, c) || steps++ > f->clusters)
			return 0;
		done += cluster_bytes;
	}
	if ((fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0644)) < 0)
		return 0;
	done = 0u;
	while (done < size) {
		uint32_t start = cluster;
		uint64_t run = cluster_bytes;
		uint32_t next = fat_next(f, cluster);
		uint64_t piece;

		while (done + run < size && next == cluster + 1u) {
			cluster = next;
			next = fat_next(f, cluster);
			run += cluster_bytes;
		}
		piece = run < size - done ? run : size - done;
		if (place_piece(w, fat_sector(f, start) * f->bytes_per_sector / DISK_UNIT, piece, done, size,
		                relative) != 0) {
			close(fd);
			return -1;
		}
		for (uint64_t at = 0u; at < piece;) {
			size_t n = piece - at < sizeof buffer ? (size_t)(piece - at) : sizeof buffer;

			if (read_all(w->image, buffer, n, fat_sector(f, start) * f->bytes_per_sector + at) != 0 ||
			    write_all(fd, buffer, n, done + at) != 0) {
				close(fd);
				unlink(path);
				for (size_t i = first; i < w->count; i++)
					free(w->files[i].path);
				w->count = first;
				return 0;
			}
			at += n;
		}
		done += piece;
		cluster = next;
	}
	return close(fd);
}

static int fat_directory(walk *w, const fat *f, const uint8_t *entries, size_t length,
                         const char *relative, unsigned depth, char *volume, size_t volume_cap)
{
	int status = 0;

	for (size_t at = 0u; at + 32u <= length && status == 0; at += 32u) {
		const uint8_t *e = entries + at;
		unsigned       attributes = e[11];
		uint32_t       cluster = le16(e + 26);
		uint64_t       size = le32(e + 28);
		uint8_t        id[12];
		size_t         n = 0u;
		char           name[16];
		char           next[4096];
		char           path[8400];

		if (e[0] == 0u)
			break;
		if (e[0] == 0xE5u || (attributes & 0x0Fu) == 0x0Fu)
			continue;
		if (f->bits == 32u)
			cluster |= (uint32_t)le16(e + 20) << 16;
		if ((attributes & 0x08u) != 0u) {
			if (depth == 0u && volume[0] == '\0') {
				size_t l = 11u;

				while (l > 0u && e[l - 1u] == ' ')
					l--;
				for (size_t i = 0u; i < l && i + 1u < volume_cap; i++) {
					char c = (char)e[i];

					volume[i]     = c > ' ' && c <= '~' && strchr("\"\\()", c) == NULL ? c : '_';
					volume[i + 1] = '\0';
				}
			}
			continue;
		}
		for (size_t i = 0u; i < 8u && e[i] != ' '; i++)
			id[n++] = e[i];
		if (e[8] != ' ')
			id[n++] = '.';
		for (size_t i = 8u; i < 11u && e[i] != ' '; i++)
			id[n++] = e[i];
		if (!disc_name(id, n, name, sizeof name) ||
		    (size_t)snprintf(next, sizeof next, "%s%s%s", relative, relative[0] != '\0' ? "/" : "",
		                     name) >= sizeof next)
			continue;
		snprintf(path, sizeof path, "%s/%s", w->root, next);
		if ((attributes & 0x10u) != 0u) {
			size_t   sub_length = 0u;
			uint8_t *sub;
			unsigned seen = 0u;

			if (depth >= MAX_DEPTH || w->dirs == MAX_DIRS || !fat_valid(f, cluster))
				continue;
			for (unsigned i = 0u; i < w->dirs; i++)
				seen |= w->dir_extents[i] == cluster;
			if (seen || mkdir(path, 0755) != 0)
				continue;
			w->dir_extents[w->dirs++] = cluster;
			if ((sub = fat_chain(w, f, cluster, &sub_length)) != NULL) {
				status = fat_directory(w, f, sub, sub_length, next, depth + 1u, volume, volume_cap);
				free(sub);
			}
			continue;
		}
		if (w->count < MAX_FILES)
			status = fat_file(w, f, cluster, size, path, next);
	}
	return status;
}

/* A FAT volume's tree into w->files, the volume `base` bytes into the
 * image and its files under `prefix` of files/. Sets *found where it was
 * one; nothing is done where it is not. */
static int read_fat(walk *w, uint64_t base, const char *prefix, int *found, char *volume, size_t cap)
{
	uint8_t  boot[512];
	fat      f;
	uint64_t reserved;
	uint64_t fats;
	uint64_t root_entries;
	uint64_t total;
	uint64_t table_sectors;
	uint64_t root_sectors;
	int      status = 0;

	memset(&f, 0, sizeof f);
	if (w->bytes < base + sizeof boot || read_all(w->image, boot, sizeof boot, base) != 0)
		return 0;
	f.bytes_per_sector    = le16(boot + 11);
	f.sectors_per_cluster = boot[13];
	reserved              = le16(boot + 14);
	fats                  = boot[16];
	root_entries          = le16(boot + 17);
	total                 = le16(boot + 19) != 0u ? le16(boot + 19) : le32(boot + 32);
	table_sectors         = le16(boot + 22) != 0u ? le16(boot + 22) : le32(boot + 36);
	if ((f.bytes_per_sector != 512u && f.bytes_per_sector != 1024u && f.bytes_per_sector != 2048u &&
	     f.bytes_per_sector != 4096u) ||
	    f.sectors_per_cluster == 0u || (f.sectors_per_cluster & (f.sectors_per_cluster - 1u)) != 0u ||
	    reserved == 0u || fats == 0u || fats > 2u || boot[21] < 0xF0u || total == 0u ||
	    table_sectors == 0u || base % f.bytes_per_sector != 0u ||
	    base + total * f.bytes_per_sector > w->bytes)
		return 0;
	root_sectors = (root_entries * 32u + f.bytes_per_sector - 1u) / f.bytes_per_sector;
	f.first_data = reserved + fats * table_sectors + root_sectors;
	if (f.first_data >= total)
		return 0;
	f.clusters = (total - f.first_data) / f.sectors_per_cluster;
	f.first_data += base / f.bytes_per_sector;
	f.bits = f.clusters < 4085u ? 12u : f.clusters < 65525u ? 16u : 32u;
	if ((f.bits == 32u) != (root_entries == 0u) ||
	    (f.table_bytes = (size_t)(table_sectors * f.bytes_per_sector)) > (256u << 20) ||
	    (f.table = malloc(f.table_bytes)) == NULL ||
	    read_all(w->image, f.table, f.table_bytes, base + reserved * f.bytes_per_sector) != 0) {
		free(f.table);
		return 0;
	}
	w->unit = DISK_UNIT;
	*found = 1;
	/* Each volume's clusters are its own. */
	w->dirs = 0u;
	if (f.bits == 32u) {
		size_t   length = 0u;
		uint8_t *root = fat_chain(w, &f, le32(boot + 44), &length);

		if (root != NULL)
			status = fat_directory(w, &f, root, length, prefix, 0u, volume, cap);
		free(root);
	} else {
		size_t   length = (size_t)root_entries * 32u;
		uint8_t *root = malloc(length);

		if (root != NULL && read_all(w->image, root, length, base + (reserved + fats * table_sectors) *
		                             f.bytes_per_sector) == 0)
			status = fat_directory(w, &f, root, length, prefix, 0u, volume, cap);
		free(root);
	}
	free(f.table);
	return status;
}

/* --- ext2, ext3, ext4 --------------------------------------------------------- */

/*
 * A Linux filesystem's tree, read-only: a file's blocks found by its
 * extents or its indirect blocks, each run a piece, as FAT's clusters. Every
 * block number is checked against the filesystem, a directory is walked
 * once, and what cannot be read is the diff's. Regular files only: a link
 * or a device has no bytes of its own for a layout to place, and is in the
 * patch with the inode it is. Owners and modes likewise, so `files/` is what
 * the filesystem holds and not yet how it holds it.
 */
#define EXT_MAX_RUNS  65536u
#define EXT_MAX_DIR   (16u << 20)

typedef struct ext_run {
	uint64_t logical;     /* in the file, in blocks */
	uint64_t physical;
	uint64_t count;
} ext_run;

typedef struct ext {
	uint64_t base;
	uint64_t block;       /* its size */
	uint64_t blocks;
	uint64_t inodes;
	uint64_t inodes_per_group;
	uint64_t inode_size;
	uint64_t descriptors;  /* where the group descriptors are, in bytes */
	uint64_t descriptor_size;
	uint64_t groups;
	int      wide;         /* 64bit */
} ext;

static int ext_read_block(walk *w, const ext *e, uint64_t number, uint8_t *to)
{
	if (number == 0u || number >= e->blocks)
		return -1;
	return read_all(w->image, to, (size_t)e->block, e->base + number * e->block);
}

static int ext_add(ext_run *runs, size_t *count, uint64_t logical, uint64_t physical, uint64_t n,
                   const ext *e)
{
	if (n == 0u || physical == 0u || physical >= e->blocks || n > e->blocks - physical ||
	    *count == EXT_MAX_RUNS)
		return -1;
	if (*count > 0u) {
		ext_run *last = &runs[*count - 1u];

		if (last->logical + last->count == logical && last->physical + last->count == physical) {
			last->count += n;
			return 0;
		}
	}
	runs[*count].logical  = logical;
	runs[*count].physical = physical;
	runs[*count].count    = n;
	(*count)++;
	return 0;
}

/* An extent tree's node, `depth` levels at most above its leaves. */
static int ext_extents(walk *w, const ext *e, const uint8_t *node, size_t bytes, unsigned depth,
                       ext_run *runs, size_t *count)
{
	unsigned entries;
	unsigned level;

	if (bytes < 12u || le16(node) != 0xF30Au)
		return -1;
	entries = le16(node + 2);
	level   = le16(node + 6);
	if (level > depth || 12u + (size_t)entries * 12u > bytes)
		return -1;
	for (unsigned i = 0u; i < entries; i++) {
		const uint8_t *entry = node + 12u + (size_t)i * 12u;

		if (level == 0u) {
			unsigned length = le16(entry + 4);
			uint64_t start = (uint64_t)le16(entry + 6) << 32 | le32(entry + 8);

			/* Past 32768, allocated and not written: it reads as zeros,
			 * a hole, the diff's only if the disk says otherwise. */
			if (length > 32768u)
				continue;
			if (ext_add(runs, count, le32(entry), start, length, e) != 0)
				return -1;
		} else {
			uint64_t child = (uint64_t)le16(entry + 8) << 32 | le32(entry + 4);
			uint8_t *next = malloc((size_t)e->block);
			int      status;

			if (next == NULL)
				return -1;
			status = ext_read_block(w, e, child, next) != 0 ? -1 :
			         ext_extents(w, e, next, (size_t)e->block, level - 1u, runs, count);
			free(next);
			if (status != 0)
				return -1;
		}
	}
	return 0;
}

/* An indirect block's pointers, `levels` above the data, from `logical`. */
static int ext_indirect(walk *w, const ext *e, uint64_t number, unsigned levels, uint64_t *logical,
                        uint64_t limit, ext_run *runs, size_t *count)
{
	uint64_t per = e->block / 4u;
	uint8_t *block = malloc((size_t)e->block);
	int      status = 0;

	if (block == NULL)
		return -1;
	if (number == 0u) {
		/* A hole: its span passed over. */
		uint64_t span = 1u;

		for (unsigned i = 0u; i < levels; i++)
			span *= per;
		*logical += span;
		free(block);
		return 0;
	}
	if (ext_read_block(w, e, number, block) != 0) {
		free(block);
		return -1;
	}
	for (uint64_t i = 0u; i < per && *logical < limit && status == 0; i++) {
		uint64_t next = le32(block + 4u * i);

		if (levels > 1u) {
			status = ext_indirect(w, e, next, levels - 1u, logical, limit, runs, count);
		} else {
			if (next != 0u)
				status = ext_add(runs, count, *logical, next, 1u, e);
			(*logical)++;
		}
	}
	free(block);
	return status;
}

/* The inode `number` into `inode`, 128 bytes of it. */
static int ext_inode(walk *w, const ext *e, uint64_t number, uint8_t inode[128])
{
	uint8_t  descriptor[64];
	uint64_t group;
	uint64_t index;
	uint64_t table;

	if (number == 0u || number > e->inodes)
		return -1;
	group = (number - 1u) / e->inodes_per_group;
	index = (number - 1u) % e->inodes_per_group;
	if (group >= e->groups ||
	    read_all(w->image, descriptor, (size_t)(e->descriptor_size < 64u ? e->descriptor_size : 64u),
	             e->base + e->descriptors + group * e->descriptor_size) != 0)
		return -1;
	table = le32(descriptor + 8);
	if (e->wide && e->descriptor_size >= 64u)
		table |= (uint64_t)le32(descriptor + 40) << 32;
	if (table == 0u || table >= e->blocks)
		return -1;
	return read_all(w->image, inode, 128u, e->base + table * e->block + index * e->inode_size);
}

/* The inode's blocks as runs, its size in *size. -1 where they are not to
 * be trusted, or its data sits in the inode, which is the diff's. */
static int ext_runs(walk *w, const ext *e, const uint8_t inode[128], uint64_t *size, ext_run *runs,
                    size_t *count)
{
	uint32_t flags = le32(inode + 32);

	*size  = (uint64_t)le32(inode + 108) << 32 | le32(inode + 4);
	*count = 0u;
	if ((flags & 0x10000000u) != 0u)
		return -1;
	if ((flags & 0x80000u) != 0u)
		return ext_extents(w, e, inode + 40, 60u, 5u, runs, count);
	{
		uint64_t blocks = (*size + e->block - 1u) / e->block;
		uint64_t logical = 0u;

		for (unsigned i = 0u; i < 12u && logical < blocks; i++, logical++) {
			uint64_t number = le32(inode + 40 + 4u * i);

			if (number != 0u && ext_add(runs, count, logical, number, 1u, e) != 0)
				return -1;
		}
		for (unsigned level = 1u; level <= 3u && logical < blocks; level++)
			if (ext_indirect(w, e, le32(inode + 40 + 48 + 4u * (level - 1u)), level, &logical,
			                 blocks, runs, count) != 0)
				return -1;
	}
	return 0;
}

/* A filesystem's name for a file as a path of one: anything but what a
 * layout line or a path cannot hold. */
static int ext_name(const uint8_t *id, size_t length, char *out, size_t cap)
{
	if (length == 0u || length >= cap || id[0] == '@')
		return 0;
	for (size_t i = 0u; i < length; i++) {
		if (id[i] < 0x20u || id[i] == '/' || id[i] == 0x7Fu)
			return 0;
		out[i] = (char)id[i];
	}
	out[length] = '\0';
	return strcmp(out, ".") != 0 && strcmp(out, "..") != 0;
}

/* The regular file of the inode to `path`, a piece a run. */
static int ext_file(walk *w, const ext *e, const uint8_t inode[128], ext_run *runs,
                    const char *path, const char *relative)
{
	static uint8_t buffer[COPY_BYTES];
	size_t         count;
	uint64_t       size;
	size_t         first = w->count;
	int            fd;

	/* Past the image's size it is a hole or a lie, and either way not
	 * something to make on this disk. */
	if (ext_runs(w, e, inode, &size, runs, &count) != 0 || size > w->bytes)
		return 0;
	if ((fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0644)) < 0)
		return 0;
	if (ftruncate(fd, (off_t)size) != 0) {
		close(fd);
		unlink(path);
		return 0;
	}
	if (size == 0u) {
		close(fd);
		return place(w, 0u, 0u, relative);
	}
	for (size_t i = 0u; i < count; i++) {
		uint64_t offset = runs[i].logical * e->block;
		uint64_t at = e->base + runs[i].physical * e->block;
		uint64_t piece = runs[i].count * e->block;

		if (offset >= size)
			break;
		if (piece > size - offset)
			piece = size - offset;
		if (place_piece(w, at / DISK_UNIT, piece, offset, size, relative) != 0) {
			close(fd);
			return -1;
		}
		for (uint64_t done = 0u; done < piece;) {
			size_t n = piece - done < sizeof buffer ? (size_t)(piece - done) : sizeof buffer;

			if (read_all(w->image, buffer, n, at + done) != 0 ||
			    write_all(fd, buffer, n, offset + done) != 0) {
				close(fd);
				unlink(path);
				for (size_t k = first; k < w->count; k++)
					free(w->files[k].path);
				w->count = first;
				return 0;
			}
			done += n;
		}
	}
	return close(fd);
}

static int ext_directory(walk *w, const ext *e, uint64_t number, const char *relative,
                         unsigned depth, ext_run *runs)
{
	uint8_t  inode[128];
	uint64_t size;
	size_t   count;
	uint8_t *bytes;
	int      status = 0;

	if (depth > MAX_DEPTH || w->dirs == MAX_DIRS || ext_inode(w, e, number, inode) != 0 ||
	    (le16(inode) & 0xF000u) != 0x4000u || ext_runs(w, e, inode, &size, runs, &count) != 0 ||
	    size > EXT_MAX_DIR)
		return 0;
	for (unsigned i = 0u; i < w->dirs; i++)
		if (w->dir_extents[i] == number)
			return 0;
	w->dir_extents[w->dirs++] = number;
	if ((bytes = calloc(1u, (size_t)size + 1u)) == NULL)
		return -1;
	for (size_t i = 0u; i < count; i++) {
		uint64_t offset = runs[i].logical * e->block;
		uint64_t n = runs[i].count * e->block;

		if (offset >= size)
			continue;
		if (n > size - offset)
			n = size - offset;
		if (read_all(w->image, bytes + offset, (size_t)n, e->base + runs[i].physical * e->block) != 0) {
			free(bytes);
			return 0;
		}
	}
	for (uint64_t at = 0u; at + 8u <= size && status == 0;) {
		uint64_t entry = le32(bytes + at);
		unsigned length = le16(bytes + at + 4);
		size_t   name_length = bytes[at + 6];
		uint8_t  child[128];
		char     name[256];
		char     next[4096];
		char     path[8400];

		if (length < 8u || (length & 3u) != 0u || at + length > size)
			break;
		if (entry != 0u && 8u + name_length <= length &&
		    ext_name(bytes + at + 8, name_length, name, sizeof name) &&
		    (size_t)snprintf(next, sizeof next, "%s%s%s", relative, relative[0] != '\0' ? "/" : "",
		                     name) < sizeof next &&
		    ext_inode(w, e, entry, child) == 0) {
			unsigned kind = le16(child) & 0xF000u;

			snprintf(path, sizeof path, "%s/%s", w->root, next);
			if (kind == 0x4000u) {
				if (mkdir(path, 0755) == 0)
					status = ext_directory(w, e, entry, next, depth + 1u, runs);
			} else if (kind == 0x8000u && w->count < MAX_FILES) {
				status = ext_file(w, e, child, runs, path, next);
			}
		}
		at += length;
	}
	free(bytes);
	return status;
}

/* A filesystem's tree, as read_fat. */
static int read_ext(walk *w, uint64_t base, const char *prefix, int *found, char *volume, size_t cap)
{
	uint8_t  super[1024];
	ext      e;
	uint64_t per_group;
	ext_run *runs;
	int      status;

	memset(&e, 0, sizeof e);
	if (w->bytes < base + 2048u || read_all(w->image, super, sizeof super, base + 1024u) != 0 ||
	    le16(super + 56) != 0xEF53u || le32(super + 24) > 6u)
		return 0;
	e.base             = base;
	e.block            = 1024u << le32(super + 24);
	e.wide             = (le32(super + 96) & 0x80u) != 0u;
	e.blocks           = le32(super + 4) | (e.wide ? (uint64_t)le32(super + 0x150) << 32 : 0u);
	e.inodes           = le32(super);
	e.inodes_per_group = le32(super + 40);
	e.inode_size       = le32(super + 76) >= 1u ? le16(super + 88) : 128u;
	e.descriptor_size  = e.wide && le16(super + 254) >= 32u ? le16(super + 254) : 32u;
	per_group          = le32(super + 32);
	if (e.inodes_per_group == 0u || per_group == 0u || e.inode_size < 128u || e.blocks == 0u ||
	    base % DISK_UNIT != 0u || e.blocks > (w->bytes - base) / e.block)
		return 0;
	e.groups      = (e.blocks + per_group - 1u) / per_group;
	e.descriptors = ((uint64_t)le32(super + 20) + 1u) * e.block;
	if ((runs = malloc(EXT_MAX_RUNS * sizeof *runs)) == NULL)
		return -1;
	w->unit = DISK_UNIT;
	*found = 1;
	w->dirs = 0u;
	if (volume[0] == '\0')
		for (size_t i = 0u; i < 16u && super[120 + i] != 0u && i + 1u < cap; i++) {
			char c = (char)super[120 + i];

			volume[i]     = c >= ' ' && c <= '~' && strchr("\"\\()", c) == NULL ? c : '_';
			volume[i + 1] = '\0';
		}
	status = ext_directory(w, &e, 2u, prefix, 0u, runs);
	free(runs);
	return status;
}

/* --- partitions ------------------------------------------------------------ */

#define MAX_PARTITIONS 128u

/* The first sector of each partition a table in the image names, GPT's or
 * failing that MBR's, into `first`, each checked against the image; how
 * many. Their sectors are 512 bytes, as a table's are on these disks. */
static unsigned read_table(walk *w, uint64_t first[MAX_PARTITIONS])
{
	uint8_t  sector[512];
	unsigned count = 0u;

	if (w->bytes < 1024u || read_all(w->image, sector, sizeof sector, 0u) != 0 ||
	    sector[510] != 0x55u || sector[511] != 0xAAu)
		return 0u;
	if (sector[446 + 4] == 0xEEu) {
		uint8_t  header[512];
		uint64_t entries;
		uint32_t number;
		uint32_t size;

		if (read_all(w->image, header, sizeof header, 512u) != 0 ||
		    memcmp(header, "EFI PART", 8u) != 0)
			return 0u;
		entries = (uint64_t)le32(header + 72) | (uint64_t)le32(header + 76) << 32;
		number = le32(header + 80);
		size = le32(header + 84);
		if (size < 128u || size > 4096u || number > 4096u ||
		    entries > w->bytes / 512u || entries * 512u + (uint64_t)number * size > w->bytes)
			return 0u;
		for (uint32_t i = 0u; i < number && count < MAX_PARTITIONS; i++) {
			uint8_t  entry[128];
			uint64_t start;
			static const uint8_t none[16];

			if (read_all(w->image, entry, sizeof entry, entries * 512u + (uint64_t)i * size) != 0)
				break;
			if (memcmp(entry, none, sizeof none) == 0)
				continue;
			start = (uint64_t)le32(entry + 32) | (uint64_t)le32(entry + 36) << 32;
			if (start != 0u && start < w->bytes / 512u)
				first[count++] = start;
		}
		return count;
	}
	for (unsigned i = 0u; i < 4u; i++) {
		const uint8_t *entry = sector + 446 + 16u * i;
		uint64_t       start = le32(entry + 8);

		if (entry[4] != 0u && start != 0u && start < w->bytes / 512u)
			first[count++] = start;
	}
	return count;
}

/* Each FAT partition's tree, under pN of files/, N its place in the table.
 * What is in a partition this cannot read is the diff's, like the table. */
static int read_partitions(walk *w, char *volume, size_t cap)
{
	uint64_t first[MAX_PARTITIONS];
	unsigned count = read_table(w, first);

	for (unsigned i = 0u; i < count; i++) {
		char prefix[16];
		char path[4300];
		int  found = 0;

		snprintf(prefix, sizeof prefix, "p%u", i + 1u);
		snprintf(path, sizeof path, "%s/%s", w->root, prefix);
		if (mkdir(path, 0755) != 0)
			return -1;
		if (read_fat(w, first[i] * 512u, prefix, &found, volume, cap) != 0 ||
		    (!found && read_ext(w, first[i] * 512u, prefix, &found, volume, cap) != 0))
			return -1;
		if (!found)
			rmdir(path);
	}
	return 0;
}

/* --- reassembly ----------------------------------------------------------- */

/* media.conf's bytes and sha256, by the text this file wrote and any
 * readable form as near it as a hand might: (bytes N), (sha256 "H"). */
static int read_conf(const char *dir, uint64_t *bytes, char sha[65], unsigned *unit)
{
	char    path[4300];
	static char text[65536];
	int     fd;
	ssize_t got;
	const char *at;

	snprintf(path, sizeof path, "%s/media.conf", dir);
	if ((fd = open(path, O_RDONLY | O_CLOEXEC)) < 0)
		return -1;
	got = read(fd, text, sizeof text - 1u);
	close(fd);
	if (got <= 0)
		return -1;
	text[got] = '\0';
	if ((at = strstr(text, "(bytes ")) == NULL || sscanf(at, "(bytes %" SCNu64 ")", bytes) != 1)
		return -1;
	if ((at = strstr(text, "(sha256 \"")) == NULL || sscanf(at, "(sha256 \"%64[0-9a-f]\")", sha) != 1)
		return -1;
	*unit = SECTOR;
	if ((at = strstr(text, "(unit ")) != NULL &&
	    (sscanf(at, "(unit %u)", unit) != 1 || *unit == 0u || *unit > SECTOR * 2u))
		return -1;
	return strlen(sha) == 64u ? 0 : -1;
}

/* layout.conf's files placed on `fd`, already `bytes` long and empty. */
static int place_files(const char *dir, int fd, uint64_t bytes, unsigned unit, char *detail,
                       size_t cap)
{
	static uint8_t buffer[COPY_BYTES];
	char           path[4300];
	char           line[4400];
	FILE          *layout;
	int            status = 0;

	snprintf(path, sizeof path, "%s/layout.conf", dir);
	if ((layout = fopen(path, "re")) == NULL)
		return fail(detail, cap, "the collection's layout.conf cannot be read", strerror(errno));
	while (status == 0 && fgets(line, sizeof line, layout) != NULL) {
		uint64_t sector;
		uint64_t size;
		uint64_t offset = 0u;
		uint64_t total;
		int      used = 0;
		int      more = 0;
		size_t   n = strlen(line);
		char     file[8800];
		int      in;
		uint64_t done = 0u;
		struct stat st;

		if (n > 0u && line[n - 1u] == '\n')
			line[n - 1u] = '\0';
		if (sscanf(line, "%" SCNu64 " %" SCNu64 " %n", &sector, &size, &used) != 2 || used == 0) {
			status = fail(detail, cap, "a line of layout.conf is not a file on this disc", line);
			break;
		}
		total = size;
		/* A piece of a fragmented file: @OFFSET/TOTAL before its path. */
		if (line[used] == '@') {
			if (sscanf(line + used, "@%" SCNu64 "/%" SCNu64 " %n", &offset, &total, &more) != 2 ||
			    more == 0 || offset + size > total) {
				status = fail(detail, cap, "a line of layout.conf is not a file on this disc", line);
				break;
			}
			used += more;
		}
		if (!plain_path(line + used) || sector > bytes / unit || sector * unit + size > bytes) {
			status = fail(detail, cap, "a line of layout.conf is not a file on this disc", line);
			break;
		}
		snprintf(file, sizeof file, "%s/files/%s", dir, line + used);
		if ((in = open(file, O_RDONLY | O_CLOEXEC | O_NOFOLLOW)) < 0) {
			status = fail(detail, cap, file, strerror(errno));
			break;
		}
		/* Longer would pass the hash, its tail never read, and an edit
		 * nothing reassembles should not read as an intact disc. */
		if (fstat(in, &st) != 0 || (uint64_t)st.st_size != total) {
			status = fail(detail, cap, "a file is not the size the layout says", file);
			close(in);
			break;
		}
		while (done < size && status == 0) {
			size_t n2 = size - done < sizeof buffer ? (size_t)(size - done) : sizeof buffer;

			if (read_all(in, buffer, n2, offset + done) != 0 ||
			    write_all(fd, buffer, n2, sector * unit + done) != 0)
				status = fail(detail, cap, "a file is shorter than the layout says", file);
			done += n2;
		}
		close(in);
	}
	fclose(layout);
	return status;
}

/* quirks/sectors/ laid over what the files gave. */
static int apply_quirks(const char *dir, int fd, uint64_t bytes, unsigned unit, char *detail,
                        size_t cap)
{
	static uint8_t buffer[COPY_BYTES];
	char           path[4300];
	DIR           *runs;
	struct dirent *entry;
	int            status = 0;

	snprintf(path, sizeof path, "%s/quirks/sectors", dir);
	if ((runs = opendir(path)) == NULL)
		return errno == ENOENT ? 0 : fail(detail, cap, path, strerror(errno));
	while (status == 0 && (entry = readdir(runs)) != NULL) {
		uint64_t sector;
		int      used = 0;
		char     file[4700];
		int      in;
		uint64_t at;
		ssize_t  got;

		if (entry->d_name[0] == '.')
			continue;
		if (sscanf(entry->d_name, "0x%" SCNx64 ".raw%n", &sector, &used) != 1 ||
		    entry->d_name[used] != '\0' || sector > bytes / unit) {
			status = fail(detail, cap, "a file in quirks/sectors is no run of this disc",
			              entry->d_name);
			break;
		}
		snprintf(file, sizeof file, "%s/%s", path, entry->d_name);
		if ((in = open(file, O_RDONLY | O_CLOEXEC | O_NOFOLLOW)) < 0) {
			status = fail(detail, cap, file, strerror(errno));
			break;
		}
		at = sector * unit;
		while (status == 0 && (got = read(in, buffer, sizeof buffer)) > 0) {
			if (at + (uint64_t)got > bytes || write_all(fd, buffer, (size_t)got, at) != 0)
				status = fail(detail, cap, "a run in quirks/sectors passes the disc's end",
				              entry->d_name);
			at += (uint64_t)got;
		}
		close(in);
	}
	closedir(runs);
	return status;
}

static int hash_fd(int fd, uint64_t bytes, char out[65])
{
	static uint8_t buffer[COPY_BYTES];
	hull_sha256    h;
	uint8_t        digest[32];
	uint64_t       done = 0u;

	hull_sha256_init(&h);
	while (done < bytes) {
		size_t n = bytes - done < sizeof buffer ? (size_t)(bytes - done) : sizeof buffer;

		if (read_all(fd, buffer, n, done) != 0)
			return -1;
		hull_sha256_update(&h, buffer, n);
		done += n;
	}
	hull_sha256_final(&h, digest);
	hull_sha256_hex(digest, out);
	return 0;
}

/* --- a raw dump ------------------------------------------------------------ */

/*
 * A CD's data sector, Mode 1, is 2352 bytes around its 2048 (ECMA-130):
 * twelve of sync, a header of its address, the data, an EDC over those,
 * eight zeros, and the Reed-Solomon P and Q parity of annex A. All of it
 * but the data follows from the data and the address, so none of it need
 * be kept where it is what it should be.
 */
#define RAW_SECTOR 2352u
#define RAW_DATA   16u
#define MSF_LEAD   150u   /* sector 0 is 00:02:00 */

#define MAX_TRACKS 99u

typedef struct raw_track {
	unsigned number;
	unsigned part;    /* the sheet's file it is in */
	uint64_t at;      /* in the dump, its INDEX 01 */
	uint64_t bytes;
	int      audio;
} raw_track;

/* A sheet of several files, a track each as a Redump set has them, is one
 * dump of them end to end, each file a part of it. */
typedef struct raw_part {
	char     name[256];
	uint64_t bytes;
} raw_part;

typedef struct raw_disc {
	uint64_t  bytes;
	uint64_t  data_sectors;
	char      sha[65];
	raw_track tracks[MAX_TRACKS];
	unsigned  count;
	raw_part  parts[MAX_TRACKS];
	unsigned  part_count;
} raw_disc;

static uint32_t edc_table[256];
static uint8_t  gf_forward[256];
static uint8_t  gf_backward[256];

static void tables(void)
{
	for (unsigned i = 0u; i < 256u; i++) {
		uint32_t edc = i;
		unsigned next = (i << 1) ^ ((i & 0x80u) != 0u ? 0x11Du : 0u);

		for (unsigned bit = 0u; bit < 8u; bit++)
			edc = (edc >> 1) ^ ((edc & 1u) != 0u ? 0xD8018001u : 0u);
		edc_table[i]   = edc;
		gf_forward[i]  = (uint8_t)next;
		gf_backward[i ^ (next & 0xFFu)] = (uint8_t)i;
	}
}

static void ecc_block(const uint8_t *from, unsigned major_count, unsigned minor_count,
                      unsigned major_mult, unsigned minor_step, uint8_t *to)
{
	unsigned size = major_count * minor_count;

	for (unsigned major = 0u; major < major_count; major++) {
		unsigned index = (major >> 1) * major_mult + (major & 1u);
		uint8_t  a = 0u;
		uint8_t  b = 0u;

		for (unsigned minor = 0u; minor < minor_count; minor++) {
			uint8_t byte = from[index];

			index += minor_step;
			if (index >= size)
				index -= size;
			a = (uint8_t)(a ^ byte);
			b = (uint8_t)(b ^ byte);
			a = gf_forward[a];
		}
		a = gf_backward[gf_forward[a] ^ b];
		to[major] = a;
		to[major + major_count] = (uint8_t)(a ^ b);
	}
}

/* The Mode 1 sector `sector` around its 2048 bytes of `data`. */
static void make_sector(uint8_t *raw, const uint8_t *data, uint64_t sector)
{
	uint64_t address = sector + MSF_LEAD;
	unsigned minute = (unsigned)(address / 4500u);
	unsigned second = (unsigned)(address / 75u % 60u);
	unsigned frame  = (unsigned)(address % 75u);
	uint32_t edc = 0u;

	memset(raw, 0, RAW_SECTOR);
	memset(raw + 1, 0xFF, 10u);
	raw[12] = (uint8_t)(minute / 10u << 4 | minute % 10u);
	raw[13] = (uint8_t)(second / 10u << 4 | second % 10u);
	raw[14] = (uint8_t)(frame / 10u << 4 | frame % 10u);
	raw[15] = 1u;
	memcpy(raw + RAW_DATA, data, SECTOR);
	for (unsigned i = 0u; i < 2064u; i++)
		edc = (edc >> 8) ^ edc_table[(edc ^ raw[i]) & 0xFFu];
	raw[2064] = (uint8_t)edc;
	raw[2065] = (uint8_t)(edc >> 8);
	raw[2066] = (uint8_t)(edc >> 16);
	raw[2067] = (uint8_t)(edc >> 24);
	ecc_block(raw + 12, 86u, 24u, 2u, 86u, raw + 2076);
	ecc_block(raw + 12, 52u, 43u, 86u, 88u, raw + 2248);
}

/* MM:SS:FF as bytes of a raw dump. */
static int cue_time(const char *text, uint64_t *at)
{
	unsigned minute;
	unsigned second;
	unsigned frame;

	if (sscanf(text, "%u:%u:%u", &minute, &second, &frame) != 3 || second > 59u || frame > 74u)
		return -1;
	*at = ((uint64_t)minute * 4500u + second * 75u + frame) * RAW_SECTOR;
	return 0;
}

/* The cue sheet at `path`: its files and tracks, each track's place its
 * time in its own file so far. A sheet of another shape says why it is
 * not read in `detail`. */
static int read_cue(const char *path, raw_disc *disc, char *detail, size_t cap)
{
	char  line[1024];
	FILE *cue = fopen(path, "re");

	if (cue == NULL)
		return fail(detail, cap, path, strerror(errno));
	while (fgets(line, sizeof line, cue) != NULL) {
		char     mode[32];
		char     when[32];
		char     name[512];
		unsigned number;
		unsigned index;

		if (sscanf(line, " FILE \"%511[^\"]\"", name) == 1) {
			if (disc->part_count == MAX_TRACKS || strchr(name, '/') != NULL ||
			    strlen(name) >= sizeof disc->parts[0].name) {
				fclose(cue);
				return fail(detail, cap, "a file in another directory is not read", name);
			}
			memcpy(disc->parts[disc->part_count++].name, name, strlen(name) + 1u);
		} else if (sscanf(line, " TRACK %u %31s", &number, mode) == 2) {
			if (disc->count == MAX_TRACKS || disc->part_count == 0u ||
			    (disc->count == 0u && strcmp(mode, "MODE1/2352") != 0) ||
			    (disc->count != 0u && strcmp(mode, "AUDIO") != 0)) {
				fclose(cue);
				return fail(detail, cap, "only a Mode 1 data track and audio after it are read "
				            "yet, and this track is", mode);
			}
			disc->tracks[disc->count].number = number;
			disc->tracks[disc->count].part = disc->part_count - 1u;
			disc->tracks[disc->count].audio = strcmp(mode, "AUDIO") == 0;
			disc->count++;
		} else if (sscanf(line, " INDEX %u %31s", &index, when) == 2 && index == 1u &&
		           disc->count != 0u) {
			if (cue_time(when, &disc->tracks[disc->count - 1u].at) != 0) {
				fclose(cue);
				return fail(detail, cap, "a time in the sheet cannot be read", when);
			}
		}
	}
	fclose(cue);
	if (disc->part_count == 0u || disc->count == 0u)
		return fail(detail, cap, "the sheet names no binary file or no track", NULL);
	return 0;
}

/* Bytes of the dump to `out`, as a RIFF WAVE of CD audio. */
static int write_wav(int dump, uint64_t at, uint64_t bytes, const char *path)
{
	static uint8_t buffer[COPY_BYTES];
	uint8_t        head[44];
	int            fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
	uint32_t       data = (uint32_t)bytes;
	uint32_t       riff = data + 36u;
	static const uint8_t format[] = { 16, 0, 0, 0, 1, 0, 2, 0, 0x44, 0xAC, 0, 0, 0x10, 0xB1, 2, 0,
	                                  4, 0, 16, 0 };
	uint64_t       done = 0u;

	if (fd < 0)
		return -1;
	memcpy(head, "RIFF", 4u);
	for (unsigned i = 0u; i < 4u; i++) {
		head[4u + i]  = (uint8_t)(riff >> (8u * i));
		head[40u + i] = (uint8_t)(data >> (8u * i));
	}
	memcpy(head + 8, "WAVEfmt ", 8u);
	memcpy(head + 16, format, sizeof format);
	memcpy(head + 36, "data", 4u);
	if (write_all(fd, head, sizeof head, 0u) != 0) {
		close(fd);
		return -1;
	}
	while (done < bytes) {
		size_t n = bytes - done < sizeof buffer ? (size_t)(bytes - done) : sizeof buffer;

		if (read_all(dump, buffer, n, at + done) != 0 ||
		    write_all(fd, buffer, n, sizeof head + done) != 0) {
			close(fd);
			return -1;
		}
		done += n;
	}
	return close(fd);
}

/* media.conf's raw dump, what hull_media_write_raw and verify read. 0, or
 * 1 where the collection has none. */
static int read_raw(const char *dir, raw_disc *disc)
{
	static char text[65536];
	char        path[4300];
	int         fd;
	ssize_t     got;
	const char *at;

	snprintf(path, sizeof path, "%s/media.conf", dir);
	if ((fd = open(path, O_RDONLY | O_CLOEXEC)) < 0)
		return -1;
	got = read(fd, text, sizeof text - 1u);
	close(fd);
	if (got <= 0)
		return -1;
	text[got] = '\0';
	memset(disc, 0, sizeof *disc);
	if ((at = strstr(text, "(raw (bytes ")) == NULL)
		return 1;
	if (sscanf(at, "(raw (bytes %" SCNu64 ") (sha256 \"%64[0-9a-f]\") (sectors %" SCNu64 "))",
	           &disc->bytes, disc->sha, &disc->data_sectors) != 3)
		return -1;
	for (at = strstr(text, "(part "); at != NULL; at = strstr(at + 1, "(part ")) {
		raw_part *part = &disc->parts[disc->part_count];

		if (disc->part_count == MAX_TRACKS ||
		    sscanf(at, "(part \"%255[^\"]\" %" SCNu64 ")", part->name, &part->bytes) != 2 ||
		    !plain_path(part->name) || strchr(part->name, '/') != NULL)
			return -1;
		disc->part_count++;
	}
	for (at = strstr(text, "(track "); at != NULL; at = strstr(at + 1, "(track ")) {
		raw_track *t = &disc->tracks[disc->count];
		char       file[64];

		if (disc->count == MAX_TRACKS ||
		    sscanf(at, "(track (number %u) (audio \"%63[^\"]\") (at %" SCNu64 ") (bytes %" SCNu64 "))",
		           &t->number, file, &t->at, &t->bytes) != 4)
			return -1;
		t->audio = 1;
		disc->count++;
	}
	return 0;
}

/* The dump as data and audio alone give it, on `fd`, `disc->bytes` long
 * and empty: every data sector made from the disc reassembled in `cooked`,
 * and each track's samples from tracks/. */
static int project_raw(const char *dir, const raw_disc *disc, int cooked, int fd, char *detail,
                       size_t cap)
{
	static uint8_t buffer[COPY_BYTES];
	uint8_t        data[SECTOR];
	uint8_t        raw[RAW_SECTOR];

	tables();
	for (uint64_t s = 0u; s < disc->data_sectors; s++) {
		if (read_all(cooked, data, SECTOR, s * SECTOR) != 0)
			return fail(detail, cap, "the data track cannot be read back", NULL);
		make_sector(raw, data, s);
		if (write_all(fd, raw, RAW_SECTOR, s * RAW_SECTOR) != 0)
			return fail(detail, cap, "the dump cannot be written", strerror(errno));
	}
	for (unsigned i = 0u; i < disc->count; i++) {
		char     path[4400];
		int      in;
		uint64_t done = 0u;

		if (!disc->tracks[i].audio)
			continue;
		snprintf(path, sizeof path, "%s/tracks/%02u.wav", dir, disc->tracks[i].number);
		if ((in = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW)) < 0)
			return fail(detail, cap, path, strerror(errno));
		while (done < disc->tracks[i].bytes) {
			uint64_t left = disc->tracks[i].bytes - done;
			size_t   n = left < sizeof buffer ? (size_t)left : sizeof buffer;

			if (disc->tracks[i].at + done + n > disc->bytes ||
			    read_all(in, buffer, n, 44u + done) != 0 ||
			    write_all(fd, buffer, n, disc->tracks[i].at + done) != 0) {
				close(in);
				return fail(detail, cap, "an audio track is not the size media.conf says", path);
			}
			done += n;
		}
		close(in);
	}
	return 0;
}

/* quirks/raw/ over the projected dump. */
static int apply_raw_quirks(const char *dir, int fd, uint64_t bytes, char *detail, size_t cap)
{
	static uint8_t buffer[COPY_BYTES];
	char           path[4300];
	DIR           *runs;
	struct dirent *entry;
	int            status = 0;

	snprintf(path, sizeof path, "%s/quirks/raw", dir);
	if ((runs = opendir(path)) == NULL)
		return errno == ENOENT ? 0 : fail(detail, cap, path, strerror(errno));
	while (status == 0 && (entry = readdir(runs)) != NULL) {
		uint64_t sector;
		int      used = 0;
		char     file[4700];
		int      in;
		uint64_t at;
		ssize_t  got;

		if (entry->d_name[0] == '.')
			continue;
		if (sscanf(entry->d_name, "0x%" SCNx64 ".raw%n", &sector, &used) != 1 ||
		    entry->d_name[used] != '\0' || sector > bytes / RAW_SECTOR) {
			status = fail(detail, cap, "a file in quirks/raw is no run of this dump",
			              entry->d_name);
			break;
		}
		snprintf(file, sizeof file, "%s/%s", path, entry->d_name);
		if ((in = open(file, O_RDONLY | O_CLOEXEC | O_NOFOLLOW)) < 0) {
			status = fail(detail, cap, file, strerror(errno));
			break;
		}
		at = sector * RAW_SECTOR;
		while (status == 0 && (got = read(in, buffer, sizeof buffer)) > 0) {
			if (at + (uint64_t)got > bytes || write_all(fd, buffer, (size_t)got, at) != 0)
				status = fail(detail, cap, "a run in quirks/raw passes the dump's end",
				              entry->d_name);
			at += (uint64_t)got;
		}
		close(in);
	}
	closedir(runs);
	return status;
}

int hull_media_write_raw(const char *dir, int fd, char *detail, size_t cap)
{
	raw_disc *disc = malloc(sizeof *disc);
	char      path[4300];
	char      got[65];
	int       cooked = -1;
	int       kind;
	int       status = -1;

	if (disc == NULL)
		return fail(detail, cap, "out of memory", NULL);
	kind = read_raw(dir, disc);
	if (kind != 0) {
		free(disc);
		return kind > 0 ? 1 : fail(detail, cap, "media.conf's (raw ...) cannot be read", NULL);
	}
	snprintf(path, sizeof path, "%s/.cooked-XXXXXX", dir);
	if ((cooked = mkostemp(path, O_CLOEXEC)) < 0) {
		fail(detail, cap, path, strerror(errno));
		goto done;
	}
	unlink(path);
	if (hull_media_write(dir, cooked, detail, cap) != 0)
		goto done;
	if (ftruncate(fd, (off_t)disc->bytes) != 0) {
		fail(detail, cap, "the dump cannot be sized", strerror(errno));
		goto done;
	}
	if (project_raw(dir, disc, cooked, fd, detail, cap) != 0 ||
	    apply_raw_quirks(dir, fd, disc->bytes, detail, cap) != 0)
		goto done;
	if (hash_fd(fd, disc->bytes, got) != 0) {
		fail(detail, cap, "the dump cannot be read back", strerror(errno));
		goto done;
	}
	if (strcmp(got, disc->sha) != 0) {
		snprintf(detail, cap, "the collection no longer gives its dump: sha256 %s, and %s was "
		         "imported", got, disc->sha);
		goto done;
	}
	status = 0;
done:
	if (cooked >= 0)
		close(cooked);
	free(disc);
	return status;
}

int hull_media_exact(const char *dir)
{
	char path[4300];

	snprintf(path, sizeof path, "%s/layout.conf", dir);
	return access(path, R_OK) == 0;
}

int hull_media_write(const char *dir, int fd, char *detail, size_t cap)
{
	uint64_t bytes;
	unsigned unit;
	char     want[65];
	char     got[65];

	if (read_conf(dir, &bytes, want, &unit) != 0)
		return fail(detail, cap, "the collection's media.conf gives no bytes and sha256", NULL);
	if (ftruncate(fd, (off_t)bytes) != 0)
		return fail(detail, cap, "the disc's image cannot be sized", strerror(errno));
	if (place_files(dir, fd, bytes, unit, detail, cap) != 0 ||
	    apply_quirks(dir, fd, bytes, unit, detail, cap) != 0)
		return -1;
	if (hash_fd(fd, bytes, got) != 0)
		return fail(detail, cap, "the disc's image cannot be read back", strerror(errno));
	if (strcmp(want, got) != 0) {
		snprintf(detail, cap, "the collection no longer gives its disc: sha256 %s, and %s was "
		         "imported", got, want);
		return -1;
	}
	return 0;
}

int hull_media_export(const char *dir, const char *out, char *detail, size_t cap)
{
	raw_disc *disc = malloc(sizeof *disc);
	int       kind = -1;
	int       fd = -1;
	int       status = -1;
	char      path[8800];

	if (disc == NULL)
		return fail(detail, cap, "out of memory", NULL);
	kind = read_raw(dir, disc);
	if (kind < 0) {
		fail(detail, cap, "media.conf's (raw ...) cannot be read", NULL);
		goto done;
	}
	if (kind != 0 || disc->part_count < 2u) {
		/* One file: the dump, or the image. */
		if ((fd = open(out, O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0644)) < 0) {
			fail(detail, cap, out, strerror(errno));
			goto done;
		}
		status = hull_media_write_raw(dir, fd, detail, cap);
		if (status == 1)
			status = hull_media_write(dir, fd, detail, cap);
		close(fd);
		fd = -1;
		if (status != 0)
			unlink(out);
		goto done;
	}
	/* Several: `out` a directory of them and the sheet, as they came. */
	if (mkdir(out, 0755) != 0) {
		fail(detail, cap, out, strerror(errno));
		goto done;
	}
	snprintf(path, sizeof path, "%s/.dump-XXXXXX", out);
	if ((fd = mkostemp(path, O_CLOEXEC)) < 0) {
		fail(detail, cap, path, strerror(errno));
		goto done;
	}
	unlink(path);
	if (hull_media_write_raw(dir, fd, detail, cap) != 0)
		goto done;
	{
		static uint8_t buffer[COPY_BYTES];
		uint64_t       at = 0u;
		char           sheet[4400];

		for (unsigned i = 0u; i < disc->part_count; i++) {
			int part;

			snprintf(path, sizeof path, "%s/%s", out, disc->parts[i].name);
			if ((part = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644)) < 0) {
				fail(detail, cap, path, strerror(errno));
				goto done;
			}
			for (uint64_t done = 0u; done < disc->parts[i].bytes;) {
				uint64_t left = disc->parts[i].bytes - done;
				size_t   n = left < sizeof buffer ? (size_t)left : sizeof buffer;

				if (read_all(fd, buffer, n, at + done) != 0 || write_all(part, buffer, n, done) != 0) {
					close(part);
					fail(detail, cap, path, strerror(errno));
					goto done;
				}
				done += n;
			}
			close(part);
			at += disc->parts[i].bytes;
		}
		snprintf(sheet, sizeof sheet, "%s/disc.cue", dir);
		snprintf(path, sizeof path, "%s/disc.cue", out);
		{
			int     in = open(sheet, O_RDONLY | O_CLOEXEC);
			int     to = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
			ssize_t got = in >= 0 ? read(in, buffer, sizeof buffer) : -1;

			if (got < 0 || to < 0 || write_all(to, buffer, (size_t)got, 0u) != 0) {
				if (in >= 0)
					close(in);
				if (to >= 0)
					close(to);
				fail(detail, cap, "the cue sheet cannot be written", path);
				goto done;
			}
			close(in);
			close(to);
		}
	}
	status = 0;
done:
	if (fd >= 0)
		close(fd);
	free(disc);
	return status;
}

/* --- import --------------------------------------------------------------- */

static int remove_entry(const char *path, const struct stat *file, int kind, struct FTW *where)
{
	(void)file;
	(void)kind;
	(void)where;
	remove(path);
	return 0;
}

static int by_sector(const void *a, const void *b)
{
	const placed *x = a;
	const placed *y = b;

	return x->sector < y->sector ? -1 : x->sector > y->sector;
}

/* The bytes of the image that the placed files do not give, each run a
 * file. `naive` is the image with the files alone. */
static int write_quirks(const walk *w, int naive, const char *dir, hull_media_report *report,
                        char *detail, size_t cap)
{
	static uint8_t ours[COPY_BYTES];
	static uint8_t theirs[COPY_BYTES];
	char           path[4400];
	int            run = -1;
	uint64_t       run_at = 0u;

	for (uint64_t at = 0u; at < w->bytes;) {
		size_t n = w->bytes - at < sizeof ours ? (size_t)(w->bytes - at) : sizeof ours;

		if (read_all(w->image, theirs, n, at) != 0 || read_all(naive, ours, n, at) != 0)
			return fail(detail, cap, "the image cannot be read", strerror(errno));
		for (size_t i = 0u; i < n; i += w->unit) {
			size_t piece = n - i < w->unit ? n - i : w->unit;

			if (memcmp(ours + i, theirs + i, piece) == 0) {
				if (run >= 0) {
					close(run);
					run = -1;
				}
				continue;
			}
			if (run < 0) {
				snprintf(path, sizeof path, "%s/quirks/sectors/0x%" PRIX64 ".raw", dir,
				         (at + i) / w->unit);
				run_at = at + i;
				if ((run = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644)) < 0)
					return fail(detail, cap, path, strerror(errno));
			}
			if (write_all(run, theirs + i, piece, at + i - run_at) != 0) {
				close(run);
				return fail(detail, cap, path, strerror(errno));
			}
			report->patch_bytes += piece;
		}
		at += n;
	}
	if (run >= 0)
		close(run);
	return 0;
}

static int import_into(walk *w, const char *dir, hull_media_report *report, char *detail,
                       size_t cap)
{
	char        volume[33] = "";
	char        path[4400];
	char        sha[65];
	FILE       *file;
	int         naive = -1;
	int         status = -1;

	snprintf(path, sizeof path, "%s/quirks", dir);
	if (mkdir(w->root, 0755) != 0 || mkdir(path, 0755) != 0)
		return fail(detail, cap, "the collection's directories cannot be made", strerror(errno));
	snprintf(path, sizeof path, "%s/quirks/sectors", dir);
	if (mkdir(path, 0755) != 0)
		return fail(detail, cap, "the collection's directories cannot be made", strerror(errno));
	w->unit = SECTOR;
	if (read_iso(w, volume, sizeof volume) != 0)
		return fail(detail, cap, "the disc's files cannot be written", strerror(errno));
	if (w->count == 0u && w->dirs == 0u) {
		int found = 0;

		if (read_fat(w, 0u, "", &found, volume, sizeof volume) != 0 ||
		    (!found && read_ext(w, 0u, "", &found, volume, sizeof volume) != 0) ||
		    (!found && read_partitions(w, volume, sizeof volume) != 0))
			return fail(detail, cap, "the disk's files cannot be written", strerror(errno));
	}
	if (hash_fd(w->image, w->bytes, sha) != 0)
		return fail(detail, cap, "the image cannot be read", strerror(errno));
	if (w->count != 0u)
		qsort(w->files, w->count, sizeof w->files[0], by_sector);

	snprintf(path, sizeof path, "%s/media.conf", dir);
	if ((file = fopen(path, "we")) == NULL)
		return fail(detail, cap, path, strerror(errno));
	fprintf(file, "(disc (volume \"%s\") (bytes %" PRIu64 ") (sha256 \"%s\") (unit %u)%s)\n",
	        volume, w->bytes, sha, w->unit, w->extra != NULL ? w->extra : "");
	if (fclose(file) != 0)
		return fail(detail, cap, path, strerror(errno));
	snprintf(path, sizeof path, "%s/layout.conf", dir);
	if ((file = fopen(path, "we")) == NULL)
		return fail(detail, cap, path, strerror(errno));
	for (size_t i = 0u; i < w->count; i++) {
		const placed *f = &w->files[i];

		if (f->size == f->total)
			fprintf(file, "%" PRIu64 " %" PRIu64 " %s\n", f->sector, f->size, f->path);
		else
			fprintf(file, "%" PRIu64 " %" PRIu64 " @%" PRIu64 "/%" PRIu64 " %s\n", f->sector,
			        f->size, f->offset, f->total, f->path);
	}
	if (fclose(file) != 0)
		return fail(detail, cap, path, strerror(errno));

	/* Project, diff, and reassemble: the last is the proof, by the same
	 * function a machine's disc is made with. */
	snprintf(path, sizeof path, "%s/.projected", dir);
	if ((naive = open(path, O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0600)) < 0)
		return fail(detail, cap, path, strerror(errno));
	unlink(path);
	if (ftruncate(naive, (off_t)w->bytes) != 0 ||
	    place_files(dir, naive, w->bytes, w->unit, detail, cap) != 0 ||
	    write_quirks(w, naive, dir, report, detail, cap) != 0)
		goto done;
	close(naive);
	snprintf(path, sizeof path, "%s/.projected", dir);
	if ((naive = open(path, O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0600)) < 0) {
		fail(detail, cap, path, strerror(errno));
		goto done;
	}
	unlink(path);
	if (hull_media_write(dir, naive, detail, cap) != 0)
		goto done;
	for (size_t i = 0u; i < w->count; i++)
		report->files += w->files[i].offset == 0u;
	report->bytes = w->bytes;
	status = 0;
done:
	if (naive >= 0)
		close(naive);
	return status;
}

/* The raw dump's bytes that its projection does not give, each run a file
 * of quirks/raw. As write_quirks, a raw sector at a time. */
static int write_raw_quirks(int dump, int naive, uint64_t bytes, const char *dir,
                            hull_media_report *report, char *detail, size_t cap)
{
	uint8_t  ours[RAW_SECTOR];
	uint8_t  theirs[RAW_SECTOR];
	char     path[4400];
	int      run = -1;
	uint64_t run_at = 0u;

	for (uint64_t at = 0u; at < bytes; at += RAW_SECTOR) {
		size_t n = bytes - at < RAW_SECTOR ? (size_t)(bytes - at) : RAW_SECTOR;

		if (read_all(dump, theirs, n, at) != 0 || read_all(naive, ours, n, at) != 0)
			return fail(detail, cap, "the dump cannot be read", strerror(errno));
		if (memcmp(ours, theirs, n) == 0) {
			if (run >= 0) {
				close(run);
				run = -1;
			}
			continue;
		}
		if (run < 0) {
			snprintf(path, sizeof path, "%s/quirks/raw/0x%" PRIX64 ".raw", dir, at / RAW_SECTOR);
			run_at = at;
			if ((run = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644)) < 0)
				return fail(detail, cap, path, strerror(errno));
		}
		if (write_all(run, theirs, n, at - run_at) != 0) {
			close(run);
			return fail(detail, cap, path, strerror(errno));
		}
		report->patch_bytes += n;
	}
	if (run >= 0)
		close(run);
	return 0;
}

/* `hull media import X.cue`: the data track's 2048 bytes a sector taken out
 * and imported as a disc, the audio to tracks/, and the dump's remainder
 * found by regenerating it. */
static int import_cue(const char *cue, const char *dir, hull_media_report *report, char *detail,
                      size_t cap)
{
	static uint8_t raw[RAW_SECTOR];
	raw_disc      *disc = calloc(1u, sizeof *disc);
	char          *extra = malloc(MAX_TRACKS * (96u + 300u) + 256u);
	char           path[4400];
	char           cooked_path[4400];
	char           dump_path[4400] = "";
	const char    *slash = strrchr(cue, '/');
	struct stat    st;
	int            dump = -1;
	int            cooked = -1;
	int            naive = -1;
	int            status = -1;
	size_t         used = 0u;
	walk          *w = NULL;
	uint64_t       patch_cooked;

	if (disc == NULL || extra == NULL) {
		fail(detail, cap, "out of memory", NULL);
		goto done;
	}
	if (read_cue(cue, disc, detail, cap) != 0)
		goto done;
	if (mkdir(dir, 0755) != 0) {
		fail(detail, cap, dir, strerror(errno));
		goto done;
	}
	/* The dump: its one file, or its several end to end. */
	for (unsigned i = 0u; i < disc->part_count; i++) {
		snprintf(path, sizeof path, "%.*s%s", slash != NULL ? (int)(slash + 1 - cue) : 0, cue,
		         disc->parts[i].name);
		if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) {
			fail(detail, cap, path, "not a file");
			goto done;
		}
		disc->parts[i].bytes = (uint64_t)st.st_size;
		for (unsigned t = 0u; t < disc->count; t++)
			if (disc->tracks[t].part == i)
				disc->tracks[t].at += disc->bytes;
		disc->bytes += (uint64_t)st.st_size;
	}
	if (disc->part_count == 1u) {
		dump = open(path, O_RDONLY | O_CLOEXEC);
	} else {
		static uint8_t buffer[COPY_BYTES];
		uint64_t       at = 0u;

		snprintf(dump_path, sizeof dump_path, "%s/.dump", dir);
		dump = open(dump_path, O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
		for (unsigned i = 0u; dump >= 0 && i < disc->part_count; i++) {
			int     in;
			ssize_t got;

			snprintf(path, sizeof path, "%.*s%s", slash != NULL ? (int)(slash + 1 - cue) : 0, cue,
			         disc->parts[i].name);
			if ((in = open(path, O_RDONLY | O_CLOEXEC)) < 0) {
				fail(detail, cap, path, strerror(errno));
				goto done;
			}
			while ((got = read(in, buffer, sizeof buffer)) > 0) {
				if (write_all(dump, buffer, (size_t)got, at) != 0) {
					close(in);
					fail(detail, cap, dump_path, strerror(errno));
					goto done;
				}
				at += (uint64_t)got;
			}
			close(in);
		}
	}
	if (dump < 0) {
		fail(detail, cap, path, strerror(errno));
		goto done;
	}
	for (unsigned i = 0u; i < disc->count; i++) {
		uint64_t end = i + 1u < disc->count ? disc->tracks[i + 1u].at : disc->bytes;

		if (disc->tracks[i].at > end || end > disc->bytes) {
			fail(detail, cap, "the sheet's tracks are not in order within the dump", NULL);
			goto done;
		}
		disc->tracks[i].bytes = end - disc->tracks[i].at;
	}
	if (disc->tracks[0].at != 0u) {
		fail(detail, cap, "the data track does not start the dump", NULL);
		goto done;
	}
	disc->data_sectors = disc->tracks[0].bytes / RAW_SECTOR;
	if (hash_fd(dump, disc->bytes, disc->sha) != 0) {
		fail(detail, cap, "the dump cannot be read", strerror(errno));
		goto done;
	}

	/* The data track as a disc. */
	snprintf(cooked_path, sizeof cooked_path, "%s/.cooked", dir);
	if ((cooked = open(cooked_path, O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0600)) < 0) {
		fail(detail, cap, cooked_path, strerror(errno));
		goto done;
	}
	for (uint64_t s = 0u; s < disc->data_sectors; s++) {
		if (read_all(dump, raw, RAW_SECTOR, s * RAW_SECTOR) != 0 ||
		    write_all(cooked, raw + RAW_DATA, SECTOR, s * SECTOR) != 0) {
			fail(detail, cap, "the data track cannot be read", strerror(errno));
			goto done;
		}
	}
	used = (size_t)snprintf(extra, 256u, " (raw (bytes %" PRIu64 ") (sha256 \"%s\") (sectors %" PRIu64
	                        "))", disc->bytes, disc->sha, disc->data_sectors);
	if (disc->part_count > 1u) {
		used += (size_t)sprintf(extra + used, " (parts");
		for (unsigned i = 0u; i < disc->part_count; i++)
			used += (size_t)sprintf(extra + used, " (part \"%s\" %" PRIu64 ")", disc->parts[i].name,
			                        disc->parts[i].bytes);
		used += (size_t)sprintf(extra + used, ")");
	}
	if (disc->count > 1u) {
		used += (size_t)sprintf(extra + used, " (tracks");
		for (unsigned i = 1u; i < disc->count; i++)
			used += (size_t)sprintf(extra + used, " (track (number %u) (audio \"tracks/%02u.wav\") "
			                        "(at %" PRIu64 ") (bytes %" PRIu64 "))", disc->tracks[i].number,
			                        disc->tracks[i].number, disc->tracks[i].at,
			                        disc->tracks[i].bytes);
		sprintf(extra + used, ")");
	}
	if ((w = calloc(1u, sizeof *w)) == NULL) {
		fail(detail, cap, "out of memory", NULL);
		goto done;
	}
	w->image = cooked;
	w->bytes = disc->data_sectors * SECTOR;
	w->extra = extra;
	snprintf(w->root, sizeof w->root, "%s/files", dir);
	if (import_into(w, dir, report, detail, cap) != 0)
		goto done;
	patch_cooked = report->patch_bytes;

	/* The audio. */
	snprintf(path, sizeof path, "%s/tracks", dir);
	if (disc->count > 1u && mkdir(path, 0755) != 0) {
		fail(detail, cap, path, strerror(errno));
		goto done;
	}
	for (unsigned i = 1u; i < disc->count; i++) {
		snprintf(path, sizeof path, "%s/tracks/%02u.wav", dir, disc->tracks[i].number);
		disc->tracks[i].audio = 1;
		if (write_wav(dump, disc->tracks[i].at, disc->tracks[i].bytes, path) != 0) {
			fail(detail, cap, path, strerror(errno));
			goto done;
		}
	}
	disc->tracks[0].audio = 0;

	/* The sheet, as it came. */
	snprintf(path, sizeof path, "%s/disc.cue", dir);
	{
		static uint8_t text[65536];
		int            in = open(cue, O_RDONLY | O_CLOEXEC);
		ssize_t        got = in >= 0 ? read(in, text, sizeof text) : -1;
		int            out = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);

		if (in >= 0)
			close(in);
		if (got < 0 || got == (ssize_t)sizeof text || out < 0 ||
		    write_all(out, text, (size_t)got, 0u) != 0) {
			if (out >= 0)
				close(out);
			fail(detail, cap, "the cue sheet cannot be kept", strerror(errno));
			goto done;
		}
		close(out);
	}

	/* What regenerating the dump does not give. */
	snprintf(path, sizeof path, "%s/quirks/raw", dir);
	if (mkdir(path, 0755) != 0) {
		fail(detail, cap, path, strerror(errno));
		goto done;
	}
	snprintf(path, sizeof path, "%s/.naive", dir);
	if ((naive = open(path, O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0600)) < 0) {
		fail(detail, cap, path, strerror(errno));
		goto done;
	}
	unlink(path);
	if (ftruncate(naive, (off_t)disc->bytes) != 0 ||
	    project_raw(dir, disc, cooked, naive, detail, cap) != 0 ||
	    write_raw_quirks(dump, naive, disc->bytes, dir, report, detail, cap) != 0)
		goto done;
	close(naive);
	naive = -1;
	/* Nothing left is a disc that is what its data says: a machine may
	 * run it, nothing being lost by reading it as files. */
	snprintf(path, sizeof path, "%s/quirks/raw", dir);
	rmdir(path);
	snprintf(path, sizeof path, "%s/.naive", dir);
	if ((naive = open(path, O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0600)) < 0) {
		fail(detail, cap, path, strerror(errno));
		goto done;
	}
	unlink(path);
	if (hull_media_write_raw(dir, naive, detail, cap) != 0)
		goto done;
	report->bytes = disc->bytes;
	(void)patch_cooked;
	status = 0;
done:
	if (cooked >= 0) {
		close(cooked);
		unlink(cooked_path);
	}
	if (naive >= 0)
		close(naive);
	if (dump >= 0)
		close(dump);
	if (dump_path[0] != '\0')
		unlink(dump_path);
	if (w != NULL) {
		for (size_t i = 0u; i < w->count; i++)
			free(w->files[i].path);
		free(w->files);
		free(w);
	}
	free(extra);
	free(disc);
	if (status != 0)
		nftw(dir, remove_entry, 16, FTW_DEPTH | FTW_PHYS);
	return status;
}

int hull_media_import(const char *image, const char *dir, hull_media_report *report, char *detail,
                      size_t cap)
{
	size_t      length = strlen(image);
	walk       *w;
	struct stat st;
	int         status = -1;

	memset(report, 0, sizeof *report);
	if (length > 4u && strcasecmp(image + length - 4u, ".cue") == 0) {
		if (access(dir, F_OK) == 0)
			return fail(detail, cap, dir, strerror(EEXIST));
		return import_cue(image, dir, report, detail, cap);
	}
	w = calloc(1u, sizeof *w);
	if (w == NULL)
		return fail(detail, cap, "out of memory", NULL);
	if ((w->image = open(image, O_RDONLY | O_CLOEXEC)) < 0 || fstat(w->image, &st) != 0 ||
	    !S_ISREG(st.st_mode)) {
		fail(detail, cap, image, w->image < 0 ? strerror(errno) : "not a file");
		goto done;
	}
	w->bytes = (uint64_t)st.st_size;
	if (mkdir(dir, 0755) != 0) {
		fail(detail, cap, dir, strerror(errno));
		goto done;
	}
	snprintf(w->root, sizeof w->root, "%s/files", dir);
	status = import_into(w, dir, report, detail, cap);
	if (status != 0)
		nftw(dir, remove_entry, 16, FTW_DEPTH | FTW_PHYS);
done:
	if (w->image >= 0)
		close(w->image);
	for (size_t i = 0u; i < w->count; i++)
		free(w->files[i].path);
	free(w->files);
	free(w);
	return status;
}
