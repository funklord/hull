/*
 * storefs.c -- see storefs.h.
 */

#define _GNU_SOURCE
#include "storefs.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/fuse.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <unistd.h>

#include "sexpr.h"

/* A tree's entry, the root at index 0: offsets are into the manifest's
 * bytes, which stay mapped while the tree is served. */
typedef struct node {
	uint32_t        path;          /* its path's bytes, no leading slash; "" for the root */
	uint32_t        path_length;
	uint32_t        parent;
	uint32_t        first_child;   /* 0 for none: the root is no one's child */
	uint32_t        last_child;
	uint32_t        next;          /* its next sibling, or 0 */
	uint32_t        mode;          /* st_mode, its type in it */
	uint32_t        uid;
	uint32_t        gid;
	uint32_t        target;        /* a symlink's target's bytes */
	uint32_t        target_length;
	uint64_t        size;
	uint64_t        rdev;
	hull_sexpr_form xattr;         /* its first (name value), where has_xattrs */
	uint8_t         has_xattrs;
	uint8_t         has_object;
	uint8_t         digest[32];
} node;

typedef struct tree {
	const uint8_t  *data;   /* the manifest, mapped */
	size_t          length;
	hull_sexpr_doc  doc;
	node           *nodes;
	uint32_t        count;
	size_t          nodes_size;  /* their mapping's length */
	int             objects;     /* objects/sha256, opened */
} tree;

#define TTL     (365u * 86400u)  /* a stored tree does not change: cached a year */
#define PAGES   256u             /* a read's most, 1 MiB */
#define REQUEST ((PAGES + 1u) * 4096u)

