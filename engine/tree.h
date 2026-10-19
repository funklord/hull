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

#ifdef __cplusplus
}
#endif

#endif /* HULL_TREE_H */
