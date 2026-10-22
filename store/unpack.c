/*
 * unpack.c -- OCI layers applied into a tree, as `hull pull` builds one to
 * commit (section 3.5). Each layer is a tar, read through zlib, which
 * passes one that is not compressed through as it is. See store.h.
 *
 * Every parent is opened with openat2() and RESOLVE_IN_ROOT, so that a
 * symlink a layer made, `/lib` to `usr/lib` say, is followed as the tree's
 * own and an absolute or `..` one cannot leave it; the entry itself is
 * made with the *at calls, following nothing.
 */

#define _GNU_SOURCE

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/openat2.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/xattr.h>
#include <unistd.h>
#include <zlib.h>

#include "store.h"

#define BLOCK   512u
#define MAX_PAX (1u << 20)

typedef struct dir_mode {
	char  *path;
	mode_t mode;
} dir_mode;

struct hull_unpack {
	int       root;
	dir_mode *dirs;
	size_t    dir_count;
	char    **made;       /* paths this layer made, for an opaque whiteout */
	size_t    made_count;
};

static int say(char *why, size_t cap, const char *what, const char *detail)
{
	if (detail != NULL)
		snprintf(why, cap, "%s: %s", what, detail);
	else
		snprintf(why, cap, "%s", what);
	return -1;
}

hull_unpack *hull_unpack_begin(const char *dir, char *why, size_t cap)
{
	hull_unpack *u = calloc(1u, sizeof *u);

	if (u == NULL) {
		say(why, cap, "out of memory", NULL);
		return NULL;
	}
	if ((u->root = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC)) < 0) {
		say(why, cap, dir, strerror(errno));
		free(u);
		return NULL;
	}
	return u;
}

/* --- the tree's paths --------------------------------------------------- */

/* `dir`, a path in the tree or "" for its root, opened as a directory the
 * tree's symlinks resolve in. */
static int open_in(const hull_unpack *u, const char *dir, int flags)
{
	struct open_how how = { .flags = (uint64_t)(unsigned)(flags | O_DIRECTORY | O_CLOEXEC),
	                        .resolve = RESOLVE_IN_ROOT | RESOLVE_NO_MAGICLINKS };

	return (int)syscall(SYS_openat2, u->root, dir[0] != '\0' ? dir : ".", &how, sizeof how);
}

/* A tar's name as a path in the tree: `./` and `/` off its front, `/` off
 * its end; NULL for one with a `.` or `..` part, or the root itself. */
static char *tree_path(const char *name)
{
	char  *path;
	size_t n;

	while (name[0] == '/' || (name[0] == '.' && name[1] == '/'))
		name += name[0] == '/' ? 1 : 2;
	path = strdup(name);
	if (path == NULL)
		return NULL;
	n = strlen(path);
	while (n > 0u && path[n - 1u] == '/')
		path[--n] = '\0';
	for (char *part = path; n > 0u; ) {
		char  *slash = strchr(part, '/');
		size_t length = slash != NULL ? (size_t)(slash - part) : strlen(part);

		if (length == 0u || (length == 1u && part[0] == '.') ||
		    (length == 2u && part[0] == '.' && part[1] == '.'))
			break;
		if (slash == NULL)
			return path;
		part = slash + 1;
	}
	free(path);
	return NULL;
}

static void set_mode(hull_unpack *u, const char *path, mode_t mode)
{
	dir_mode *grown;

	for (size_t i = 0u; i < u->dir_count; i++)
		if (strcmp(u->dirs[i].path, path) == 0) {
			u->dirs[i].mode = mode;
			return;
		}
	if ((grown = realloc(u->dirs, (u->dir_count + 1u) * sizeof *grown)) == NULL)
		return;
	u->dirs = grown;
	if ((u->dirs[u->dir_count].path = strdup(path)) != NULL)
		u->dirs[u->dir_count++].mode = mode;
}

static void note_made(hull_unpack *u, const char *path)
{
	char **grown = realloc(u->made, (u->made_count + 1u) * sizeof *grown);

	if (grown == NULL)
		return;
	u->made = grown;
	if ((u->made[u->made_count] = strdup(path)) != NULL)
		u->made_count++;
}