static void *map(size_t length)
{
	void *p = mmap(NULL, length, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

	return p == MAP_FAILED ? NULL : p;
}

int hull_storefs_parts(const char *image, char *store, size_t store_cap, char hex[65])
{
	const char *slash = strrchr(image, '/');
	size_t      mounts;

	if (slash == NULL || strlen(slash + 1) < 65u)
		return -1;
	for (unsigned i = 0u; i < 64u; i++)
		if (!((slash[1 + i] >= '0' && slash[1 + i] <= '9') ||
		      (slash[1 + i] >= 'a' && slash[1 + i] <= 'f')))
			return -1;
	if (slash[65] != '.')
		return -1;
	memcpy(hex, slash + 1, 64u);
	hex[64] = '\0';
	mounts = (size_t)(slash - image);
	if (mounts < 7u || memcmp(slash - 7, "/mounts", 7u) != 0 || mounts - 7u >= store_cap)
		return -1;
	memcpy(store, image, mounts - 7u);
	store[mounts - 7u] = '\0';
	return 0;
}

/* An atom's bytes as a decimal number. */
static uint64_t number(const tree *t, const hull_sexpr_form *f)
{
	uint64_t n = 0u;

	for (uint32_t i = 0u; i < f->content_length; i++)
		n = n * 10u + (uint64_t)(t->data[f->content + i] - '0');
	return n;
}

/* Among `list`'s children, the entry `(key value ...)` whose key is `key`:
 * 1, its value -- the form after the key -- in `out`. */
static int field(const tree *t, const hull_sexpr_form *list, const char *key, hull_sexpr_form *out)
{
	hull_sexpr_form item;
	hull_sexpr_form head;

	if (!hull_sexpr_child(&t->doc, list, &item))
		return 0;
	do {
		if (item.kind == HULL_SEXPR_LIST && hull_sexpr_child(&t->doc, &item, &head) &&
		    hull_sexpr_is(&t->doc, &head, key))
			return hull_sexpr_next(&t->doc, &head, out);
	} while (hull_sexpr_next(&t->doc, &item, &item));
	return 0;
}

/* `(key value)` lists from `first` on, siblings: each value by its key. */
static void pairs(const tree *t, hull_sexpr_form first, void (*each)(const tree *t,
                  const hull_sexpr_form *key, const hull_sexpr_form *value, void *context),
                  void *context)
{
	do {
		hull_sexpr_form key;
		hull_sexpr_form value;

		if (first.kind == HULL_SEXPR_LIST && hull_sexpr_child(&t->doc, &first, &key) &&
		    hull_sexpr_next(&t->doc, &key, &value))
			each(t, &key, &value, context);
	} while (hull_sexpr_next(&t->doc, &first, &first));
}

typedef struct device {
	uint64_t major;
	uint64_t minor;
	int      block;
} device;

static void device_part(const tree *t, const hull_sexpr_form *key, const hull_sexpr_form *value,
                        void *context)
{
	device *d = context;

	if (hull_sexpr_is(&t->doc, key, "major"))
		d->major = number(t, value);
	else if (hull_sexpr_is(&t->doc, key, "minor"))
		d->minor = number(t, value);
	else if (hull_sexpr_is(&t->doc, key, "type"))
		d->block = hull_sexpr_is(&t->doc, value, "b");
}

static void owner_part(const tree *t, const hull_sexpr_form *key, const hull_sexpr_form *value,
                       void *context)
{
	node *e = context;

	if (hull_sexpr_is(&t->doc, key, "uid"))
		e->uid = (uint32_t)number(t, value);
	else if (hull_sexpr_is(&t->doc, key, "gid"))
		e->gid = (uint32_t)number(t, value);
}

/* Bytewise order of paths, as a manifest's entries are in. */
static int order(const tree *t, uint32_t a, uint32_t a_length, const uint8_t *b, size_t b_length)
{
	size_t n = a_length < b_length ? a_length : b_length;
	int    c = memcmp(t->data + a, b, n);

	return c != 0 ? c : a_length < b_length ? -1 : a_length > b_length ? 1 : 0;
}

/* The node of `path`, or 0 where the tree has none (0 is the root, which
 * no path names). */
static uint32_t find(const tree *t, const uint8_t *path, size_t length)
{
	uint32_t low = 1u;
	uint32_t high = t->count;

	while (low < high) {
		uint32_t mid = low + (high - low) / 2u;
		int      c = order(t, t->nodes[mid].path, t->nodes[mid].path_length, path, length);

		if (c == 0)
			return mid;
		if (c < 0)
			low = mid + 1u;
		else
			high = mid;
	}
	return 0u;
}

/* One entry of the manifest made `e`. */
static const char *entry_of(const tree *t, const hull_sexpr_form *entry, node *e)
{
	hull_sexpr_form value;

	memset(e, 0, sizeof *e);
	if (!field(t, entry, "path", &value) || value.kind != HULL_SEXPR_ATOM)
		return "a manifest's entry has no path";
	e->path = value.content;
	e->path_length = value.content_length;
	if (!field(t, entry, "mode", &value))
		return "a manifest's entry has no mode";
	e->mode = (uint32_t)(number(t, &value) & 07777u);
	if (!field(t, entry, "kind", &value))
		return "a manifest's entry has no kind";
	if (hull_sexpr_is(&t->doc, &value, "dir")) {
		e->mode |= S_IFDIR;
	} else if (hull_sexpr_is(&t->doc, &value, "reg")) {
		e->mode |= S_IFREG;
	} else if (hull_sexpr_is(&t->doc, &value, "symlink")) {
		e->mode |= S_IFLNK;
	} else if (hull_sexpr_is(&t->doc, &value, "fifo")) {
		e->mode |= S_IFIFO;
	} else if (hull_sexpr_is(&t->doc, &value, "dev")) {
		device d = { 0u, 0u, 0 };

		if (field(t, entry, "dev", &value))
			pairs(t, value, device_part, &d);
		e->mode |= d.block ? S_IFBLK : S_IFCHR;
		/* The kernel's new encoding, as fuse_attr's rdev is read. */
		e->rdev = (d.minor & 0xffu) | ((d.major & 0xfffu) << 8) | ((d.minor & ~0xffull) << 12);
	} else {
		return "a manifest's entry is of a kind this does not serve";
	}
	/* (owner root), or (owner (gid N) (uid N)): the value is the atom, or
	 * the first of the two lists. */
	if (field(t, entry, "owner", &value) && value.kind == HULL_SEXPR_LIST)
		pairs(t, value, owner_part, e);
	if (field(t, entry, "size", &value))
		e->size = number(t, &value);
	if (field(t, entry, "object", &value) && value.content_length == 32u) {
		memcpy(e->digest, t->data + value.content, 32u);
		e->has_object = 1;
	}
	if (field(t, entry, "target", &value)) {
		e->target = value.content;
		e->target_length = value.content_length;
		e->size = value.content_length;
	}
	if (field(t, entry, "xattrs", &value) && value.kind == HULL_SEXPR_LIST) {
		e->xattr = value;
		e->has_xattrs = 1;
	}
	return NULL;
}

/* The manifest `hex` under `store`, read and its entries made nodes. */
static const char *load(tree *t, const char *store, const char *hex)
{
	char            path[4300];
	struct stat     st;
	hull_sexpr_form top;
	hull_sexpr_form first;
	hull_sexpr_form entry;
	int             fd;
	int             any;
	uint32_t        n = 0u;

	memset(t, 0, sizeof *t);
	t->objects = -1;
	snprintf(path, sizeof path, "%s/manifests/sha256/%s", store, hex);
	if ((fd = open(path, O_RDONLY | O_CLOEXEC)) < 0 || fstat(fd, &st) != 0 ||
	    st.st_size <= 0 || st.st_size > (off_t)(1u << 30)) {
		if (fd >= 0)
			close(fd);
		return "the tree's manifest cannot be read";
	}
	t->length = (size_t)st.st_size;
	t->data = mmap(NULL, t->length, PROT_READ, MAP_PRIVATE, fd, 0);
	close(fd);
	if (t->data == MAP_FAILED) {
		t->data = NULL;
		return "the tree's manifest cannot be mapped";
	}
	snprintf(path, sizeof path, "%s/objects/sha256", store);
	if ((t->objects = open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC)) < 0)
		return "the store's objects cannot be opened";
	{
		hull_sexpr_form head;

		if (hull_sexpr_open(&t->doc, t->data, t->length, HULL_SEXPR_CANONICAL, NULL) !=
		        HULL_SEXPR_OK ||
		    !hull_sexpr_first(&t->doc, &top) || top.kind != HULL_SEXPR_LIST ||
		    !hull_sexpr_child(&t->doc, &top, &head) || !hull_sexpr_is(&t->doc, &head, "manifest"))
			return "the tree's manifest is not one";
	}
	/* (entries (entry ...) ...): the first entry is the value after the
	 * key, and the rest its siblings; a tree of nothing has none. */
	any = field(t, &top, "entries", &first);
	for (entry = first; any; any = hull_sexpr_next(&t->doc, &entry, &entry))
		n++;
	t->nodes_size = ((size_t)n + 1u) * sizeof *t->nodes;
	if ((t->nodes = map(t->nodes_size)) == NULL)
		return "out of memory";
	memset(&t->nodes[0], 0, sizeof t->nodes[0]);
	t->nodes[0].mode = S_IFDIR | 0755;
	t->count = 1u;
	any = n > 0u;
	for (entry = first; any; any = hull_sexpr_next(&t->doc, &entry, &entry)) {
		const char *wrong = entry_of(t, &entry, &t->nodes[t->count]);

		if (wrong != NULL)
			return wrong;
		t->count++;
	}
	/* Each node under its parent, in order: a manifest is sorted by path,
	 * so a parent comes before what it holds. */
	for (uint32_t i = 1u; i < t->count; i++) {
		node    *e = &t->nodes[i];
		uint32_t cut = e->path_length;
		uint32_t parent;

		while (cut > 0u && t->data[e->path + cut - 1u] != '/')
			cut--;
		parent = cut == 0u ? 0u : find(t, t->data + e->path, cut - 1u);
		if ((cut != 0u && parent == 0u) || !S_ISDIR(t->nodes[parent].mode))
			return "a manifest's entry has no directory above it";
		e->parent = parent;
		if (t->nodes[parent].first_child == 0u)
			t->nodes[parent].first_child = i;
		else
			t->nodes[t->nodes[parent].last_child].next = i;
		t->nodes[parent].last_child = i;
	}
	return NULL;
}

