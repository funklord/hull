/*
 * push.h -- section 3.5's `hull push`: a stored tree to an OCI registry,
 * as a single-layer image.
 */

#ifndef HULL_PUSH_H
#define HULL_PUSH_H

#include <stddef.h>

#include "store.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The tree `name` names, a ref or sha256:HEX, pushed as `image`, named
 * as hull_pull() reads one and with a tag: one layer, the tree's tar from
 * its manifest, gzip-compressed the same way each time, so that one tree
 * is one image; a config of the manifest's arch and A.4 runtime; and the
 * image manifest, under the tag. A blob the registry has is not sent
 * again. The image manifest's digest into `digest`. 0, or -1 with `why`.
 */
int hull_push(hull_store *store, const char *name, const char *image, char digest[80], char *why,
              size_t cap);

/*
 * The same tree published object by object (Appendix C), beside the image
 * hull_push() made: an OCI manifest under the tag TAG.hull, whose config
 * is the tree's canonical manifest and whose layers are its objects, each
 * a blob of its raw bytes, so that a blob's digest is the object's name in
 * the store. A registry dedups them as it does any blob, so a new version
 * sends only the objects the last one did not have, and hull_pull_objects
 * fetches only those a store lacks. An object of HULL_CHUNK_FLOOR bytes
 * or more goes as its chunks (hull_chunk_length), each a blob annotated
 * HULL_CHUNK_OF with the object's digest, in order, so a large file
 * changed in one place sends one chunk. How many blobs were sent into
 * `*sent`, how many layers there are into `*total`. 0, or -1 with `why`.
 */
int hull_push_objects(hull_store *store, const char *name, const char *image, unsigned *sent,
                      unsigned *total, char *why, size_t cap);

#define HULL_OBJECTS_SUFFIX   ".hull"
#define HULL_TREE_TYPE        "application/vnd.hull.tree.v1"
#define HULL_MANIFEST_TYPE    "application/vnd.hull.manifest.v1"
#define HULL_OBJECT_TYPE      "application/vnd.hull.object.v1"
#define HULL_EXECUTABLE_TYPE  "application/vnd.hull.object.executable.v1"
#define HULL_CHUNK_TYPE       "application/vnd.hull.chunk.v1"
#define HULL_CHUNK_EXECUTABLE_TYPE "application/vnd.hull.chunk.executable.v1"
#define HULL_CHUNK_OF         "vnd.hull.object"

#ifdef __cplusplus
}
#endif

#endif /* HULL_PUSH_H */
