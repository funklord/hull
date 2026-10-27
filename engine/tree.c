/*
 * tree.c -- a directory as a tar stream, the same bytes for the same tree.
 */

#define _GNU_SOURCE

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/xattr.h>
#include <unistd.h>

#include "tree.h"

#define BLOCK     512u
#define MAX_DEPTH 256u
#define MAX_PATH  4096u

typedef struct writer {
	hull_tree_sink sink;
	void          *context;
	char          *detail;
	size_t         detail_cap;
	char           path[MAX_PATH];  /* the entry's name in the stream */
	int            one_fs;          /* only the root's own mount */
	uint64_t       mount;           /* the root's mount id, where one_fs */
	dev_t          device;          /* its device, where no mount id is given */
	uint8_t        buffer[65536];
} writer;

static int fail(writer *w, const char *what)
{
	snprintf(w->detail, w->detail_cap, "%s: %s", w->path[0] != '\0' ? w->path : ".", what);
	return -1;
}

static int put(writer *w, const void *data, size_t n)
{
	if (w->sink(w->context, data, n) != 0) {
		snprintf(w->detail, w->detail_cap, "the import's stream was refused");
		return -1;
	}
	return 0;
}

/* Zeroes to the end of the block, after `n` bytes of content. */
static int pad(writer *w, unsigned long long n)
{
	static const uint8_t zeroes[BLOCK];
	size_t               rest = (size_t)(n % BLOCK);

	return rest == 0u ? 0 : put(w, zeroes, BLOCK - rest);
}

/* One pax record, "<length> <key>=<value>\n", the length counting itself;
 * the value's bytes as they are, so that an xattr's need not be text. */
static size_t record(char *out, size_t cap, size_t at, const char *key, const void *value,
                     size_t value_length)
{
	size_t body = strlen(key) + value_length + 3u;  /* " ", "=" and "\n" */
	size_t length = body + 1u;
	char   digits[24];
	int    n;

	/* The length's own digits, until it stops growing. */
	while ((size_t)snprintf(digits, sizeof digits, "%zu", length) + body != length)
		length = (size_t)snprintf(digits, sizeof digits, "%zu", length) + body;
	if (at + length >= cap)
		return cap;
	n = snprintf(out + at, cap - at, "%zu %s=", length, key);
	memcpy(out + at + (size_t)n, value, value_length);
	out[at + (size_t)n + value_length] = '\n';
	return at + length;
}

static size_t text_record(char *out, size_t cap, size_t at, const char *key, const char *value)
{
	return record(out, cap, at, key, value, strlen(value));
}

static void octal(uint8_t *field, size_t width, unsigned long long value)
{
	snprintf((char *)field, width, "%0*llo", (int)(width - 1u), value);
}

static int write_header(writer *w, const hull_tar_entry *e)
{
	uint8_t  block[BLOCK];
	char    *records;
	size_t   cap = 3u * MAX_PATH + 64u;
	size_t   at = 0u;
	unsigned sum = 0u;
	char     number[24];

	for (size_t i = 0u; i < e->xattr_count; i++)
		cap += strlen(e->xattr_names[i]) + e->xattr_lengths[i] + 64u;
	if ((records = malloc(cap)) == NULL)
		return fail(w, "out of memory");
	if (strlen(e->name) > 100u)
		at = text_record(records, cap, at, "path", e->name);
	if (e->link != NULL && strlen(e->link) > 100u)
		at = text_record(records, cap, at, "linkpath", e->link);
	if (e->uid > 07777777u) {
		snprintf(number, sizeof number, "%llu", e->uid);
		at = text_record(records, cap, at, "uid", number);
	}
	if (e->gid > 07777777u) {
		snprintf(number, sizeof number, "%llu", e->gid);
		at = text_record(records, cap, at, "gid", number);
	}
	if (e->size > 077777777777ull) {
		snprintf(number, sizeof number, "%llu", e->size);
		at = text_record(records, cap, at, "size", number);
	}
	for (size_t i = 0u; i < e->xattr_count && at < cap; i++) {
		char key[300];

		snprintf(key, sizeof key, "SCHILY.xattr.%s", e->xattr_names[i]);
		at = record(records, cap, at, key, e->xattr_values[i], e->xattr_lengths[i]);
	}
	if (at >= cap) {
		free(records);
		return fail(w, "its name is too long");
	}
	if (at != 0u) {
		hull_tar_entry pax = { .name = "././@PaxHeader", .type = 'x', .mode = 0644,
		                       .mtime = e->mtime, .size = at };

		if (write_header(w, &pax) != 0 || put(w, records, at) != 0 || pad(w, at) != 0) {
			free(records);
			return -1;
		}
	}
	free(records);

	memset(block, 0, sizeof block);
	memcpy(block, e->name, strlen(e->name) > 100u ? 100u : strlen(e->name));
	octal(block + 100, 8u, (unsigned long long)(e->mode & 07777));
	octal(block + 108, 8u, e->uid & 07777777u);
	octal(block + 116, 8u, e->gid & 07777777u);
	octal(block + 124, 12u, e->size > 077777777777ull ? 0u : e->size);
	octal(block + 136, 12u, e->mtime > 077777777777ull ? 077777777777ull : e->mtime);
	block[156] = (uint8_t)e->type;
	if (e->link != NULL)
		memcpy(block + 157, e->link, strlen(e->link) > 100u ? 100u : strlen(e->link));
	memcpy(block + 257, "ustar", 6u);
	memcpy(block + 263, "00", 2u);
	if (e->type == '3' || e->type == '4') {
		octal(block + 329, 8u, e->major & 07777777u);
		octal(block + 337, 8u, e->minor & 07777777u);
	}
	memset(block + 148, ' ', 8u);
	for (unsigned i = 0u; i < BLOCK; i++)
		sum += block[i];
	snprintf((char *)block + 148, 8u, "%06o", sum);
	block[155] = ' ';
	return put(w, block, sizeof block);
}

