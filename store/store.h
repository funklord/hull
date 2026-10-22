/*
 * store.h -- section 3's optional store: a content-addressed object store,
 * manifests (Appendix A) naming trees of those objects, and refs naming
 * manifests, under $XDG_DATA_HOME/hull/store (~/.local/share/hull/store).
 *
 *     store/objects/sha256/ab/abcdef...   content, 0444, or 0555 if executable
 *     store/manifests/sha256/abcdef...    a manifest's canonical bytes, 0444
 *     store/refs/NAME                     a symlink to ../manifests/sha256/...
 *     store/checkouts/sha256/abcdef...    a manifest's tree, made once, for a
 *                                         machine whose source is a ref
 *
 * Nothing else in hull needs it: dir sources are the primary mode (3.7).
 */

#ifndef HULL_STORE_H
#define HULL_STORE_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct hull_store {
	char root[4096];  /* .../hull/store */
	int  lock;        /* store/lock, flock()ed by what changes the store */
} hull_store;

/* What a commit made, for the frontend to say. */
typedef struct hull_commit {
	char     manifest[65];  /* the manifest's sha256, in hex */
	uint64_t entries;
	uint64_t objects;       /* distinct objects the tree has */
	uint64_t new_objects;   /* of them, the ones the store did not have */
	uint64_t total_size;
	uint64_t skipped;       /* entries a manifest cannot hold, each said */
} hull_commit;

/* The store, made if it is not there and `create` says to: 0; 1 when it
 * is not there and is not to be made; or -1 with `why`. */
int hull_store_open(hull_store *store, int create, char *why, size_t cap);
void hull_store_close(hull_store *store);

/* What a manifest may carry beside its tree, each a canonical form or
 * NULL: A.5's `(10:provenance ...)` and A.4's `(7:runtime ...)`. */
typedef struct hull_commit_extra {
	const uint8_t *provenance;
	size_t         provenance_length;
	const uint8_t *runtime;
	size_t         runtime_length;
} hull_commit_extra;

/* `dir` as a manifest of `arch`, its files into the objects, named `ref`,
 * or named nothing where `ref` is NULL, with `extra` where it is not NULL.
 * 0, or -1 with `why`. A file this cannot hold is said on stderr and left
 * out, counted in `skipped`. A manifest no ref names is gc's to take once
 * nothing holds the store (hull_store_hold()). */
int hull_store_commit(hull_store *store, const char *dir, const char *arch, const char *ref,
                      const hull_commit_extra *extra, hull_commit *out, char *why, size_t cap);

/* The manifest `name` names, a ref or sha256:HEX, which the store holds,
 * read whole and checked: its hex into `hex`. 0, or -1 with `why`. */
int hull_store_resolve(hull_store *store, const char *name, char hex[65], char *why, size_t cap);

/* The manifest `name` names, with `runtime`, a canonical `(7:runtime
 * ...)`, in place of its own or where one goes: its entries and its
 * provenance as they were. Stored, named by no ref, its hex into `hex`. 0,
 * or -1 with `why`. */
int hull_store_with_runtime(hull_store *store, const char *name, const uint8_t *runtime,
                            size_t runtime_length, char hex[65], char *why, size_t cap);

/* The store held, shared, through a descriptor of its own, so that gc
 * waits however many operations come and go meanwhile, as a build needs
 * for the manifests it makes before a ref names them: the descriptor, or
 * -1 with `why`. hull_store_release() lets it go. */
int hull_store_hold(hull_store *store, char *why, size_t cap);
void hull_store_release(int held);

/* A.4's runtime defaults of the manifest `name` names, a ref or
 * sha256:HEX, as their canonical form, malloc'd into `*out`; `*out` is
 * NULL where it has none. 0, or -1 with `why`. */
int hull_store_runtime(hull_store *store, const char *name, uint8_t **out, size_t *length,
                       char *why, size_t cap);

/* The tree `ref` names, a ref or sha256:HEX, made at `dir`, which must not
 * exist or be empty.
 * What the user cannot make rootless -- a device, another owner, a
 * security.* xattr -- is said on stderr, counted in `*skipped`, and left
 * as the user can. 0, or -1 with `why`. */
int hull_store_materialize(hull_store *store, const char *ref, const char *dir,
                           uint64_t *skipped, char *why, size_t cap);