/* Whether this layer made `path` or anything beneath it. */
static int made_here(const hull_unpack *u, const char *path)
{
	size_t n = strlen(path);

	for (size_t i = 0u; i < u->made_count; i++)
		if (strncmp(u->made[i], path, n) == 0 && (u->made[i][n] == '\0' || u->made[i][n] == '/'))
			return 1;
	return 0;
}

/* The parent of `path` opened, made, with its own parents, where a layer
 * names a path before its directory, as a layer may. */
static int parent(hull_unpack *u, char *path, const char **leaf, char *why, size_t cap)
{
	char *slash = strrchr(path, '/');
	int   fd;

	if (slash == NULL) {
		*leaf = path;
		return open_in(u, "", O_PATH);
	}
	*slash = '\0';
	*leaf = slash + 1;
	if ((fd = open_in(u, path, O_PATH)) < 0 && errno == ENOENT) {
		/* Each missing directory made in turn, from the root down: `path`
		 * cut at each slash is the next one, made in the one above it. */
		for (char *at = path + 1;; at++) {
			char        saved;
			char       *last;
			const char *name = path;
			int         above;

			if (*at != '/' && *at != '\0')
				continue;
			saved = *at;
			*at = '\0';
			last = strrchr(path, '/');
			if (last != NULL) {
				*last = '\0';
				above = open_in(u, path, O_PATH);
				*last = '/';
				name = last + 1;
			} else {
				above = open_in(u, "", O_PATH);
			}
			if (above >= 0) {
				if (mkdirat(above, name, 0700) == 0) {
					set_mode(u, path, 0755);
					note_made(u, path);
				}
				close(above);
			}
			*at = saved;
			if (saved == '\0')
				break;
		}
		fd = open_in(u, path, O_PATH);
	}
	if (fd < 0)
		say(why, cap, path, errno == ENOTDIR ? "a layer makes a path beneath a file" :
		                                       strerror(errno));
	*slash = '/';
	return fd;
}

/* Whatever is at `leaf` taken away, a directory with all it holds, unless
 * `keep_dir` and it is one. */
static void clear(int dir, const char *leaf, int keep_dir)
{
	struct stat st;

	if (fstatat(dir, leaf, &st, AT_SYMLINK_NOFOLLOW) != 0)
		return;
	if (S_ISDIR(st.st_mode)) {
		if (!keep_dir)
			hull_store_remove_tree(dir, leaf);
	} else {
		unlinkat(dir, leaf, 0);
	}
}

/* --- reading the tar ---------------------------------------------------- */

typedef struct entry {
	char              *name;      /* the whole of it, pax's or GNU's where given */
	char              *link;
	unsigned long long size;
	int                has_size;  /* pax gave the size */
	char               type;
	mode_t             mode;
	char             **xattr_names;
	uint8_t          **xattr_values;
	size_t            *xattr_lengths;
	size_t             xattr_count;
} entry;

static void free_entry(entry *e)
{
	free(e->name);
	free(e->link);
	for (size_t i = 0u; i < e->xattr_count; i++) {
		free(e->xattr_names[i]);
		free(e->xattr_values[i]);
	}
	free(e->xattr_names);
	free(e->xattr_values);
	free(e->xattr_lengths);
	memset(e, 0, sizeof *e);
}

static int read_exact(gzFile gz, void *out, size_t n)
{
	uint8_t *at = out;

	while (n > 0u) {
		int got = gzread(gz, at, n > (1u << 30) ? 1u << 30 : (unsigned)n);

		if (got <= 0)
			return -1;
		at += got;
		n -= (size_t)got;
	}
	return 0;
}

static int skip(gzFile gz, unsigned long long n)
{
	uint8_t buffer[16384];

	while (n > 0u) {
		size_t take = n > sizeof buffer ? sizeof buffer : (size_t)n;

		if (read_exact(gz, buffer, take) != 0)
			return -1;
		n -= take;
	}
	return 0;
}

static unsigned long long padding(unsigned long long n)
{
	return (BLOCK - n % BLOCK) % BLOCK;
}

