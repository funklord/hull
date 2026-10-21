/*
 * store.c -- section 3's store: committing a directory to objects and a
 * manifest, materializing a manifest back to a directory, refs, and the
 * reachability gc of 3.4.
 *
 * A manifest is Appendix A's canonical s-expression, written here byte for
 * byte as manifest/manifest.situ states it: entries sorted by path,
 * bytewise, each entry's keys in canonical order, no mtimes. Its identity
 * is the sha256 of those bytes, which is its file's name, so a manifest
 * read back is checked against its name, and an object against its own.
 *
 * What a commit decides that Appendix A leaves open, recorded in
 * project.md section 3.1:
 * - owner: a file the invoking user owns is `root`, as a dir source's
 *   files are inside its machine (4.8, 4.11); any other is explicit.
 * - a hardlink set is committed as `reg` entries, one per path, sharing
 *   their object: A.2's `hardlink` kind has a group and no object, and
 *   which member carries the content is not settled.
 * - stats: file_count is the entries, object_count the distinct objects,
 *   total_size the sum of the regular files' sizes.
 *
 * Materializing makes each path under the target with every parent
 * resolved beneath it and through no symlink (openat2), so no manifest
 * can write outside it.
 */

#define _GNU_SOURCE

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/openat2.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/sysmacros.h>
#include <sys/xattr.h>
#include <unistd.h>

#include "sexpr.h"
#include "sha256.h"
#include "store.h"

/* --- a growing buffer ------------------------------------------------------ */

typedef struct buf {
	uint8_t *data;
	size_t   length;
	size_t   cap;
	int      failed;
} buf;

static void put(buf *b, const void *bytes, size_t n)
{
	if (b->failed)
		return;
	if (b->length + n > b->cap) {
		size_t   cap = b->cap != 0u ? b->cap : 4096u;
		uint8_t *grown;

		while (cap < b->length + n)
			cap *= 2u;
		if ((grown = realloc(b->data, cap)) == NULL) {
			b->failed = 1;
			return;
		}
		b->data = grown;
		b->cap  = cap;
	}
	memcpy(b->data + b->length, bytes, n);
	b->length += n;
}

static void put_text(buf *b, const char *text)
{
	put(b, text, strlen(text));
}

static void put_atom(buf *b, const void *bytes, size_t n)
{
	char prefix[24];

	snprintf(prefix, sizeof prefix, "%zu:", n);
	put_text(b, prefix);
	put(b, bytes, n);
}

static void put_number(buf *b, unsigned long long n)
{
	char digits[24];

	snprintf(digits, sizeof digits, "%llu", n);
	put_atom(b, digits, strlen(digits));
}

/* --- small things ---------------------------------------------------------- */

static int say(char *why, size_t cap, const char *what, const char *detail)
{
	snprintf(why, cap, "%s%s%s", what, detail != NULL ? ": " : "", detail != NULL ? detail : "");
	return -1;
}

static int make_dirs(const char *path)
{
	char  copy[4096];
	char *at;

	snprintf(copy, sizeof copy, "%s", path);
	for (at = copy + 1; *at != '\0'; at++)
		if (*at == '/') {
			*at = '\0';
			if (mkdir(copy, 0700) != 0 && errno != EEXIST)
				return -1;
			*at = '/';
		}
	return mkdir(copy, 0700) != 0 && errno != EEXIST ? -1 : 0;
}

static void hex_of(const uint8_t digest[32], char out[65])
{
	hull_sha256_hex(digest, out);
}

/* Valid UTF-8, as A.2 has a path be. Normalization to NFC is not checked. */
static int is_utf8(const char *text)
{
	const unsigned char *p = (const unsigned char *)text;

	while (*p != '\0') {
		unsigned extra = *p < 0x80u ? 0u : (*p >> 5) == 6u ? 1u : (*p >> 4) == 14u ? 2u :
		                 (*p >> 3) == 30u ? 3u : 4u;

		if (extra == 4u)
			return 0;
		for (unsigned i = 1u; i <= extra; i++)
			if ((p[i] & 0xc0u) != 0x80u)
				return 0;
		p += 1u + extra;
	}
	return 1;
}

/* A ref names a file under refs/: components of letters, digits and
 * `._-:@`, none `.` or `..`, joined by `/`. */
static int is_ref(const char *ref)
{
	const char *start = ref;

	/* A manifest is named sha256:HEX where a ref is, so no ref is. */
	if (ref[0] == '\0' || strlen(ref) > 255u || strncmp(ref, "sha256:", 7u) == 0)
		return 0;
	for (const char *p = ref;; p++) {
		if (*p == '/' || *p == '\0') {
			size_t n = (size_t)(p - start);

			if (n == 0u || (n == 1u && start[0] == '.') ||
			    (n == 2u && start[0] == '.' && start[1] == '.'))
				return 0;
			if (*p == '\0')
				return 1;
			start = p + 1;
		} else if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
		             (*p >= '0' && *p <= '9') || strchr("._-:@", *p) != NULL)) {
			return 0;
		}
	}
}

/* --- opening -------------------------------------------------------------- */