/* The object of a regular file, opened, or -1: objects/sha256/ab/abcdef... */
static int open_object(const tree *t, const node *e)
{
	static const char digits[] = "0123456789abcdef";
	char              name[2u + 1u + 64u + 1u];

	if (!e->has_object)
		return -1;
	for (unsigned i = 0u; i < 32u; i++) {
		name[3u + 2u * i] = digits[e->digest[i] >> 4];
		name[3u + 2u * i + 1u] = digits[e->digest[i] & 15u];
	}
	name[0] = name[3];
	name[1] = name[4];
	name[2] = '/';
	name[67] = '\0';
	return openat(t->objects, name, O_RDONLY | O_CLOEXEC);
}

/* --- the FUSE protocol, read-only -------------------------------------- */

typedef struct server {
	tree    t;
	int     fuse;
	uint8_t *in;
	uint8_t *out;    /* a reply's payload, up to a read's most */
} server;

static void reply(server *s, uint64_t unique, int error, const void *data, size_t length)
{
	struct fuse_out_header h = { .len = (uint32_t)(sizeof h + (error == 0 ? length : 0u)),
	                             .error = error, .unique = unique };
	struct iovec           v[2] = { { &h, sizeof h }, { (void *)data, error == 0 ? length : 0u } };

	/* ENOENT: the request was interrupted and is gone; nothing to do. */
	while (writev(s->fuse, v, data != NULL && error == 0 && length > 0u ? 2 : 1) < 0 &&
	       errno == EINTR)
		;
}