/* A header's number: octal digits, or GNU's base 256 when its first byte
 * has its top bit. -1 for neither. */
static long long number(const uint8_t *field, size_t n)
{
	unsigned long long value = 0u;
	size_t             i = 0u;

	if (field[0] & 0x80u) {
		value = field[0] & 0x7fu;
		for (i = 1u; i < n; i++) {
			if (value >> 55)
				return -1;
			value = value << 8 | field[i];
		}
		return (long long)value;
	}
	while (i < n && field[i] == ' ')
		i++;
	for (; i < n && field[i] >= '0' && field[i] <= '7'; i++) {
		if (value >> 60)
			return -1;
		value = value * 8u + (unsigned)(field[i] - '0');
	}
	if (i < n && field[i] != '\0' && field[i] != ' ')
		return -1;
	return (long long)value;
}

static char *field_text(const uint8_t *field, size_t n)
{
	size_t length = 0u;
	char  *text;

	while (length < n && field[length] != '\0')
		length++;
	if ((text = malloc(length + 1u)) != NULL) {
		memcpy(text, field, length);
		text[length] = '\0';
	}
	return text;
}

/* A pax header's records, `LENGTH KEY=VALUE\n` each, into `e`. */
static int read_pax(const uint8_t *data, size_t n, entry *e)
{
	size_t at = 0u;

	while (at < n) {
		size_t      length = 0u;
		size_t      start = at;
		const char *key;
		const char *equals;
		size_t      key_length;
		size_t      value_length;

		while (at < n && data[at] >= '0' && data[at] <= '9')
			length = length * 10u + (size_t)(data[at++] - '0');
		if (at >= n || data[at] != ' ' || length == 0u || start + length > n ||
		    data[start + length - 1u] != '\n')
			return -1;
		key = (const char *)data + at + 1u;
		equals = memchr(key, '=', start + length - (at + 1u));
		if (equals == NULL)
			return -1;
		key_length = (size_t)(equals - key);
		value_length = start + length - 1u - (size_t)(equals + 1 - (const char *)data);
		if (key_length == 4u && memcmp(key, "path", 4u) == 0) {
			free(e->name);
			e->name = strndup(equals + 1, value_length);
		} else if (key_length == 8u && memcmp(key, "linkpath", 8u) == 0) {
			free(e->link);
			e->link = strndup(equals + 1, value_length);
		} else if (key_length == 4u && memcmp(key, "size", 4u) == 0) {
			char text[32];

			if (value_length == 0u || value_length >= sizeof text)
				return -1;
			memcpy(text, equals + 1, value_length);
			text[value_length] = '\0';
			if (strspn(text, "0123456789") != value_length)
				return -1;
			e->size = strtoull(text, NULL, 10);
			e->has_size = 1;
		} else if (key_length > 12u && memcmp(key, "SCHILY.xattr.", 13u) == 0) {
			size_t    i = e->xattr_count;
			char    **names = realloc(e->xattr_names, (i + 1u) * sizeof *names);
			uint8_t **values = realloc(e->xattr_values, (i + 1u) * sizeof *values);
			size_t   *lengths = realloc(e->xattr_lengths, (i + 1u) * sizeof *lengths);

			if (names != NULL)
				e->xattr_names = names;
			if (values != NULL)
				e->xattr_values = values;
			if (lengths != NULL)
				e->xattr_lengths = lengths;
			if (names == NULL || values == NULL || lengths == NULL)
				return -1;
			names[i] = strndup(key + 13, key_length - 13u);
			values[i] = malloc(value_length + 1u);
			if (names[i] == NULL || values[i] == NULL) {
				free(names[i]);
				free(values[i]);
				return -1;
			}
			memcpy(values[i], equals + 1, value_length);
			lengths[i] = value_length;
			e->xattr_count++;
		}
		at = start + length;
	}
	return 0;
}

/* The next entry's header, with any pax or GNU long-name header before it
 * read into it: 1, 0 at the archive's end, or -1 with `why`. */
