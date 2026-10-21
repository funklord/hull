/*
 * tree.h -- a directory as a tar stream, the same bytes for the same
 * tree, for the docker adapter's import (project.md section 4.9).
 *
 * POSIX ustar, with a pax header for a name, link, owner or size that
 * does not fit one. Entries come in bytewise order of their names, each
 * directory before what it holds, and carry the mode, owner and mtime on
 * disk and nothing else, so two writes of a tree that has not changed
 * are the same bytes, whatever order the filesystem lists it in.
 *
 * What it holds: directories, regular files, symlinks and fifos. A
 * hardlink is written as a file of its own. A socket is left out, being
 * no file's content; a device node is refused, since a root daemon
 * extracting it would make the node. Nothing is followed: every entry is
 * opened from its directory's descriptor, never by a path that a symlink
 * could redirect.
 */

#ifndef HULL_TREE_H
#define HULL_TREE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Takes each piece of the stream in order; returns 0 to go on. */
typedef int (*hull_tree_sink)(void *context, const uint8_t *data, size_t n);

/*
 * Write the tree under `root` to `sink`. Returns 0, or -1 with why in
 * `detail`, naming the entry: one that cannot be read, a device node, a
 * file whose size changed while it was read, or the sink refusing.
 */
int hull_tree_tar(const char *root, hull_tree_sink sink, void *context, char *detail,
                  size_t detail_cap);

/* One entry of such a stream, for a writer that has no directory to walk,
 * as `hull push` writes a layer from a manifest: its name as the stream
 * holds it, a directory's ending in `/`; its type, '0' a file, '2' a
 * symlink, '3' and '4' a character and a block device, '5' a directory,
 * '6' a fifo; and its xattrs, as pax's SCHILY.xattr records. */
typedef struct hull_tar_entry {
	const char            *name;
	char                   type;
	unsigned               mode;
	unsigned long long     uid;
	unsigned long long     gid;
	unsigned long long     mtime;
	unsigned long long     size;
	const char            *link;
	unsigned               major;
	unsigned               minor;
	const char *const     *xattr_names;
	const uint8_t *const  *xattr_values;
	const size_t          *xattr_lengths;
	size_t                 xattr_count;
} hull_tar_entry;

/* The entry's header, a pax one before it where a field does not fit
 * ustar's or an xattr is given. A file's `size` bytes follow, then
 * hull_tar_pad(). 0, or -1 with `detail`. */
int hull_tar_header(hull_tree_sink sink, void *context, const hull_tar_entry *e, char *detail,
                    size_t detail_cap);

/* The zeroes after `n` bytes of a file, to the end of its block. */
int hull_tar_pad(hull_tree_sink sink, void *context, unsigned long long n, char *detail,
                 size_t detail_cap);

/* The stream's end, two blocks of zeroes. */
int hull_tar_end(hull_tree_sink sink, void *context, char *detail, size_t detail_cap);

#ifdef __cplusplus
}
#endif

#endif /* HULL_TREE_H */