static void attr_of(const tree *t, uint32_t i, struct fuse_attr *a)
{
	const node *e = &t->nodes[i];

	memset(a, 0, sizeof *a);
	a->ino = (uint64_t)i + 1u;
	a->size = e->size;
	a->blocks = (e->size + 511u) / 512u;
	a->mode = e->mode;
	a->nlink = S_ISDIR(e->mode) ? 2u : 1u;
	a->uid = e->uid;
	a->gid = e->gid;
	a->rdev = (uint32_t)e->rdev;
	a->blksize = 4096u;
}

/* The node a request's nodeid names, or -1 where it names none. */
static int64_t nodeid(const server *s, uint64_t id)
{
	return id >= 1u && id <= s->t.count ? (int64_t)(id - 1u) : -1;
}

static void lookup(server *s, const struct fuse_in_header *h, const char *name, size_t size)
{
	int64_t              parent = nodeid(s, h->nodeid);
	struct fuse_entry_out out;
	uint8_t              path[4200];
	size_t               at = 0u;
	size_t               length = strnlen(name, size);
	uint32_t             found;

	if (parent < 0 || !S_ISDIR(s->t.nodes[parent].mode)) {
		reply(s, h->unique, -ENOTDIR, NULL, 0u);
		return;
	}
	if (s->t.nodes[parent].path_length + 1u + length > sizeof path) {
		reply(s, h->unique, -ENAMETOOLONG, NULL, 0u);
		return;
	}
	if (parent != 0) {
		memcpy(path, s->t.data + s->t.nodes[parent].path, s->t.nodes[parent].path_length);
		at = s->t.nodes[parent].path_length;
		path[at++] = '/';
	}
	memcpy(path + at, name, length);
	found = find(&s->t, path, at + length);
	if (found == 0u) {
		reply(s, h->unique, -ENOENT, NULL, 0u);
		return;
	}
	memset(&out, 0, sizeof out);
	out.nodeid = (uint64_t)found + 1u;
	out.entry_valid = TTL;
	out.attr_valid = TTL;
	attr_of(&s->t, found, &out.attr);
	reply(s, h->unique, 0, &out, sizeof out);
}