static int next_entry(gzFile gz, entry *e, char *why, size_t cap)
{
	uint8_t  block[BLOCK];
	char    *long_name = NULL;
	char    *long_link = NULL;
	int      found = 0;

	free_entry(e);
	for (;;) {
		unsigned long sum = 0u;
		long long     size;
		long long     given;
		char          type;
		int           zero = 1;

		if (read_exact(gz, block, BLOCK) != 0) {
			int         code;
			const char *text = gzerror(gz, &code);

			free(long_name);
			free(long_link);
			return say(why, cap, "the layer ends inside its archive",
			           code != Z_OK ? text : NULL);
		}
		for (unsigned i = 0u; i < BLOCK; i++)
			zero &= block[i] == 0u;
		if (zero) {
			free(long_name);
			free(long_link);
			return 0;
		}
		for (unsigned i = 0u; i < BLOCK; i++)
			sum += i >= 148u && i < 156u ? ' ' : block[i];
		given = number(block + 148, 8u);
		size = number(block + 124, 12u);
		if (given < 0 || (unsigned long)given != sum || size < 0) {
			free(long_name);
			free(long_link);
			return say(why, cap, "a layer's tar header is not one", NULL);
		}
		type = (char)block[156];
		if (type == 'x' || type == 'g' || type == 'L' || type == 'K') {
			uint8_t *data;

			if ((unsigned long long)size > MAX_PAX || (data = malloc((size_t)size + 1u)) == NULL ||
			    read_exact(gz, data, (size_t)size) != 0 || skip(gz, padding((unsigned long long)size)) != 0) {
				free(long_name);
				free(long_link);
				return say(why, cap, "a layer's extended header could not be read", NULL);
			}
			data[size] = 0u;
			if (type == 'x' && read_pax(data, (size_t)size, e) != 0) {
				free(data);
				free(long_name);
				free(long_link);
				return say(why, cap, "a layer's pax header is not one", NULL);
			}
			if (type == 'L') {
				free(long_name);
				long_name = strdup((const char *)data);
			} else if (type == 'K') {
				free(long_link);
				long_link = strdup((const char *)data);
			}
			free(data);
			continue;  /* a 'g' header is the archive's, and says nothing here */
		}
		found = 1;
		e->type = type == '\0' || type == '7' ? '0' : type;
		e->mode = (mode_t)(number(block + 100, 8u) & 07777);
		if (!e->has_size)
			e->size = (unsigned long long)size;
		if (e->name == NULL) {
			if (long_name != NULL) {
				e->name = long_name;
				long_name = NULL;
			} else {
				char *name = field_text(block, 100u);
				char *prefix = memcmp(block + 257, "ustar", 5u) == 0 ? field_text(block + 345, 155u) :
				                                                    strdup("");

				if (name != NULL && prefix != NULL &&
				    (e->name = malloc(strlen(prefix) + strlen(name) + 2u)) != NULL)
					sprintf(e->name, "%s%s%s", prefix, prefix[0] != '\0' ? "/" : "", name);
				free(name);
				free(prefix);
			}
		}
		if (e->link == NULL) {
			if (long_link != NULL) {
				e->link = long_link;
				long_link = NULL;
			} else {
				e->link = field_text(block + 157, 100u);
			}
		}
		free(long_name);
		free(long_link);
		if (e->name == NULL || e->link == NULL)
			return say(why, cap, "out of memory", NULL);
		return found;
	}
}

/* --- applying it -------------------------------------------------------- */

/* A file's content from the layer into `fd`. */
static int copy_out(gzFile gz, int fd, unsigned long long n)
{
	uint8_t buffer[65536];

	while (n > 0u) {
		size_t   take = n > sizeof buffer ? sizeof buffer : (size_t)n;
		uint8_t *at = buffer;
		size_t   left = take;

		if (read_exact(gz, buffer, take) != 0)
			return -1;
		while (left > 0u) {
			ssize_t wrote = write(fd, at, left);

			if (wrote < 0 && errno == EINTR)
				continue;
			if (wrote <= 0)
				return -2;
			at += wrote;
			left -= (size_t)wrote;
		}
		n -= take;
	}
	return 0;
}