int hull_store_open(hull_store *store, int create, char *why, size_t cap)
{
	const char *data = getenv("XDG_DATA_HOME");
	const char *home = getenv("HOME");
	char        path[4200];
	static const char *const parts[] = { "objects/sha256", "manifests/sha256", "refs", "tmp" };

	store->lock = -1;
	if (data != NULL && data[0] == '/')
		snprintf(store->root, sizeof store->root, "%s/hull/store", data);
	else if (home != NULL && home[0] == '/')
		snprintf(store->root, sizeof store->root, "%s/.local/share/hull/store", home);
	else
		return say(why, cap, "neither XDG_DATA_HOME nor HOME says where the store is", NULL);
	if (!create && access(store->root, F_OK) != 0)
		return 1;
	for (unsigned i = 0u; i < sizeof parts / sizeof parts[0]; i++) {
		snprintf(path, sizeof path, "%s/%s", store->root, parts[i]);
		if (make_dirs(path) != 0)
			return say(why, cap, path, strerror(errno));
	}
	snprintf(path, sizeof path, "%s/lock", store->root);
	store->lock = open(path, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
	if (store->lock < 0)
		return say(why, cap, path, strerror(errno));
	return 0;
}

void hull_store_close(hull_store *store)
{
	if (store->lock >= 0)
		close(store->lock);
	store->lock = -1;
}

/* --- committing ---------------------------------------------------------- */

typedef struct item {
	char        *path;
	struct stat  st;
	char        *target;      /* a symlink's */
	uint8_t      digest[32];  /* a regular file's object */
	buf          xattrs;      /* `(6:xattrs ...)`, or empty */
} item;

typedef struct walk {
	item   *items;
	size_t  count;
	size_t  cap;
	uint64_t skipped;
	int     failed;
	char   *why;
	size_t  why_cap;
} walk;

static item *add_item(walk *w)
{
	if (w->count == w->cap) {
		size_t cap   = w->cap != 0u ? 2u * w->cap : 256u;
		item  *grown = realloc(w->items, cap * sizeof *grown);

		if (grown == NULL)
			return NULL;
		w->items = grown;
		w->cap   = cap;
	}
	memset(&w->items[w->count], 0, sizeof w->items[0]);
	return &w->items[w->count++];
}

/* user.* and security.* only, sorted by name, as A.2 has them. */
static int name_order(const void *a, const void *b)
{
	return strcmp(*(char *const *)a, *(char *const *)b);
}

static void read_xattrs(int dir, const char *name, buf *out)
{
	char    list[65536];
	char   *names[512];
	char    path[64];
	ssize_t n;
	unsigned count = 0u;
	int     fd = openat(dir, name, O_PATH | O_NOFOLLOW | O_CLOEXEC);

	if (fd < 0)
		return;
	snprintf(path, sizeof path, "/proc/self/fd/%d", fd);
	/* Through the descriptor's magic link, which the l- calls would take
	 * as the link itself; it leads to the file, a symlink included. */
	n = listxattr(path, list, sizeof list);
	for (ssize_t at = 0; n > 0 && at < n && count < 512u; at += (ssize_t)strlen(list + at) + 1)
		if (strncmp(list + at, "user.", 5u) == 0 || strncmp(list + at, "security.", 9u) == 0)
			names[count++] = list + at;
	qsort(names, count, sizeof names[0], name_order);
	for (unsigned i = 0u; i < count; i++) {
		static uint8_t value[65536];
		ssize_t        v = getxattr(path, names[i], value, sizeof value);

		if (v < 0)
			continue;
		if (out->length == 0u)
			put_text(out, "(6:xattrs");
		put_text(out, "(");
		put_atom(out, names[i], strlen(names[i]));
		put_atom(out, value, (size_t)v);
		put_text(out, ")");
	}
	if (out->length != 0u)
		put_text(out, ")");
	close(fd);
}

/* The file's content into the objects: hashed while copied to tmp/, then
 * linked in under its name, unless the store has it. */
static int store_object(hull_store *store, int fd, const struct stat *st, uint8_t digest[32],
                        int *made, char *why, size_t cap)
{
	static uint8_t chunk[1 << 16];
	hull_sha256    h;
	char           temp[4300];
	char           final[4300];
	char           hex[65];
	int            out;
	ssize_t        n;

	snprintf(temp, sizeof temp, "%s/tmp/object.XXXXXX", store->root);
	out = mkostemp(temp, O_CLOEXEC);
	if (out < 0)
		return say(why, cap, temp, strerror(errno));
	hull_sha256_init(&h);
	while ((n = read(fd, chunk, sizeof chunk)) > 0) {
		hull_sha256_update(&h, chunk, (size_t)n);
		if (write(out, chunk, (size_t)n) != n) {
			n = -1;
			break;
		}
	}
	hull_sha256_final(&h, digest);
	if (n < 0 || fchmod(out, (st->st_mode & 0111) != 0 ? 0555 : 0444) != 0 || close(out) != 0) {
		unlink(temp);
		return say(why, cap, "an object could not be written", strerror(errno));
	}
	hex_of(digest, hex);
	snprintf(final, sizeof final, "%s/objects/sha256/%.2s", store->root, hex);
	mkdir(final, 0700);
	snprintf(final, sizeof final, "%s/objects/sha256/%.2s/%s", store->root, hex, hex);
	*made = 0;
	if (link(temp, final) == 0)
		*made = 1;
	else if (errno != EEXIST) {
		unlink(temp);
		return say(why, cap, final, strerror(errno));
	}
	unlink(temp);
	return 0;
}

static int walk_dir(hull_store *store, walk *w, int dir, const char *prefix, hull_commit *out);

static void walk_item(hull_store *store, walk *w, int dir, const char *prefix, const char *name,
                      hull_commit *out)
{
	char        path[PATH_MAX];
	struct stat st;
	item       *it;

	snprintf(path, sizeof path, "%s%s%s", prefix, prefix[0] != '\0' ? "/" : "", name);
	if (fstatat(dir, name, &st, AT_SYMLINK_NOFOLLOW) != 0) {
		fprintf(stderr, "hull: commit: %s: %s, left out\n", path, strerror(errno));
		w->skipped++;
		return;
	}
	if (!is_utf8(path) || S_ISSOCK(st.st_mode)) {
		fprintf(stderr, "hull: commit: %s: %s, left out\n", path,
		        S_ISSOCK(st.st_mode) ? "a socket, which a manifest cannot hold" :
		                               "not UTF-8, which a manifest's path must be");
		w->skipped++;
		return;
	}
	if ((it = add_item(w)) == NULL || (it->path = strdup(path)) == NULL) {
		w->failed = 1;
		return;
	}
	it->st = st;
	read_xattrs(dir, name, &it->xattrs);
	if (S_ISLNK(st.st_mode)) {
		char    target[PATH_MAX];
		ssize_t n = readlinkat(dir, name, target, sizeof target - 1u);

		if (n < 0) {
			w->failed = 1;
			return;
		}
		target[n] = '\0';
		it->target = strdup(target);
	} else if (S_ISREG(st.st_mode)) {
		int fd = openat(dir, name, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
		int made = 0;

		if (fd < 0 || store_object(store, fd, &st, it->digest, &made, w->why, w->why_cap) != 0) {
			if (fd < 0)
				say(w->why, w->why_cap, path, strerror(errno));
			if (fd >= 0)
				close(fd);
			w->failed = 1;
			return;
		}
		close(fd);
		out->new_objects += (uint64_t)made;
	} else if (S_ISDIR(st.st_mode)) {
		int sub = openat(dir, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);

		if (sub < 0) {
			say(w->why, w->why_cap, path, strerror(errno));
			w->failed = 1;
			return;
		}
		walk_dir(store, w, sub, path, out);
	}
}

/* Each entry of `dir`, which this closes. */
static int walk_dir(hull_store *store, walk *w, int dir, const char *prefix, hull_commit *out)
{
	DIR           *list = fdopendir(dir);
	struct dirent *entry;

	if (list == NULL) {
		close(dir);
		w->failed = 1;
		return -1;
	}
	while (!w->failed && (entry = readdir(list)) != NULL)
		if (strcmp(entry->d_name, ".") != 0 && strcmp(entry->d_name, "..") != 0)
			walk_item(store, w, dirfd(list), prefix, entry->d_name, out);
	closedir(list);
	return w->failed ? -1 : 0;
}

static int path_order(const void *a, const void *b)
{
	return strcmp(((const item *)a)->path, ((const item *)b)->path);
}

static int digest_order(const void *a, const void *b)
{
	return memcmp(a, b, 32u);
}

/* One entry's canonical bytes, its keys in A.2's order. */
static void put_entry(buf *b, const item *it)
{
	mode_t type = it->st.st_mode & S_IFMT;

	put_text(b, "(5:entry");
	if (type == S_IFCHR || type == S_IFBLK) {
		put_text(b, "(3:dev(5:major");
		put_number(b, major(it->st.st_rdev));
		put_text(b, ")(5:minor");
		put_number(b, minor(it->st.st_rdev));
		put_text(b, type == S_IFCHR ? ")(4:type1:c))" : ")(4:type1:b))");
	}
	put_text(b, "(4:kind");
	put_text(b, type == S_IFREG ? "3:reg" : type == S_IFDIR ? "3:dir" : type == S_IFLNK ? "7:symlink" :
	            type == S_IFIFO ? "4:fifo" : "3:dev");
	put_text(b, ")(4:mode");
	put_number(b, it->st.st_mode & 07777);
	put_text(b, ")");
	if (type == S_IFREG) {
		put_text(b, "(6:object");
		put_atom(b, it->digest, 32u);
		put_text(b, ")");
	}
	put_text(b, "(5:owner");
	if (it->st.st_uid == getuid()) {
		put_text(b, "4:root");
	} else {
		put_text(b, "(3:gid");
		put_number(b, it->st.st_gid);
		put_text(b, ")(3:uid");
		put_number(b, it->st.st_uid);
		put_text(b, ")");
	}
	put_text(b, ")(4:path");
	put_atom(b, it->path, strlen(it->path));
	put_text(b, ")");
	if (type == S_IFREG) {
		put_text(b, "(4:size");
		put_number(b, (unsigned long long)it->st.st_size);
		put_text(b, ")");
	}
	if (type == S_IFLNK) {
		put_text(b, "(6:target");
		put_atom(b, it->target, strlen(it->target));
		put_text(b, ")");
	}
	if (it->xattrs.length != 0u)
		put(b, it->xattrs.data, it->xattrs.length);
	put_text(b, ")");
}

/* `data` under manifests/, by its sha256, unless the store has it. */
static int store_manifest(hull_store *store, const buf *data, char hex[65], char *why, size_t cap)
{
	hull_sha256 h;
	uint8_t     digest[32];
	char        temp[4300];
	char        final[4300];
	int         fd;

	hull_sha256_init(&h);
	hull_sha256_update(&h, data->data, data->length);
	hull_sha256_final(&h, digest);
	hex_of(digest, hex);
	snprintf(final, sizeof final, "%s/manifests/sha256/%s", store->root, hex);
	snprintf(temp, sizeof temp, "%s/tmp/manifest.XXXXXX", store->root);
	if ((fd = mkostemp(temp, O_CLOEXEC)) < 0)
		return say(why, cap, temp, strerror(errno));
	if (write(fd, data->data, data->length) != (ssize_t)data->length || fchmod(fd, 0444) != 0 ||
	    close(fd) != 0 || (link(temp, final) != 0 && errno != EEXIST)) {
		unlink(temp);
		return say(why, cap, final, strerror(errno));
	}
	unlink(temp);
	return 0;
}

/* refs/REF as a symlink to the manifest, replaced whole by a rename. */
static int set_ref(hull_store *store, const char *ref, const char *hex, char *why, size_t cap)
{
	char path[4400];
	char temp[4500];
	char target[4400];
	int  depth = 0;

	for (const char *p = ref; *p != '\0'; p++)
		depth += *p == '/';
	snprintf(path, sizeof path, "%s/refs/%s", store->root, ref);
	{
		char *slash = strrchr(path, '/');

		*slash = '\0';
		if (make_dirs(path) != 0)
			return say(why, cap, path, strerror(errno));
		*slash = '/';
	}
	target[0] = '\0';
	for (int i = 0; i <= depth; i++)
		strcat(target, "../");
	strcat(target, "manifests/sha256/");
	strcat(target, hex);
	snprintf(temp, sizeof temp, "%s.new", path);
	unlink(temp);
	if (symlink(target, temp) != 0 || rename(temp, path) != 0) {
		unlink(temp);
		return say(why, cap, path, strerror(errno));
	}
	return 0;
}

int hull_store_commit(hull_store *store, const char *dir, const char *arch, const char *ref,
                      hull_commit *out, char *why, size_t cap)
{
	walk     w = { .why = why, .why_cap = cap };
	buf      manifest = { 0 };
	uint8_t *digests;
	int      top;
	int      result = -1;

	memset(out, 0, sizeof *out);
	if (!is_ref(ref))
		return say(why, cap, "a ref is names of letters, digits and ._-:@, joined by /", NULL);
	if ((top = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC)) < 0)
		return say(why, cap, dir, strerror(errno));
	/* Shared: commits may run together, but not beside a gc, which could
	 * take an object before the ref that reaches it is written. */
	flock(store->lock, LOCK_SH);
	if (walk_dir(store, &w, top, "", out) != 0) {
		if (why[0] == '\0')
			say(why, cap, dir, "it could not be read whole");
		goto done;
	}
	qsort(w.items, w.count, sizeof w.items[0], path_order);
	digests = malloc(32u * (w.count + 1u));
	if (digests == NULL)
		goto done;
	for (size_t i = 0u; i < w.count; i++)
		if (S_ISREG(w.items[i].st.st_mode)) {
			memcpy(digests + 32u * out->objects++, w.items[i].digest, 32u);
			out->total_size += (uint64_t)w.items[i].st.st_size;
		}
	qsort(digests, out->objects, 32u, digest_order);
	{
		uint64_t distinct = 0u;

		for (uint64_t i = 0u; i < out->objects; i++)
			distinct += i == 0u || memcmp(digests + 32u * i, digests + 32u * (i - 1u), 32u) != 0;
		out->objects = distinct;
	}
	free(digests);
	out->entries = w.count;
	out->skipped = w.skipped;
	put_text(&manifest, "(8:manifest(4:arch");
	put_atom(&manifest, arch, strlen(arch));
	put_text(&manifest, ")(7:entries");
	for (size_t i = 0u; i < w.count; i++)
		put_entry(&manifest, &w.items[i]);
	put_text(&manifest, ")(6:schema1:1)(5:stats(10:file_count");
	put_number(&manifest, out->entries);
	put_text(&manifest, ")(12:object_count");
	put_number(&manifest, out->objects);
	put_text(&manifest, ")(10:total_size");
	put_number(&manifest, out->total_size);
	put_text(&manifest, ")))");
	if (manifest.failed) {
		say(why, cap, "out of memory", NULL);
		goto done;
	}
	if (store_manifest(store, &manifest, out->manifest, why, cap) == 0 &&
	    set_ref(store, ref, out->manifest, why, cap) == 0)
		result = 0;
done:
	flock(store->lock, LOCK_UN);
	for (size_t i = 0u; i < w.count; i++) {
		free(w.items[i].path);
		free(w.items[i].target);
		free(w.items[i].xattrs.data);
	}
	free(w.items);
	free(manifest.data);
	return result;
}

/* --- reading a manifest ----------------------------------------------------- */

/* The manifest `hex` names, checked against its name. Freed by the caller. */
static uint8_t *read_manifest(hull_store *store, const char *hex, size_t *length, char *why,
                              size_t cap)
{
	char        path[4300];
	struct stat st;
	uint8_t    *data;
	hull_sha256 h;
	uint8_t     digest[32];
	char        found[65];
	int         fd;

	snprintf(path, sizeof path, "%s/manifests/sha256/%s", store->root, hex);
	if ((fd = open(path, O_RDONLY | O_CLOEXEC)) < 0 || fstat(fd, &st) != 0 ||
	    st.st_size > (off_t)(256u << 20) || (data = malloc((size_t)st.st_size + 1u)) == NULL) {
		say(why, cap, path, strerror(errno));
		if (fd >= 0)
			close(fd);
		return NULL;
	}
	*length = (size_t)st.st_size;
	if (read(fd, data, *length) != (ssize_t)*length) {
		close(fd);
		free(data);
		say(why, cap, path, "it could not be read");
		return NULL;
	}
	close(fd);
	hull_sha256_init(&h);
	hull_sha256_update(&h, data, *length);
	hull_sha256_final(&h, digest);
	hex_of(digest, found);
	if (strcmp(found, hex) != 0) {
		free(data);
		say(why, cap, path, "its bytes are not the manifest its name says");
		return NULL;
	}
	return data;
}

/* The manifest a ref names, as its hex. */
static int ref_manifest(hull_store *store, const char *ref, char hex[65], char *why, size_t cap)
{
	char    path[4400];
	char    target[4400];
	ssize_t n;
	char   *name;

	if (!is_ref(ref))
		return say(why, cap, "a ref is names of letters, digits and ._-:@, joined by /", NULL);
	snprintf(path, sizeof path, "%s/refs/%s", store->root, ref);
	n = readlink(path, target, sizeof target - 1u);
	if (n < 0)
		return say(why, cap, ref, errno == ENOENT ? "no such ref" : strerror(errno));
	target[n] = '\0';
	name = strrchr(target, '/');
	name = name != NULL ? name + 1 : target;
	if (strlen(name) != 64u || strspn(name, "0123456789abcdef") != 64u)
		return say(why, cap, ref, "it names no manifest");
	snprintf(hex, 65, "%.64s", name);
	return 0;
}

/* What names a manifest: a ref, or sha256:HEX for the manifest itself. */
static int name_manifest(hull_store *store, const char *name, char hex[65], char *why,
                         size_t cap)
{
	const char *given = name + 7;

	if (strncmp(name, "sha256:", 7u) != 0)
		return ref_manifest(store, name, hex, why, cap);
	if (strlen(given) != 64u || strspn(given, "0123456789abcdef") != 64u)
		return say(why, cap, name, "a manifest is sha256: and 64 lowercase hex digits");
	memcpy(hex, given, 65u);
	return 0;
}

/* `(key ...)` in a record: the list. */
static int find(const hull_sexpr_doc *doc, const hull_sexpr_form *record, const char *key,
                hull_sexpr_form *found)
{
	hull_sexpr_form item;
	hull_sexpr_form name;
	int             more;

	hull_sexpr_child(doc, record, &item);
	for (more = hull_sexpr_next(doc, &item, &item); more; more = hull_sexpr_next(doc, &item, &item))
		if (item.kind == HULL_SEXPR_LIST && hull_sexpr_child(doc, &item, &name) &&
		    hull_sexpr_is(doc, &name, key)) {
			*found = item;
			return 1;
		}
	return 0;
}

/* `(key value ...)` in a record: its first value. */
static int field(const hull_sexpr_doc *doc, const hull_sexpr_form *record, const char *key,
                 hull_sexpr_form *value)
{
	hull_sexpr_form item;
	hull_sexpr_form name;

	return find(doc, record, key, &item) && hull_sexpr_child(doc, &item, &name) &&
	       hull_sexpr_next(doc, &name, value);
}

static unsigned long long number(const hull_sexpr_doc *doc, const hull_sexpr_form *value)
{
	char text[24];

	if (value->kind != HULL_SEXPR_ATOM || value->content_length >= sizeof text)
		return 0u;
	memcpy(text, doc->data + value->content, value->content_length);
	text[value->content_length] = '\0';
	return strtoull(text, NULL, 10);
}

/* The manifest's `(7:entries ...)`, opened: its elements after the first
 * are the entries. */
static int open_entries(hull_sexpr_doc *doc, const uint8_t *data, size_t length,
                        hull_sexpr_form *entries)
{
	hull_sexpr_form top;
	hull_sexpr_form head;

	return hull_sexpr_open(doc, data, length, HULL_SEXPR_CANONICAL, NULL) == HULL_SEXPR_OK &&
	       hull_sexpr_first(doc, &top) && hull_sexpr_child(doc, &top, &head) &&
	       hull_sexpr_is(doc, &head, "manifest") && find(doc, &top, "entries", entries);
}

/* --- materializing ------------------------------------------------------ */

/* A relative path a manifest may hold: no `/` at either end, no empty,
 * `.` or `..` component. */
static int is_relative(const char *path)
{
	const char *start = path;

	if (path[0] == '\0')
		return 0;
	for (const char *p = path;; p++)
		if (*p == '/' || *p == '\0') {
			size_t n = (size_t)(p - start);

			if (n == 0u || (n == 1u && start[0] == '.') ||
			    (n == 2u && start[0] == '.' && start[1] == '.'))
				return 0;
			if (*p == '\0')
				return 1;
			start = p + 1;
		}
}

/* The directory a path's leaf is made in, resolved beneath `root` and
 * through no symlink; `leaf` points into `path`. */
static int parent_of(int root, char *path, const char **leaf)
{
	struct open_how how = { .flags = O_PATH | O_DIRECTORY | O_CLOEXEC,
	                        .resolve = RESOLVE_BENEATH | RESOLVE_NO_SYMLINKS };
	char           *slash = strrchr(path, '/');
	int             fd;

	if (slash == NULL) {
		*leaf = path;
		return dup(root);
	}
	*slash = '\0';
	fd = (int)syscall(SYS_openat2, root, path, &how, sizeof how);
	*slash = '/';
	*leaf = slash + 1;
	return fd;
}

/* An object's content into a new file, checked against its name. */
static int copy_object(hull_store *store, const uint8_t digest[32], int dir, const char *leaf,
                       mode_t mode, char *why, size_t cap)
{
	static uint8_t chunk[1 << 16];
	char           hex[65];
	char           path[4300];
	hull_sha256    h;
	uint8_t        found[32];
	ssize_t        n;
	int            in;
	int            out;

	hex_of(digest, hex);
	snprintf(path, sizeof path, "%s/objects/sha256/%.2s/%s", store->root, hex, hex);
	if ((in = open(path, O_RDONLY | O_CLOEXEC)) < 0)
		return say(why, cap, path, strerror(errno));
	out = openat(dir, leaf, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
	if (out < 0) {
		close(in);
		return say(why, cap, leaf, strerror(errno));
	}
	hull_sha256_init(&h);
	while ((n = read(in, chunk, sizeof chunk)) > 0) {
		hull_sha256_update(&h, chunk, (size_t)n);
		if (write(out, chunk, (size_t)n) != n) {
			n = -1;
			break;
		}
	}
	hull_sha256_final(&h, found);
	close(in);
	if (n < 0 || fchmod(out, mode) != 0 || close(out) != 0)
		return say(why, cap, leaf, strerror(errno));
	if (memcmp(found, digest, 32u) != 0)
		return say(why, cap, path, "its content is not the object its name says");
	return 0;
}

/* As hull_store_materialize(), under a lock the caller holds. */
static int materialize_locked(hull_store *store, const char *ref, const char *dir,
                              uint64_t *skipped, char *why, size_t cap)
{
	char            hex[65];
	uint8_t        *data;
	size_t          length;
	hull_sexpr_doc  doc;
	hull_sexpr_form entries;
	hull_sexpr_form entry;
	char          **dirs = NULL;
	mode_t         *dir_modes = NULL;
	size_t          dir_count = 0u;
	int             root;
	int             more;
	int             result = -1;

	*skipped = 0u;
	if (name_manifest(store, ref, hex, why, cap) != 0 ||
	    (data = read_manifest(store, hex, &length, why, cap)) == NULL)
		return -1;
	if (!open_entries(&doc, data, length, &entries)) {
		free(data);
		return say(why, cap, ref, "its manifest is not one");
	}
	if (mkdir(dir, 0700) != 0 && errno != EEXIST) {
		free(data);
		return say(why, cap, dir, strerror(errno));
	}
	if ((root = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC)) < 0) {
		free(data);
		return say(why, cap, dir, strerror(errno));
	}
	{
		DIR           *list = fdopendir(dup(root));
		struct dirent *item;
		int            empty = 1;

		while (list != NULL && (item = readdir(list)) != NULL)
			empty &= strcmp(item->d_name, ".") == 0 || strcmp(item->d_name, "..") == 0;
		if (list != NULL)
			closedir(list);
		if (!empty) {
			close(root);
			free(data);
			return say(why, cap, dir, "it is not empty");
		}
	}
	more = hull_sexpr_child(&doc, &entries, &entry);
	for (more = more && hull_sexpr_next(&doc, &entry, &entry); more;
	     more = hull_sexpr_next(&doc, &entry, &entry)) {
		hull_sexpr_form value;
		char            path[PATH_MAX];
		char            kind[16] = "";
		const char     *leaf;
		mode_t          mode;
		int             parent;
		int             made = 0;

		if (!field(&doc, &entry, "path", &value) || value.content_length >= sizeof path)
			goto bad;
		memcpy(path, doc.data + value.content, value.content_length);
		path[value.content_length] = '\0';
		if (!is_relative(path) || memchr(path, '\0', value.content_length) != NULL)
			goto bad;
		if (field(&doc, &entry, "kind", &value) && value.content_length < sizeof kind)
			memcpy(kind, doc.data + value.content, value.content_length);
		mode = field(&doc, &entry, "mode", &value) ? (mode_t)(number(&doc, &value) & 07777) : 0644;
		if ((parent = parent_of(root, path, &leaf)) < 0) {
			say(why, cap, path, "its directory is not one beneath the target, through no symlink");
			goto done;
		}
		if (strcmp(kind, "dir") == 0) {
			char **grown = realloc(dirs, (dir_count + 1u) * sizeof *dirs);
			mode_t *modes = realloc(dir_modes, (dir_count + 1u) * sizeof *dir_modes);

			if (grown != NULL)
				dirs = grown;
			if (modes != NULL)
				dir_modes = modes;
			/* 0700 until every entry is in: the manifest's mode, which
			 * may not let the user write, is set last. */
			made = grown != NULL && modes != NULL && mkdirat(parent, leaf, 0700) == 0;
			if (made) {
				dirs[dir_count] = strdup(path);
				dir_modes[dir_count++] = mode;
			}
		} else if (strcmp(kind, "reg") == 0) {
			if (!field(&doc, &entry, "object", &value) || value.content_length != 32u) {
				close(parent);
				goto bad;
			}
			if (copy_object(store, doc.data + value.content, parent, leaf, mode, why, cap) != 0) {
				close(parent);
				goto done;
			}
			made = 1;
		} else if (strcmp(kind, "symlink") == 0) {
			char target[PATH_MAX];

			if (field(&doc, &entry, "target", &value) && value.content_length < sizeof target) {
				memcpy(target, doc.data + value.content, value.content_length);
				target[value.content_length] = '\0';
				made = symlinkat(target, parent, leaf) == 0;
			}
		} else if (strcmp(kind, "fifo") == 0) {
			made = mkfifoat(parent, leaf, mode) == 0;
		} else if (strcmp(kind, "dev") == 0 || strcmp(kind, "hardlink") == 0) {
			fprintf(stderr, "hull: materialize: %s: a %s, which is not made here, left out\n",
			        path, kind);
			(*skipped)++;
			close(parent);
			continue;
		} else {
			close(parent);
			goto bad;
		}
		if (!made) {
			say(why, cap, path, strerror(errno));
			close(parent);
			goto done;
		}
		/* The user's, group too: what is made in a setgid directory takes
		 * the directory's group, which a machine's user namespace, mapping
		 * the user's own alone, cannot write under. A chown may clear a
		 * file's setuid and setgid bits, so its mode is given again;
		 * directories are given theirs last. */
		if (fchownat(parent, leaf, (uid_t)-1, getgid(), AT_SYMLINK_NOFOLLOW) != 0) {
			say(why, cap, path, strerror(errno));
			close(parent);
			goto done;
		}
		if ((strcmp(kind, "reg") == 0 || strcmp(kind, "fifo") == 0) &&
		    fchmodat(parent, leaf, mode, 0) != 0) {
			say(why, cap, path, strerror(errno));
			close(parent);
			goto done;
		}
		/* An owner other than root and the user cannot be given rootless:
		 * the user's file stands for both. */
		if (field(&doc, &entry, "owner", &value) && value.kind == HULL_SEXPR_LIST) {
			fprintf(stderr, "hull: materialize: %s: its owner is kept as the user\n", path);
			(*skipped)++;
		}
		if (field(&doc, &entry, "xattrs", &value)) {
			hull_sexpr_form pair = value;
			char            at[64];
			int             fd = openat(parent, leaf, O_PATH | O_NOFOLLOW | O_CLOEXEC);
			int             go;

			snprintf(at, sizeof at, "/proc/self/fd/%d", fd);
			for (go = 1; fd >= 0 && go; go = hull_sexpr_next(&doc, &pair, &pair)) {
				hull_sexpr_form name;
				hull_sexpr_form bytes;
				char            key[256];

				if (pair.kind != HULL_SEXPR_LIST || !hull_sexpr_child(&doc, &pair, &name) ||
				    !hull_sexpr_next(&doc, &name, &bytes) || name.content_length >= sizeof key)
					continue;
				memcpy(key, doc.data + name.content, name.content_length);
				key[name.content_length] = '\0';
				if (setxattr(at, key, doc.data + bytes.content, bytes.content_length, 0) != 0) {
					fprintf(stderr, "hull: materialize: %s: xattr %s: %s, left out\n", path, key,
					        strerror(errno));
					(*skipped)++;
				}
			}
			if (fd >= 0)
				close(fd);
		}
		close(parent);
	}
	/* Deepest first, so a directory's mode never stops its own. */
	for (size_t i = dir_count; i > 0u; i--)
		if (fchmodat(root, dirs[i - 1u], dir_modes[i - 1u], 0) != 0) {
			say(why, cap, dirs[i - 1u], strerror(errno));
			goto done;
		}
	result = 0;
	goto done;
bad:
	say(why, cap, ref, "its manifest has an entry that is not one");
done:
	for (size_t i = 0u; i < dir_count; i++)
		free(dirs[i]);
	free(dirs);
	free(dir_modes);
	close(root);
	free(data);
	return result;
}

/* Shared, so that gc, which takes the lock alone, cannot take an object
 * between the manifest's read and its copy. */
int hull_store_materialize(hull_store *store, const char *ref, const char *dir,
                           uint64_t *skipped, char *why, size_t cap)
{
	int result;

	flock(store->lock, LOCK_SH);
	result = materialize_locked(store, ref, dir, skipped, why, cap);
	flock(store->lock, LOCK_UN);
	return result;
}

/* --- checkouts ------------------------------------------------------------ */

/* `name` in `parent`, and all beneath it: each directory made the user's
 * to empty first, since a tree's modes may not let it be, and no symlink
 * followed, since one is removed as itself. */
static int remove_tree(int parent, const char *name)
{
	int            fd = openat(parent, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
	DIR           *dir;
	struct dirent *entry;
	int            result = 0;

	if (fd < 0)
		return unlinkat(parent, name, 0) == 0 || errno == ENOENT ? 0 : -1;
	fchmod(fd, 0700);
	if ((dir = fdopendir(fd)) == NULL) {
		close(fd);
		return -1;
	}
	while ((entry = readdir(dir)) != NULL)
		if (strcmp(entry->d_name, ".") != 0 && strcmp(entry->d_name, "..") != 0 &&
		    remove_tree(dirfd(dir), entry->d_name) != 0)
			result = -1;
	closedir(dir);
	return unlinkat(parent, name, AT_REMOVEDIR) == 0 ? result : -1;
}

int hull_store_checkout(hull_store *store, const char *name, int create, char *path,
                        size_t path_cap, uint64_t *skipped, char *why, size_t cap)
{
	char        hex[65];
	char        named[72];
	char        made[4400];
	struct stat st;
	int         result = -1;

	*skipped = 0u;
	if (name_manifest(store, name, hex, why, cap) != 0)
		return -1;
	if ((size_t)snprintf(path, path_cap, "%s/checkouts/sha256/%s", store->root, hex) >= path_cap)
		return say(why, cap, store->root, "a checkout's path would be too long");
	if (!create)
		return 0;
	flock(store->lock, LOCK_SH);
	if (lstat(path, &st) == 0 && S_ISDIR(st.st_mode)) {
		flock(store->lock, LOCK_UN);
		return 0;
	}
	/* Made whole under tmp/ and renamed in, so that a checkout is the tree
	 * or absent; tmp/ is gc's to empty only under the lock alone. */
	snprintf(made, sizeof made, "%s/checkouts/sha256", store->root);
	if (make_dirs(made) != 0) {
		say(why, cap, made, strerror(errno));
		goto done;
	}
	snprintf(made, sizeof made, "%s/tmp/checkout-XXXXXX", store->root);
	if (mkdtemp(made) == NULL) {
		say(why, cap, made, strerror(errno));
		goto done;
	}
	snprintf(named, sizeof named, "sha256:%s", hex);
	if (materialize_locked(store, named, made, skipped, why, cap) != 0) {
		remove_tree(AT_FDCWD, made);
		goto done;
	}
	/* A machine's / is a directory anyone may enter, as a dir source's is. */
	chmod(made, 0755);
	if (rename(made, path) != 0) {
		int taken = errno == EEXIST || errno == ENOTEMPTY;

		if (!taken)
			say(why, cap, path, strerror(errno));
		remove_tree(AT_FDCWD, made);
		if (!taken)
			goto done;
	}
	result = 0;
done:
	flock(store->lock, LOCK_UN);
	return result;
}

/* --- diffing -------------------------------------------------------------- */

/* One side of a diff: its manifest, opened, and where its walk is. */
typedef struct side {
	uint8_t        *data;
	hull_sexpr_doc  doc;
	hull_sexpr_form top;
	hull_sexpr_form entry;
	int             more;
	const uint8_t  *path;          /* the entry's */
	size_t          path_length;
	const uint8_t  *last;          /* the entry's before it, or NULL */
	size_t          last_length;
} side;

static int open_side(hull_store *store, const char *name, side *s, char *why, size_t cap)
{
	char            hex[65];
	size_t          length;
	hull_sexpr_form entries;

	memset(s, 0, sizeof *s);
	if (name_manifest(store, name, hex, why, cap) != 0 ||
	    (s->data = read_manifest(store, hex, &length, why, cap)) == NULL)
		return -1;
	if (!open_entries(&s->doc, s->data, length, &entries) || !hull_sexpr_first(&s->doc, &s->top)) {
		free(s->data);
		s->data = NULL;
		return say(why, cap, name, "its manifest is not one");
	}
	s->more = hull_sexpr_child(&s->doc, &entries, &s->entry) &&
	          hull_sexpr_next(&s->doc, &s->entry, &s->entry);
	return 0;
}

/* The side's next entry's path, or 0 at its end; -1 for an entry with
 * none, or one out of order, which no manifest of ours holds. */
static int side_path(side *s)
{
	hull_sexpr_form value;

	if (!s->more)
		return 0;
	if (!field(&s->doc, &s->entry, "path", &value) || value.kind != HULL_SEXPR_ATOM)
		return -1;
	s->path = s->doc.data + value.content;
	s->path_length = value.content_length;
	if (s->last != NULL) {
		size_t n = s->last_length < s->path_length ? s->last_length : s->path_length;
		int    order = memcmp(s->last, s->path, n);

		if (order > 0 || (order == 0 && s->last_length >= s->path_length))
			return -1;
	}
	return 1;
}

static void side_next(side *s)
{
	s->last = s->path;
	s->last_length = s->path_length;
	s->more = hull_sexpr_next(&s->doc, &s->entry, &s->entry);
}

/* `key`'s whole form in each record, the same bytes or both absent. */
static int same_field(const hull_sexpr_doc *a, const hull_sexpr_form *ra, const hull_sexpr_doc *b,
                      const hull_sexpr_form *rb, const char *key)
{
	hull_sexpr_form fa;
	hull_sexpr_form fb;
	int             in_a = find(a, ra, key, &fa);
	int             in_b = find(b, rb, key, &fb);

	if (in_a != in_b)
		return 0;
	return !in_a || (fa.extent == fb.extent &&
	                 memcmp(a->data + fa.offset, b->data + fb.offset, fa.extent) == 0);
}

/* A field's value as text: an atom's bytes, or for an owner given by ids,
 * uid:gid. */
static void field_text(const hull_sexpr_doc *doc, const hull_sexpr_form *record, const char *key,
                       char *out, size_t cap)
{
	hull_sexpr_form value;

	snprintf(out, cap, "none");
	if (!field(doc, record, key, &value))
		return;
	if (value.kind == HULL_SEXPR_ATOM) {
		snprintf(out, cap, "%.*s", (int)value.content_length, doc->data + value.content);
	} else if (strcmp(key, "owner") == 0) {
		hull_sexpr_form owner;
		hull_sexpr_form id;

		if (find(doc, record, "owner", &owner)) {
			unsigned long long uid = 0u;
			unsigned long long gid = 0u;

			if (field(doc, &owner, "uid", &id))
				uid = number(doc, &id);
			if (field(doc, &owner, "gid", &id))
				gid = number(doc, &id);
			snprintf(out, cap, "%llu:%llu", uid, gid);
		}
	}
}

/* What differs between two entries of one path, said as `kind reg -> dir,
 * mode 0644 -> 0755, content`. A kind that changed is said alone with the
 * mode and owner, since its other fields belong to the kinds. */
static void entry_changes(const side *a, const side *b, char *out, size_t cap)
{
	char   from[300];
	char   to[300];
	size_t at = 0u;
	int    kind = !same_field(&a->doc, &a->entry, &b->doc, &b->entry, "kind");

	out[0] = '\0';
#define SAY(...) do { \
		int n_ = snprintf(out + at, cap - at, "%s", at > 0u ? ", " : ""); \
		if (n_ > 0 && (size_t)n_ < cap - at) at += (size_t)n_; \
		n_ = snprintf(out + at, cap - at, __VA_ARGS__); \
		if (n_ > 0 && (size_t)n_ < cap - at) at += (size_t)n_; \
	} while (0)
	if (kind) {
		field_text(&a->doc, &a->entry, "kind", from, sizeof from);
		field_text(&b->doc, &b->entry, "kind", to, sizeof to);
		SAY("kind %s -> %s", from, to);
	}
	if (!same_field(&a->doc, &a->entry, &b->doc, &b->entry, "mode")) {
		hull_sexpr_form value;
		unsigned long long old_mode = 0u;
		unsigned long long new_mode = 0u;

		if (field(&a->doc, &a->entry, "mode", &value))
			old_mode = number(&a->doc, &value);
		if (field(&b->doc, &b->entry, "mode", &value))
			new_mode = number(&b->doc, &value);
		SAY("mode %04llo -> %04llo", old_mode, new_mode);
	}
	if (!same_field(&a->doc, &a->entry, &b->doc, &b->entry, "owner")) {
		field_text(&a->doc, &a->entry, "owner", from, sizeof from);
		field_text(&b->doc, &b->entry, "owner", to, sizeof to);
		SAY("owner %s -> %s", from, to);
	}
	if (!kind) {
		if (!same_field(&a->doc, &a->entry, &b->doc, &b->entry, "object") ||
		    !same_field(&a->doc, &a->entry, &b->doc, &b->entry, "size"))
			SAY("content");
		if (!same_field(&a->doc, &a->entry, &b->doc, &b->entry, "target")) {
			field_text(&a->doc, &a->entry, "target", from, sizeof from);
			field_text(&b->doc, &b->entry, "target", to, sizeof to);
			SAY("target %s -> %s", from, to);
		}
		if (!same_field(&a->doc, &a->entry, &b->doc, &b->entry, "dev"))
			SAY("device");
	}
	if (!same_field(&a->doc, &a->entry, &b->doc, &b->entry, "xattrs"))
		SAY("xattrs");
	/* A field no manifest of this version writes, which differs. */
	if (at == 0u)
		SAY("other");
#undef SAY
}

int hull_store_diff(hull_store *store, const char *from, const char *to,
                    void (*each)(void *context, char change, const char *path, const char *what),
                    void *context, char *why, size_t cap)
{
	side  a;
	side  b;
	int   result = -1;
	int   differ = 0;
	char  path[PATH_MAX];
	char  what[2048];

	flock(store->lock, LOCK_SH);
	if (open_side(store, from, &a, why, cap) != 0) {
		flock(store->lock, LOCK_UN);
		return -1;
	}
	if (open_side(store, to, &b, why, cap) != 0) {
		flock(store->lock, LOCK_UN);
		free(a.data);
		return -1;
	}
	flock(store->lock, LOCK_UN);
	if (!same_field(&a.doc, &a.top, &b.doc, &b.top, "arch")) {
		char old_arch[64];
		char new_arch[64];

		field_text(&a.doc, &a.top, "arch", old_arch, sizeof old_arch);
		field_text(&b.doc, &b.top, "arch", new_arch, sizeof new_arch);
		snprintf(what, sizeof what, "arch %s -> %s", old_arch, new_arch);
		each(context, '~', NULL, what);
		differ = 1;
	}
	for (;;) {
		int in_a = side_path(&a);
		int in_b = side_path(&b);
		int order;

		if (in_a < 0 || in_b < 0) {
			say(why, cap, in_a < 0 ? from : to,
			    "its manifest has an entry with no path, or out of order");
			goto done;
		}
		if (!in_a && !in_b)
			break;
		if (!in_a) {
			order = 1;
		} else if (!in_b) {
			order = -1;
		} else {
			size_t n = a.path_length < b.path_length ? a.path_length : b.path_length;

			order = memcmp(a.path, b.path, n);
			if (order == 0)
				order = a.path_length < b.path_length ? -1 : a.path_length > b.path_length;
		}
		if (order < 0) {
			snprintf(path, sizeof path, "%.*s", (int)a.path_length, a.path);
			each(context, '-', path, "");
			differ = 1;
			side_next(&a);
		} else if (order > 0) {
			snprintf(path, sizeof path, "%.*s", (int)b.path_length, b.path);
			each(context, '+', path, "");
			differ = 1;
			side_next(&b);
		} else {
			if (a.entry.extent != b.entry.extent ||
			    memcmp(a.doc.data + a.entry.offset, b.doc.data + b.entry.offset,
			           a.entry.extent) != 0) {
				snprintf(path, sizeof path, "%.*s", (int)a.path_length, a.path);
				entry_changes(&a, &b, what, sizeof what);
				each(context, '~', path, what);
				differ = 1;
			}
			side_next(&a);
			side_next(&b);
		}
	}
	result = differ;
done:
	free(a.data);
	free(b.data);
	return result;
}

/* --- refs and gc ------------------------------------------------------- */

typedef struct found_ref {
	char ref[300];
	char manifest[65];
} found_ref;

typedef struct ref_list {
	found_ref *items;
	size_t     count;
} ref_list;

static void find_refs(hull_store *store, const char *prefix, ref_list *list)
{
	char           path[4500];
	DIR           *dir;
	struct dirent *entry;

	snprintf(path, sizeof path, "%s/refs%s%s", store->root, prefix[0] != '\0' ? "/" : "", prefix);
	if ((dir = opendir(path)) == NULL)
		return;
	while ((entry = readdir(dir)) != NULL) {
		char        ref[300];
		struct stat st;
		char        hex[65];
		char        why[64];

		if (entry->d_name[0] == '.' || strstr(entry->d_name, ".new") != NULL)
			continue;
		snprintf(ref, sizeof ref, "%s%s%s", prefix, prefix[0] != '\0' ? "/" : "", entry->d_name);
		snprintf(path, sizeof path, "%s/refs/%.299s", store->root, ref);
		if (lstat(path, &st) != 0)
			continue;
		if (S_ISDIR(st.st_mode)) {
			find_refs(store, ref, list);
		} else if (S_ISLNK(st.st_mode) && ref_manifest(store, ref, hex, why, sizeof why) == 0) {
			found_ref *grown = realloc(list->items, (list->count + 1u) * sizeof *grown);

			if (grown == NULL)
				continue;
			list->items = grown;
			snprintf(list->items[list->count].ref, sizeof list->items[0].ref, "%s", ref);
			snprintf(list->items[list->count].manifest, 65, "%s", hex);
			list->count++;
		}
	}
	closedir(dir);
}

static int ref_order(const void *a, const void *b)
{
	return strcmp(((const found_ref *)a)->ref, ((const found_ref *)b)->ref);
}

int hull_store_list(hull_store *store, void (*each)(void *context, const char *ref,
                                                    const char *manifest),
                    void *context, char *why, size_t cap)
{
	ref_list list = { 0 };

	(void)why;
	(void)cap;
	find_refs(store, "", &list);
	qsort(list.items, list.count, sizeof list.items[0], ref_order);
	for (size_t i = 0u; i < list.count; i++)
		each(context, list.items[i].ref, list.items[i].manifest);
	free(list.items);
	return 0;
}

/* A sorted set of digests, as hex. */
typedef struct hex_set {
	char  (*items)[65];
	size_t count;
	size_t cap;
} hex_set;

static int set_add(hex_set *set, const char *hex)
{
	if (set->count == set->cap) {
		size_t cap = set->cap != 0u ? 2u * set->cap : 1024u;
		char (*grown)[65] = realloc(set->items, cap * sizeof *grown);

		if (grown == NULL)
			return -1;
		set->items = grown;
		set->cap   = cap;
	}
	snprintf(set->items[set->count++], 65, "%s", hex);
	return 0;
}

static int hex_order(const void *a, const void *b)
{
	return strcmp(a, b);
}

static int set_has(const hex_set *set, const char *hex)
{
	return bsearch(hex, set->items, set->count, sizeof set->items[0], hex_order) != NULL;
}

int hull_store_gc(hull_store *store, uint64_t *manifests, uint64_t *objects,
                  uint64_t *checkouts, char *why, size_t cap)
{
	ref_list       list = { 0 };
	hex_set        kept_manifests = { 0 };
	hex_set        kept_objects = { 0 };
	char           path[4400];
	DIR           *dir;
	struct dirent *entry;
	int            result = -1;

	*manifests = *objects = *checkouts = 0u;
	if (flock(store->lock, LOCK_EX) != 0)
		return say(why, cap, "the store's lock", strerror(errno));
	find_refs(store, "", &list);
	for (size_t i = 0u; i < list.count; i++) {
		uint8_t        *data;
		size_t          length;
		hull_sexpr_doc  doc;
		hull_sexpr_form entries;
		hull_sexpr_form item;
		int             more;

		if (set_add(&kept_manifests, list.items[i].manifest) != 0)
			goto done;
		/* A ref whose manifest is unreadable keeps nothing it cannot
		 * name; gc refuses to go on, rather than take its objects. */
		if ((data = read_manifest(store, list.items[i].manifest, &length, why, cap)) == NULL)
			goto done;
		if (!open_entries(&doc, data, length, &entries)) {
			free(data);
			say(why, cap, list.items[i].ref, "its manifest is not one");
			goto done;
		}
		more = hull_sexpr_child(&doc, &entries, &item);
		for (more = more && hull_sexpr_next(&doc, &item, &item); more;
		     more = hull_sexpr_next(&doc, &item, &item)) {
			hull_sexpr_form value;
			char            hex[65];

			if (field(&doc, &item, "object", &value) && value.content_length == 32u) {
				hex_of(doc.data + value.content, hex);
				if (set_add(&kept_objects, hex) != 0) {
					free(data);
					goto done;
				}
			}
		}
		free(data);
	}
	qsort(kept_manifests.items, kept_manifests.count, sizeof kept_manifests.items[0], hex_order);
	qsort(kept_objects.items, kept_objects.count, sizeof kept_objects.items[0], hex_order);
	snprintf(path, sizeof path, "%s/manifests/sha256", store->root);
	if ((dir = opendir(path)) != NULL) {
		while ((entry = readdir(dir)) != NULL)
			if (strlen(entry->d_name) == 64u && !set_has(&kept_manifests, entry->d_name) &&
			    unlinkat(dirfd(dir), entry->d_name, 0) == 0)
				(*manifests)++;
		closedir(dir);
	}
	for (unsigned top = 0u; top < 256u; top++) {
		char sub[4500];

		snprintf(sub, sizeof sub, "%s/objects/sha256/%02x", store->root, top);
		if ((dir = opendir(sub)) == NULL)
			continue;
		while ((entry = readdir(dir)) != NULL)
			if (strlen(entry->d_name) == 64u && !set_has(&kept_objects, entry->d_name) &&
			    unlinkat(dirfd(dir), entry->d_name, 0) == 0)
				(*objects)++;
		closedir(dir);
		rmdir(sub);
	}
	/* A checkout of a manifest no ref reaches. One a machine still runs
	 * on goes too: the store does not know its machines. */
	snprintf(path, sizeof path, "%s/checkouts/sha256", store->root);
	if ((dir = opendir(path)) != NULL) {
		while ((entry = readdir(dir)) != NULL)
			if (strlen(entry->d_name) == 64u && !set_has(&kept_manifests, entry->d_name) &&
			    remove_tree(dirfd(dir), entry->d_name) == 0)
				(*checkouts)++;
		closedir(dir);
	}
	/* What a commit or a checkout that went left in tmp/: none is under
	 * way, the lock being this one's alone. */
	snprintf(path, sizeof path, "%s/tmp", store->root);
	if ((dir = opendir(path)) != NULL) {
		while ((entry = readdir(dir)) != NULL)
			if (entry->d_name[0] != '.')
				remove_tree(dirfd(dir), entry->d_name);
		closedir(dir);
	}
	result = 0;
done:
	flock(store->lock, LOCK_UN);
	free(list.items);
	free(kept_manifests.items);
	free(kept_objects.items);
	return result;
}
