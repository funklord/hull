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

/* `dir` as a manifest of `arch`, its files into the objects, named `ref`.
 * 0, or -1 with `why`. A file this cannot hold is said on stderr and
 * left out, counted in `skipped`. */
int hull_store_commit(hull_store *store, const char *dir, const char *arch, const char *ref,
                      hull_commit *out, char *why, size_t cap);

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