static int name_order(const void *a, const void *b)
{
	return strcmp(*(char *const *)a, *(char *const *)b);
}

/* An entry of the tree on disk: its mode, owner and mtime as they are, and
 * its user.* and security.* xattrs, by name, as a store's commit keeps
 * them, so that a file capability, ping's say, goes with the file. `at`
 * is `entry` in `dir`, or -1 for none. */
static int header(writer *w, int dir, const char *at, const char *name, char type,
                  const struct stat *st, unsigned long long size, const char *link)
{
	hull_tar_entry e = { .name = name, .type = type, .mode = (unsigned)(st->st_mode & 07777),
	                     .uid = (unsigned long long)st->st_uid,
	                     .gid = (unsigned long long)st->st_gid,
	                     .mtime = st->st_mtime > 0 ? (unsigned long long)st->st_mtime : 0u,
	                     .size = size, .link = link };
	static char     list[65536];
	char           *names[256];
	const uint8_t  *values[256];
	size_t          lengths[256];
	uint8_t        *held[256];
	unsigned        count = 0u;
	int             status;
	int             fd = dir >= 0 ? openat(dir, at, O_PATH | O_NOFOLLOW | O_CLOEXEC) : -1;

	if (fd >= 0) {
		char    path[64];
		ssize_t n;

		/* Through the descriptor's magic link, which the l- calls would
		 * take as the link itself; it leads to the file, a symlink too. */
		snprintf(path, sizeof path, "/proc/self/fd/%d", fd);
		n = listxattr(path, list, sizeof list);
		for (ssize_t i = 0; n > 0 && i < n && count < 256u; i += (ssize_t)strlen(list + i) + 1)
			if (strncmp(list + i, "user.", 5u) == 0 || strncmp(list + i, "security.", 9u) == 0)
				names[count++] = list + i;
		qsort(names, count, sizeof names[0], name_order);
		for (unsigned i = 0u; i < count; i++) {
			ssize_t v = getxattr(path, names[i], NULL, 0u);

			held[i] = v >= 0 ? malloc((size_t)v + 1u) : NULL;
			lengths[i] = held[i] != NULL && v >= 0 ?
			             (size_t)getxattr(path, names[i], held[i], (size_t)v + 1u) : 0u;
			values[i] = held[i];
		}
		close(fd);
	}
	e.xattr_names   = (const char *const *)names;
	e.xattr_values  = values;
	e.xattr_lengths = lengths;
	e.xattr_count   = count;
	for (unsigned i = 0u; i < count; i++)
		if (held[i] == NULL) {
			for (unsigned k = 0u; k < count; k++)
				free(held[k]);
			return fail(w, "an xattr could not be read");
		}
	status = write_header(w, &e);
	for (unsigned i = 0u; i < count; i++)
		free(held[i]);
	return status;
}

