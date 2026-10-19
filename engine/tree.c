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

/* One pax record, "<length> <key>=<value>\n", the length counting itself. */
static size_t record(char *out, size_t cap, size_t at, const char *key, const char *value)
{
	size_t body = strlen(key) + strlen(value) + 3u;  /* " ", "=" and "\n" */
	size_t length = body + 1u;
	char   digits[24];

	/* The length's own digits, until it stops growing. */
	while ((size_t)snprintf(digits, sizeof digits, "%zu", length) + body != length)
		length = (size_t)snprintf(digits, sizeof digits, "%zu", length) + body;
	if (at + length >= cap)
		return cap;
	snprintf(out + at, cap - at, "%zu %s=%s\n", length, key, value);
	return at + length;
}

static void octal(uint8_t *field, size_t width, unsigned long long value)
{
	snprintf((char *)field, width, "%0*llo", (int)(width - 1u), value);
}

static int header(writer *w, const char *name, char type, const struct stat *st,
                  unsigned long long size, const char *link)
{
	uint8_t            block[BLOCK];
	char               records[3u * MAX_PATH];
	size_t             at = 0u;
	unsigned long long mtime = st->st_mtime > 0 ? (unsigned long long)st->st_mtime : 0u;
	unsigned           sum = 0u;
	char               number[24];

	if (strlen(name) > 100u)
		at = record(records, sizeof records, at, "path", name);
	if (link != NULL && strlen(link) > 100u)
		at = record(records, sizeof records, at, "linkpath", link);
	if ((unsigned long long)st->st_uid > 07777777u) {
		snprintf(number, sizeof number, "%u", (unsigned)st->st_uid);
		at = record(records, sizeof records, at, "uid", number);
	}
	if ((unsigned long long)st->st_gid > 07777777u) {
		snprintf(number, sizeof number, "%u", (unsigned)st->st_gid);
		at = record(records, sizeof records, at, "gid", number);
	}
	if (size > 077777777777ull) {
		snprintf(number, sizeof number, "%llu", size);
		at = record(records, sizeof records, at, "size", number);
	}
	if (at == sizeof records)
		return fail(w, "its name is too long");
	if (at != 0u) {
		struct stat pax = *st;

		pax.st_mode = 0644;
		pax.st_uid  = 0;
		pax.st_gid  = 0;
		if (header(w, "././@PaxHeader", 'x', &pax, at, NULL) != 0 ||
		    put(w, records, at) != 0 || pad(w, at) != 0)
			return -1;
	}

	memset(block, 0, sizeof block);
	memcpy(block, name, strlen(name) > 100u ? 100u : strlen(name));
	octal(block + 100, 8u, (unsigned long long)(st->st_mode & 07777));
	octal(block + 108, 8u, (unsigned long long)st->st_uid & 07777777u);
	octal(block + 116, 8u, (unsigned long long)st->st_gid & 07777777u);
	octal(block + 124, 12u, size > 077777777777ull ? 0u : size);
	octal(block + 136, 12u, mtime > 077777777777ull ? 077777777777ull : mtime);
	block[156] = (uint8_t)type;
	if (link != NULL)
		memcpy(block + 157, link, strlen(link) > 100u ? 100u : strlen(link));
	memcpy(block + 257, "ustar", 6u);
	memcpy(block + 263, "00", 2u);
	memset(block + 148, ' ', 8u);
	for (unsigned i = 0u; i < BLOCK; i++)
		sum += block[i];
	snprintf((char *)block + 148, 8u, "%06o", sum);
	block[155] = ' ';
	return put(w, block, sizeof block);
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

	if (S_ISDIR(st.st_mode)) {
		int fd;

		if (depth == MAX_DEPTH)
			return fail(w, "the tree is too deep");
		fd = openat(dir, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
		if (fd < 0)
			return fail(w, strerror(errno));
		w->path[base + length]      = '/';
		w->path[base + length + 1u] = '\0';
		status = header(w, w->path, '5', &st, 0u, NULL);
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
			status = header(w, w->path, '0', &opened, (unsigned long long)opened.st_size,
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
		status = header(w, w->path, '2', &st, 0u, target);
	} else if (S_ISFIFO(st.st_mode)) {
		status = header(w, w->path, '6', &st, 0u, NULL);
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
	if (status == 0)
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
	static const uint8_t end[2u * BLOCK];
	writer              *w = calloc(1u, sizeof *w);
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
	if (dir < 0) {
		snprintf(detail, detail_cap, "%s: %s", root, strerror(errno));
		free(w);
		return -1;
	}
	status = walk(w, dir, 0u, 0u);
	if (status == 0)
		status = put(w, end, sizeof end);
	close(dir);
	free(w);
	return status;
}