static void apply_xattrs(int dir, const char *leaf, const char *path, const entry *e,
                         uint64_t *skipped)
{
	char at[64];
	int  fd;

	if (e->xattr_count == 0u || (fd = openat(dir, leaf, O_PATH | O_NOFOLLOW | O_CLOEXEC)) < 0)
		return;
	snprintf(at, sizeof at, "/proc/self/fd/%d", fd);
	for (size_t i = 0u; i < e->xattr_count; i++) {
		if (strncmp(e->xattr_names[i], "user.", 5u) != 0 ||
		    setxattr(at, e->xattr_names[i], e->xattr_values[i], e->xattr_lengths[i], 0) != 0) {
			fprintf(stderr, "hull: pull: %s: xattr %s, which the user cannot set, left out\n",
			        path, e->xattr_names[i]);
			(*skipped)++;
		}
	}
	close(fd);
}

/* An opaque whiteout in `dir`: what it holds that this layer did not make
 * goes, being the lower layers'. */
static void opaque(hull_unpack *u, const char *dir)
{
	int            fd = open_in(u, dir, O_RDONLY);
	DIR           *list;
	struct dirent *item;

	if (fd < 0 || (list = fdopendir(fd)) == NULL) {
		if (fd >= 0)
			close(fd);
		return;
	}
	while ((item = readdir(list)) != NULL) {
		char path[PATH_MAX];

		if (strcmp(item->d_name, ".") == 0 || strcmp(item->d_name, "..") == 0)
			continue;
		snprintf(path, sizeof path, "%s%s%s", dir, dir[0] != '\0' ? "/" : "", item->d_name);
		if (!made_here(u, path))
			hull_store_remove_tree(dirfd(list), item->d_name);
	}
	closedir(list);
}