static void readdir_of(server *s, const struct fuse_in_header *h, const struct fuse_read_in *in)
{
	int64_t  dir = nodeid(s, h->nodeid);
	size_t   used = 0u;
	uint64_t at = 0u;
	uint32_t child;

	if (dir < 0 || !S_ISDIR(s->t.nodes[dir].mode)) {
		reply(s, h->unique, -ENOTDIR, NULL, 0u);
		return;
	}
	/* Offsets: 0 ".", 1 "..", then each child in order. */
	child = s->t.nodes[dir].first_child;
	for (at = 0u; ; at++) {
		const char         *name;
		size_t              length;
		uint64_t            ino;
		uint32_t            type;
		struct fuse_dirent *d;
		size_t              size;

		if (at == 0u) {
			name = ".";
			length = 1u;
			ino = (uint64_t)dir + 1u;
			type = S_IFDIR >> 12;
		} else if (at == 1u) {
			name = "..";
			length = 2u;
			ino = (uint64_t)s->t.nodes[dir].parent + 1u;
			type = S_IFDIR >> 12;
		} else {
			const node *e;
			uint32_t    cut;

			if (at > 2u)
				child = s->t.nodes[child].next;
			if (child == 0u)
				break;
			e = &s->t.nodes[child];
			for (cut = e->path_length; cut > 0u && s->t.data[e->path + cut - 1u] != '/'; cut--)
				;
			name = (const char *)s->t.data + e->path + cut;
			length = e->path_length - cut;
			ino = (uint64_t)child + 1u;
			type = e->mode >> 12;
		}
		if (at < in->offset)
			continue;
		size = FUSE_DIRENT_ALIGN(FUSE_NAME_OFFSET + length);
		if (used + size > in->size || used + size > PAGES * 4096u)
			break;
		d = (struct fuse_dirent *)(void *)(s->out + used);
		d->ino = ino;
		d->off = at + 1u;
		d->namelen = (uint32_t)length;
		d->type = type;
		memcpy(d->name, name, length);
		memset(d->name + length, 0, size - FUSE_NAME_OFFSET - length);
		used += size;
	}
	reply(s, h->unique, 0, s->out, used);
}

static void xattrs(server *s, const struct fuse_in_header *h, const char *name, uint32_t size,
                   int list)
{
	int64_t         i = nodeid(s, h->nodeid);
	hull_sexpr_form pair;
	size_t          used = 0u;
	int             any;

	if (i < 0) {
		reply(s, h->unique, -ENOENT, NULL, 0u);
		return;
	}
	any = s->t.nodes[i].has_xattrs;
	pair = s->t.nodes[i].xattr;
	for (; any; any = hull_sexpr_next(&s->t.doc, &pair, &pair)) {
		hull_sexpr_form key;
		hull_sexpr_form value;

		if (pair.kind != HULL_SEXPR_LIST || !hull_sexpr_child(&s->t.doc, &pair, &key) ||
		    !hull_sexpr_next(&s->t.doc, &key, &value))
			continue;
		if (list) {
			if (used + key.content_length + 1u > PAGES * 4096u)
				break;
			memcpy(s->out + used, s->t.data + key.content, key.content_length);
			used += key.content_length;
			s->out[used++] = '\0';
		} else if (strlen(name) == key.content_length &&
		           memcmp(name, s->t.data + key.content, key.content_length) == 0) {
			struct fuse_getxattr_out out = { .size = value.content_length };

			if (size == 0u)
				reply(s, h->unique, 0, &out, sizeof out);
			else if (size < value.content_length)
				reply(s, h->unique, -ERANGE, NULL, 0u);
			else
				reply(s, h->unique, 0, s->t.data + value.content, value.content_length);
			return;
		}
	}
	if (!list) {
		reply(s, h->unique, -ENODATA, NULL, 0u);
		return;
	}
	if (size == 0u) {
		struct fuse_getxattr_out out = { .size = (uint32_t)used };

		reply(s, h->unique, 0, &out, sizeof out);
	} else if (size < used) {
		reply(s, h->unique, -ERANGE, NULL, 0u);
	} else {
		reply(s, h->unique, 0, s->out, used);
	}
}