/* What differs from the tree `from` names to the tree `to` names, each a
 * ref or sha256:HEX: each path added, '+', removed, '-', or changed, '~',
 * in path order, through `each`, whose `what` says what changed in a
 * path, as "mode 0644 -> 0755, content". A NULL path is the tree's own,
 * its arch. 0 when nothing differs, 1 when something does, or -1 with
 * `why`. */
int hull_store_diff(hull_store *store, const char *from, const char *to,
                    void (*each)(void *context, char change, const char *path, const char *what),
                    void *context, char *why, size_t cap);

/* `ref` named the manifest `from` names, a ref or sha256:HEX, which the
 * store must hold; a ref already there is replaced. Its hex into `hex`.
 * 0, or -1 with `why`. */
int hull_store_tag(hull_store *store, const char *from, const char *ref, char hex[65], char *why,
                   size_t cap);

/* `ref` removed; what it named stays, for gc. 0, or -1 with `why`. */
int hull_store_untag(hull_store *store, const char *ref, char *why, size_t cap);

/* A.1's readable view of the manifest `name` names, to `out`: lossless, so
 * that it reads back as the same canonical bytes, an entry a line. 0, or -1
 * with `why`. */
int hull_store_show(hull_store *store, const char *name, FILE *out, char *why, size_t cap);

/* Section 3.5's layers, applied in order into a tree to be committed: a
 * tar each, gzip-compressed or not. Each path is made beneath the tree,
 * its parents resolved in it, so that a symlink in a layer leads nowhere
 * outside, and a path with `..` is refused; a whiteout removes what a
 * lower layer made, an opaque one empties its directory of it. What the
 * user cannot make rootless, a device, an owner, an xattr outside user.*,
 * is said on stderr and counted in `*skipped`; every path is the user's.
 * Directories are kept writable until hull_unpack_end() gives them their
 * modes. */
typedef struct hull_unpack hull_unpack;

/* Into `dir`, which must exist and be empty: NULL, with `why`, if not. */
hull_unpack *hull_unpack_begin(const char *dir, char *why, size_t cap);

/* The layer `fd` reads, from its start: 0, or -1 with `why`. */
int hull_unpack_layer(hull_unpack *u, int fd, uint64_t *skipped, char *why, size_t cap);

/* The directories given their modes, and `u` freed, whatever came before:
 * 0, or -1 with `why`. */
int hull_unpack_end(hull_unpack *u, char *why, size_t cap);

/* The tree `name` names, a ref or sha256:HEX, as one OCI layer, an
 * uncompressed tar, to `sink`, which takes it a piece at a time and
 * returns 0 to go on: the manifest's entries in its order, each directory
 * before what it holds, every mtime 0, an owner `root` as 0 and an explicit
 * one as its ids, xattrs as pax records, and each object checked against
 * its name as it goes. The manifest's arch into `arch`. 0, or -1 with
 * `why`, an owner of class `user`, which has no number in an image, among
 * the refusals. */
int hull_store_layer(hull_store *store, const char *name,
                     int (*sink)(void *context, const uint8_t *data, size_t n), void *context,
                     char arch[64], char *why, size_t cap);

/* `name` in `parent`, and all beneath it, each directory made the user's
 * to empty first, since a tree's modes may not let it be, and no symlink
 * followed: as gc removes a checkout, for what a build materialized. 0, or
 * -1 when something stayed. */
int hull_store_remove_tree(int parent, const char *name);

/* Each ref and the manifest it names, sorted by ref. */
int hull_store_list(hull_store *store, void (*each)(void *context, const char *ref,
                                                    const char *manifest),
                    void *context, char *why, size_t cap);

/* The tree `name`, a ref or sha256:HEX, checked out whole once per
 * manifest, at checkouts/sha256/HEX, its path into `path`: what a machine
 * whose source is a ref runs, as a dir source under an overlay, so that
 * nothing writes it. Without `create`, only the path, made or not. What
 * the user cannot make rootless is said, as by materializing, and counted
 * in `*skipped`. 0, or -1 with `why`. */
int hull_store_checkout(hull_store *store, const char *name, int create, char *path,
                        size_t path_cap, uint64_t *skipped, char *why, size_t cap);

/* Section 3.4: what no ref reaches, removed: manifests, objects, and the
 * checkouts of those manifests. */
int hull_store_gc(hull_store *store, uint64_t *manifests, uint64_t *objects,
                  uint64_t *checkouts, char *why, size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* HULL_STORE_H */