/* A regular file's content, exactly the size its header gave. */
static int content(writer *w, int fd, unsigned long long size)
{
	unsigned long long left = size;

	while (left > 0u) {
		size_t  want = left < sizeof w->buffer ? (size_t)left : sizeof w->buffer;
		ssize_t got = read(fd, w->buffer, want);

		if (got < 0 && errno == EINTR)
			continue;
		if (got < 0)
			return fail(w, strerror(errno));
		if (got == 0)
			return fail(w, "it shrank while it was read");
		if (put(w, w->buffer, (size_t)got) != 0)
			return -1;
		left -= (unsigned long long)got;
	}
	if (read(fd, w->buffer, 1u) > 0)
		return fail(w, "it grew while it was read");
	return pad(w, size);
}

static int by_name(const void *a, const void *b)
{
	return strcmp(*(char *const *)a, *(char *const *)b);
}

static int walk(writer *w, int dir, size_t base, unsigned depth);

/* Whether `name` in `dir` is on a mount other than the root's. */
static int elsewhere(const writer *w, int dir, const char *name, const struct stat *st)
{
	struct statx found;

	if (w->mount != 0u &&
	    statx(dir, name, AT_SYMLINK_NOFOLLOW, STATX_MNT_ID, &found) == 0 &&
	    (found.stx_mask & STATX_MNT_ID) != 0u)
		return found.stx_mnt_id != w->mount;
	return st->st_dev != w->device;
}