static void handle(server *s, const struct fuse_in_header *h, const uint8_t *arg, size_t size)
{
	int64_t i = nodeid(s, h->nodeid);

	switch (h->opcode) {
	case FUSE_INIT: {
		const struct fuse_init_in *in = (const void *)arg;
		struct fuse_init_out       out;

		memset(&out, 0, sizeof out);
		out.major = FUSE_KERNEL_VERSION;
		out.minor = FUSE_KERNEL_MINOR_VERSION;
		if (in->major < 7u) {
			reply(s, h->unique, -EPROTO, NULL, 0u);
			return;
		}
		out.max_readahead = in->max_readahead;
		out.flags = in->flags & (FUSE_ASYNC_READ | FUSE_MAX_PAGES);
		out.max_write = 4096u;
		out.max_pages = PAGES;
		out.time_gran = 1u;
		reply(s, h->unique, 0, &out, sizeof out);
		return;
	}
	case FUSE_LOOKUP:
		lookup(s, h, (const char *)arg, size);
		return;
	case FUSE_FORGET:
	case FUSE_BATCH_FORGET:
	case FUSE_INTERRUPT:
		return;   /* no reply */
	case FUSE_GETATTR: {
		struct fuse_attr_out out;

		if (i < 0) {
			reply(s, h->unique, -ENOENT, NULL, 0u);
			return;
		}
		memset(&out, 0, sizeof out);
		out.attr_valid = TTL;
		attr_of(&s->t, (uint32_t)i, &out.attr);
		reply(s, h->unique, 0, &out, sizeof out);
		return;
	}
	case FUSE_READLINK:
		if (i < 0 || !S_ISLNK(s->t.nodes[i].mode))
			reply(s, h->unique, -EINVAL, NULL, 0u);
		else
			reply(s, h->unique, 0, s->t.data + s->t.nodes[i].target,
			      s->t.nodes[i].target_length);
		return;
	case FUSE_OPEN: {
		const struct fuse_open_in *in = (const void *)arg;
		struct fuse_open_out       out;
		int                        fd;

		if (i < 0 || !S_ISREG(s->t.nodes[i].mode)) {
			reply(s, h->unique, i < 0 ? -ENOENT : -EISDIR, NULL, 0u);
			return;
		}
		if ((in->flags & O_ACCMODE) != O_RDONLY) {
			reply(s, h->unique, -EROFS, NULL, 0u);
			return;
		}
		fd = open_object(&s->t, &s->t.nodes[i]);
		if (fd < 0 && s->t.nodes[i].size != 0u) {
			reply(s, h->unique, -EIO, NULL, 0u);
			return;
		}
		memset(&out, 0, sizeof out);
		out.fh = (uint64_t)(int64_t)fd;
		out.open_flags = FOPEN_KEEP_CACHE;
		reply(s, h->unique, 0, &out, sizeof out);
		return;
	}
	case FUSE_READ: {
		const struct fuse_read_in *in = (const void *)arg;
		int                        fd = (int)(int64_t)in->fh;
		size_t                     want = in->size < PAGES * 4096u ? in->size : PAGES * 4096u;
		ssize_t                    got = 0;

		if (fd >= 0)
			while ((got = pread(fd, s->out, want, (off_t)in->offset)) < 0 && errno == EINTR)
				;
		if (got < 0)
			reply(s, h->unique, -EIO, NULL, 0u);
		else
			reply(s, h->unique, 0, s->out, (size_t)got);
		return;
	}
	case FUSE_RELEASE: {
		const struct fuse_release_in *in = (const void *)arg;

		if ((int64_t)in->fh >= 0)
			close((int)(int64_t)in->fh);
		reply(s, h->unique, 0, NULL, 0u);
		return;
	}
	case FUSE_OPENDIR: {
		struct fuse_open_out out;

		if (i < 0 || !S_ISDIR(s->t.nodes[i].mode)) {
			reply(s, h->unique, i < 0 ? -ENOENT : -ENOTDIR, NULL, 0u);
			return;
		}
		memset(&out, 0, sizeof out);
		out.open_flags = FOPEN_KEEP_CACHE | FOPEN_CACHE_DIR;
		reply(s, h->unique, 0, &out, sizeof out);
		return;
	}
	case FUSE_READDIR:
		readdir_of(s, h, (const void *)arg);
		return;
	case FUSE_RELEASEDIR:
	case FUSE_FLUSH:
	case FUSE_FSYNC:
	case FUSE_FSYNCDIR:
		reply(s, h->unique, 0, NULL, 0u);
		return;
	case FUSE_ACCESS: {
		const struct fuse_access_in *in = (const void *)arg;

		reply(s, h->unique, (in->mask & W_OK) != 0 ? -EROFS : 0, NULL, 0u);
		return;
	}
	case FUSE_STATFS: {
		struct fuse_statfs_out out;

		memset(&out, 0, sizeof out);
		out.st.blocks = 0u;
		out.st.files = s->t.count;
		out.st.bsize = 4096u;
		out.st.frsize = 4096u;
		out.st.namelen = 255u;
		reply(s, h->unique, 0, &out, sizeof out);
		return;
	}
	case FUSE_GETXATTR: {
		const struct fuse_getxattr_in *in = (const void *)arg;

		xattrs(s, h, (const char *)(in + 1), in->size, 0);
		return;
	}
	case FUSE_LISTXATTR: {
		const struct fuse_getxattr_in *in = (const void *)arg;

		xattrs(s, h, NULL, in->size, 1);
		return;
	}
	case FUSE_DESTROY:
		reply(s, h->unique, 0, NULL, 0u);
		return;
	default:
		reply(s, h->unique, -ENOSYS, NULL, 0u);
		return;
	}
}