static int apply(hull_unpack *u, gzFile gz, entry *e, uint64_t *skipped, char *why, size_t cap)
{
	char       *path = tree_path(e->name);
	const char *leaf;
	const char *base;
	int         dir = -1;
	int         result = -1;

	if (path == NULL) {
		/* The root itself, `./`, says only its mode, which a tree's root
		 * does not keep; anything else nameless is not a path. */
		if (strspn(e->name, "./") == strlen(e->name) && e->type == '5')
			return skip(gz, e->size + padding(e->size)) == 0 ? 0 :
			       say(why, cap, "the layer ends inside an entry", NULL);
		return say(why, cap, e->name, "a layer's path with `.` or `..` in it, refused");
	}
	base = strrchr(path, '/');
	base = base != NULL ? base + 1 : path;
	if (strncmp(base, ".wh.", 4u) == 0) {
		char *slash = strrchr(path, '/');

		if (slash != NULL)
			*slash = '\0';
		if (strcmp(base, ".wh..wh..opq") == 0) {
			opaque(u, slash != NULL ? path : "");
		} else if ((dir = open_in(u, slash != NULL ? path : "", O_PATH)) >= 0) {
			hull_store_remove_tree(dir, base + 4);
		}
		result = skip(gz, e->size + padding(e->size)) == 0 ? 0 :
		         say(why, cap, "the layer ends inside an entry", NULL);
		goto done;
	}
	if ((dir = parent(u, path, &leaf, why, cap)) < 0)
		goto done;
	clear(dir, leaf, e->type == '5');
	switch (e->type) {
	case '0': {
		int fd = openat(dir, leaf, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
		int copied;

		if (fd < 0) {
			say(why, cap, path, strerror(errno));
			goto done;
		}
		copied = copy_out(gz, fd, e->size);
		if (copied != 0 || fchmod(fd, e->mode) != 0) {
			say(why, cap, path, copied == -1 ? "the layer ends inside the file" : strerror(errno));
			close(fd);
			goto done;
		}
		close(fd);
		if (skip(gz, padding(e->size)) != 0) {
			say(why, cap, "the layer ends inside an entry", NULL);
			goto done;
		}
		break;
	}
	case '5':
		if (mkdirat(dir, leaf, 0700) != 0 && errno != EEXIST) {
			say(why, cap, path, strerror(errno));
			goto done;
		}
		set_mode(u, path, e->mode);
		break;
	case '2':
		if (symlinkat(e->link, dir, leaf) != 0) {
			say(why, cap, path, strerror(errno));
			goto done;
		}
		break;
	case '1': {
		char       *target = tree_path(e->link);
		const char *target_leaf;
		int         target_dir = target != NULL ? parent(u, target, &target_leaf, why, cap) : -1;
		int         linked = target_dir >= 0 && linkat(target_dir, target_leaf, dir, leaf, 0) == 0;

		if (target_dir >= 0)
			close(target_dir);
		free(target);
		if (!linked) {
			say(why, cap, path, "a hardlink to nothing the layers have made, refused");
			goto done;
		}
		break;
	}
	case '6':
		if (mkfifoat(dir, leaf, e->mode) != 0) {
			say(why, cap, path, strerror(errno));
			goto done;
		}
		break;
	case '3':
	case '4':
		fprintf(stderr, "hull: pull: %s: a device, which is not made rootless, left out\n", path);
		(*skipped)++;
		break;
	default:
		fprintf(stderr, "hull: pull: %s: a tar entry of type %c, left out\n", path, e->type);
		(*skipped)++;
		break;
	}
	if (e->type != '3' && e->type != '4')
		apply_xattrs(dir, leaf, path, e, skipped);
	note_made(u, path);
	result = 0;
	/* A file read its own data; any other entry's, a hardlink's say, is
	 * nothing it uses. */
	if (e->type != '0' && skip(gz, e->size + padding(e->size)) != 0)
		result = say(why, cap, "the layer ends inside an entry", NULL);
done:
	if (dir >= 0)
		close(dir);
	free(path);
	return result;
}

int hull_unpack_layer(hull_unpack *u, int fd, uint64_t *skipped, char *why, size_t cap)
{
	gzFile gz;
	entry  e = { 0 };
	int    status;
	int    copy = dup(fd);

	if (copy < 0 || lseek(copy, 0, SEEK_SET) != 0 || (gz = gzdopen(copy, "rb")) == NULL) {
		if (copy >= 0)
			close(copy);
		return say(why, cap, "the layer could not be read", NULL);
	}
	gzbuffer(gz, 1u << 17);
	for (size_t i = 0u; i < u->made_count; i++)
		free(u->made[i]);
	u->made_count = 0u;
	while ((status = next_entry(gz, &e, why, cap)) == 1)
		if (apply(u, gz, &e, skipped, why, cap) != 0) {
			status = -1;
			break;
		}
	free_entry(&e);
	gzclose_r(gz);
	return status < 0 ? -1 : 0;
}

static int depth_order(const void *a, const void *b)
{
	const char *pa = ((const dir_mode *)a)->path;
	const char *pb = ((const dir_mode *)b)->path;
	size_t      da = 0u;
	size_t      db = 0u;

	for (; *pa != '\0'; pa++)
		da += *pa == '/';
	for (; *pb != '\0'; pb++)
		db += *pb == '/';
	return da < db ? 1 : da > db ? -1 : 0;
}

int hull_unpack_end(hull_unpack *u, char *why, size_t cap)
{
	int result = 0;

	if (u == NULL)
		return 0;
	/* Deepest first, so that no directory's mode stops one beneath it. A
	 * directory a later layer took away, or made something else, is
	 * passed over. */
	qsort(u->dirs, u->dir_count, sizeof u->dirs[0], depth_order);
	for (size_t i = 0u; i < u->dir_count; i++) {
		int fd = open_in(u, u->dirs[i].path, O_RDONLY | O_NOFOLLOW);

		if (fd >= 0) {
			if (fchmod(fd, u->dirs[i].mode) != 0 && result == 0)
				result = say(why, cap, u->dirs[i].path, strerror(errno));
			close(fd);
		}
		free(u->dirs[i].path);
	}
	for (size_t i = 0u; i < u->made_count; i++)
		free(u->made[i]);
	free(u->dirs);
	free(u->made);
	close(u->root);
	free(u);
	return result;
}
