/*
 * storefs.h -- a stored tree served read-only by FUSE, from its manifest
 * and the store's objects (project.md section 3.2): composefs's tree,
 * read by hull itself, for an engine whose machine cannot mount EROFS --
 * chroot, in a user namespace, which may mount FUSE and an overlay over
 * it but no block filesystem. No composefs tool is packaged for Debian 13,
 * and composefs-fuse is not among them.
 *
 * The manifest is what the image is made of, so serving it gives the
 * image's tree: each entry's mode, owner, size, symlink target, device and
 * user.* and security.* xattrs as committed, and a regular file's bytes
 * from its object, objects/sha256/ab/abcdef..., opened read-only. Times
 * are 0, as the image's are.
 *
 * hull_storefs_serve runs in a child of a machine's init, which is a raw
 * clone of the multithreaded core: it allocates by mmap alone, never
 * malloc, whose locks another core thread may have held at the clone.
 */
#ifndef HULL_STOREFS_H
#define HULL_STOREFS_H

#include <stddef.h>

/* The store's directory and the manifest's hex from a composefs image's
 * path, STORE/mounts/HEX.erofs or HEX.N.erofs. 0, or -1 where it is not
 * one. */
int hull_storefs_parts(const char *image, char *store, size_t store_cap, char hex[65]);

/* Each of the functions below takes, with `hex` NULL, `store` as the path
 * of a pack of version 1 (section 3.13): its tree is its manifest and its
 * files are its objects' frames, each decompressed as it is opened into a
 * memfd and held to its digest, the pack read into no store.
 *
 * Serve the tree of the manifest `hex` under `store` on `fuse`, a
 * /dev/fuse descriptor already mounted, until it is unmounted. Writes
 * why it could not start into `why`, and returns -1, before serving;
 * returns 0 once the mount has gone. `ready` is called, once the
 * manifest is read, before the first request is taken.
 *
 * With `verity`, (security (verity true)), each file's bytes are checked
 * as fs-verity checks them, against a tree of hashes anchored in what the
 * manifest names it by: an open reads its object whole, hashing each
 * 4096-byte block and the whole, and fails EIO where the whole is not its
 * sha256; each read then hashes the blocks it returns against those, and
 * fails EIO where one has changed since. */
int hull_storefs_serve(const char *store, const char *hex, int fuse, int verity,
                       void (*ready)(void *context), void *context, char *why, size_t cap);

/* The bytes of `path` in the manifest `hex` under `store`, a regular
 * file, following no symlink, written to `dest`, mode 0644. 0; 1 where
 * the tree has no such file; -1 with `why`. Allocates: for the core, not
 * for a child of a clone. */
int hull_storefs_extract(const char *store, const char *hex, const char *path, const char *dest,
                         char *why, size_t cap);

/* The bytes of `path` in the manifest `hex` under `store`, a regular
 * file, into `buffer`, at most `cap` - 1 of them and a NUL after: how
 * many, or -1 where the tree has no such file or it cannot be read.
 * Allocates, as hull_storefs_extract does. */
long hull_storefs_read(const char *store, const char *hex, const char *path, char *buffer,
                       size_t cap);

#endif
