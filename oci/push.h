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

#ifdef __cplusplus
}
#endif

#endif /* HULL_PUSH_H */