int hull_storefs_serve(const char *store, const char *hex, int fuse,
                       void (*ready)(void *context), void *context, char *why, size_t cap)
{
	server      s;
	const char *wrong;

	memset(&s, 0, sizeof s);
	s.fuse = fuse;
	if ((wrong = load(&s.t, store, hex)) != NULL ||
	    (s.in = map(REQUEST)) == NULL || (s.out = map(PAGES * 4096u)) == NULL) {
		snprintf(why, cap, "%s", wrong != NULL ? wrong : "out of memory");
		return -1;
	}
	if (ready != NULL)
		ready(context);
	for (;;) {
		ssize_t n = read(fuse, s.in, REQUEST);

		if (n < 0 && (errno == EINTR || errno == ENOENT || errno == EAGAIN))
			continue;
		if (n < 0)
			return errno == ENODEV ? 0 : -1;   /* unmounted */
		if ((size_t)n < sizeof(struct fuse_in_header))
			continue;
		handle(&s, (const struct fuse_in_header *)(void *)s.in,
		       s.in + sizeof(struct fuse_in_header), (size_t)n - sizeof(struct fuse_in_header));
	}
}

int hull_storefs_extract(const char *store, const char *hex, const char *path, const char *dest,
                         char *why, size_t cap)
{
	tree        t;
	const char *wrong = load(&t, store, hex);
	uint32_t    found;
	int         in;
	int         out;
	int         status = 0;
	char        buffer[65536];
	ssize_t     n;

	if (wrong != NULL) {
		snprintf(why, cap, "%s", wrong);
		status = -1;
		goto done;
	}
	found = find(&t, (const uint8_t *)path, strlen(path));
	if (found == 0u || !S_ISREG(t.nodes[found].mode)) {
		status = 1;
		goto done;
	}
	if ((in = open_object(&t, &t.nodes[found])) < 0 && t.nodes[found].size != 0u) {
		snprintf(why, cap, "%s's object cannot be read", path);
		status = -1;
		goto done;
	}
	if ((out = open(dest, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644)) < 0) {
		snprintf(why, cap, "%s: %s", dest, strerror(errno));
		if (in >= 0)
			close(in);
		status = -1;
		goto done;
	}
	while (in >= 0 && (n = read(in, buffer, sizeof buffer)) > 0)
		if (write(out, buffer, (size_t)n) != n) {
			snprintf(why, cap, "%s: %s", dest, strerror(errno));
			status = -1;
			break;
		}
	if (in >= 0)
		close(in);
	if (close(out) != 0 && status == 0) {
		snprintf(why, cap, "%s: %s", dest, strerror(errno));
		status = -1;
	}
done:
	if (t.objects >= 0)
		close(t.objects);
	if (t.data != NULL)
		munmap((void *)t.data, t.length);
	if (t.nodes != NULL)
		munmap(t.nodes, t.nodes_size);
	return status;
}

long hull_storefs_read(const char *store, const char *hex, const char *path, char *buffer,
                       size_t cap)
{
	tree     t;
	uint32_t found;
	int      in;
	long     n = -1;

	if (cap == 0u)
		return -1;
	buffer[0] = '\0';
	if (load(&t, store, hex) == NULL &&
	    (found = find(&t, (const uint8_t *)path, strlen(path))) != 0u &&
	    S_ISREG(t.nodes[found].mode)) {
		if ((in = open_object(&t, &t.nodes[found])) >= 0) {
			ssize_t got;

			n = 0;
			while ((size_t)n < cap - 1u && (got = read(in, buffer + n, cap - 1u - (size_t)n)) > 0)
				n += got;
			buffer[n] = '\0';
			close(in);
		} else if (t.nodes[found].size == 0u) {
			n = 0;
		}
	}
	if (t.objects >= 0)
		close(t.objects);
	if (t.data != NULL)
		munmap((void *)t.data, t.length);
	if (t.nodes != NULL)
		munmap(t.nodes, t.nodes_size);
	return n;
}