static int entry(writer *w, int dir, const char *name, size_t base, unsigned depth)
{
	struct stat st;
	size_t      length = strlen(name);
	int         status = 0;

	if (base + length + 2u >= sizeof w->path)
		return fail(w, "a name under it is too long");
	memcpy(w->path + base, name, length + 1u);
	if (fstatat(dir, name, &st, AT_SYMLINK_NOFOLLOW) != 0)
		return fail(w, strerror(errno));

	/* On another mount, under one_fs: a directory is a mount point,
	 * written as itself and not what is mounted on it, as an image keeps
	 * an empty /proc; anything else is a file bound over one, left out. A
	 * bind from the same filesystem keeps its device, so the mount id is
	 * what tells, and the device only where the kernel gives none. */
	if (w->one_fs && elsewhere(w, dir, name, &st)) {
		if (S_ISDIR(st.st_mode)) {
			w->path[base + length]      = '/';
			w->path[base + length + 1u] = '\0';
			status = header(w, -1, NULL, w->path, '5', &st, 0u, NULL);
		}
		w->path[base] = '\0';
		return status;
	}
	if (S_ISDIR(st.st_mode)) {
		int fd;

		if (depth == MAX_DEPTH)
			return fail(w, "the tree is too deep");
		fd = openat(dir, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
		if (fd < 0)
			return fail(w, strerror(errno));
		w->path[base + length]      = '/';
		w->path[base + length + 1u] = '\0';
		status = header(w, dir, name, w->path, '5', &st, 0u, NULL);
		if (status == 0)
			status = walk(w, fd, base + length + 1u, depth + 1u);
		close(fd);
	} else if (S_ISREG(st.st_mode)) {
		struct stat opened;
		int         fd = openat(dir, name, O_RDONLY | O_NOFOLLOW | O_NOCTTY | O_CLOEXEC);

		if (fd < 0)
			return fail(w, strerror(errno));
		if (fstat(fd, &opened) != 0 || !S_ISREG(opened.st_mode) || opened.st_ino != st.st_ino)
			status = fail(w, "it changed while it was read");
		else
			status = header(w, dir, name, w->path, '0', &opened, (unsigned long long)opened.st_size,
			                NULL) != 0 ? -1 : content(w, fd, (unsigned long long)opened.st_size);
		close(fd);
	} else if (S_ISLNK(st.st_mode)) {
		char    target[MAX_PATH];
		ssize_t n = readlinkat(dir, name, target, sizeof target - 1u);

		if (n < 0)
			return fail(w, strerror(errno));
		if ((size_t)n == sizeof target - 1u)
			return fail(w, "its target is too long");
		target[n] = '\0';
		status = header(w, dir, name, w->path, '2', &st, 0u, target);
	} else if (S_ISFIFO(st.st_mode)) {
		status = header(w, dir, name, w->path, '6', &st, 0u, NULL);
	} else if (S_ISCHR(st.st_mode) || S_ISBLK(st.st_mode)) {
		status = fail(w, "a device node is not imported");
	}
	/* A socket is no file's content, and is left out. */
	w->path[base] = '\0';
	return status;
}

static int walk(writer *w, int dir, size_t base, unsigned depth)
{
	DIR           *listing;
	struct dirent *found;
	char         **names = NULL;
	size_t         count = 0u;
	size_t         cap = 0u;
	int            status = 0;
	int            copy = dup(dir);

	if (copy < 0 || (listing = fdopendir(copy)) == NULL) {
		if (copy >= 0)
			close(copy);
		return fail(w, strerror(errno));
	}
	while ((found = readdir(listing)) != NULL) {
		if (strcmp(found->d_name, ".") == 0 || strcmp(found->d_name, "..") == 0)
			continue;
		if (count == cap) {
			char **grown = realloc(names, (cap = cap != 0u ? 2u * cap : 64u) * sizeof *names);

			if (grown == NULL) {
				status = fail(w, "out of memory");
				break;
			}
			names = grown;
		}
		if ((names[count] = strdup(found->d_name)) == NULL) {
			status = fail(w, "out of memory");
			break;
		}
		count++;
	}
	closedir(listing);
	if (status == 0 && count > 1u)
		qsort(names, count, sizeof *names, by_name);
	for (size_t i = 0u; i < count && status == 0; i++)
		status = entry(w, dir, names[i], base, depth);
	for (size_t i = 0u; i < count; i++)
		free(names[i]);
	free(names);
	return status;
}

int hull_tree_tar(const char *root, hull_tree_sink sink, void *context, char *detail,
                  size_t detail_cap)
{
	return hull_tree_tar_fs(root, 0, sink, context, detail, detail_cap);
}

int hull_tree_tar_fs(const char *root, int one_fs, hull_tree_sink sink, void *context,
                     char *detail, size_t detail_cap)
{
	static const uint8_t end[2u * BLOCK];
	writer              *w = calloc(1u, sizeof *w);
	struct stat          top;
	int                  dir;
	int                  status;

	if (w == NULL) {
		snprintf(detail, detail_cap, "out of memory");
		return -1;
	}
	w->sink       = sink;
	w->context    = context;
	w->detail     = detail;
	w->detail_cap = detail_cap;
	dir = open(root, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	if (dir < 0 || fstat(dir, &top) != 0) {
		snprintf(detail, detail_cap, "%s: %s", root, strerror(errno));
		if (dir >= 0)
			close(dir);
		free(w);
		return -1;
	}
	w->one_fs = one_fs;
	w->device = top.st_dev;
	{
		struct statx root_mount;

		if (statx(dir, "", AT_EMPTY_PATH, STATX_MNT_ID, &root_mount) == 0 &&
		    (root_mount.stx_mask & STATX_MNT_ID) != 0u)
			w->mount = root_mount.stx_mnt_id;
	}
	status = walk(w, dir, 0u, 0u);
	if (status == 0)
		status = put(w, end, sizeof end);
	close(dir);
	free(w);
	return status;
}

/* --- an entry at a time ---------------------------------------------------- */

/* A writer over `sink` for one call, `w->path` empty: what fails names the
 * entry itself. */
static void bare_writer(writer *w, hull_tree_sink sink, void *context, char *detail,
                        size_t detail_cap)
{
	w->sink       = sink;
	w->context    = context;
	w->detail     = detail;
	w->detail_cap = detail_cap;
	w->path[0]    = '\0';
}

int hull_tar_header(hull_tree_sink sink, void *context, const hull_tar_entry *e, char *detail,
                    size_t detail_cap)
{
	static writer w;

	bare_writer(&w, sink, context, detail, detail_cap);
	snprintf(w.path, sizeof w.path, "%s", e->name);
	return write_header(&w, e);
}

int hull_tar_pad(hull_tree_sink sink, void *context, unsigned long long n, char *detail,
                 size_t detail_cap)
{
	static writer w;

	bare_writer(&w, sink, context, detail, detail_cap);
	return pad(&w, n);
}

int hull_tar_end(hull_tree_sink sink, void *context, char *detail, size_t detail_cap)
{
	static const uint8_t end[2u * BLOCK];
	static writer        w;

	bare_writer(&w, sink, context, detail, detail_cap);
	return put(&w, end, sizeof end);
}
