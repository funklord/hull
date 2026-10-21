/*
 * pull.h -- section 3.5's `hull pull`: an image from an OCI registry, its
 * layers applied in order into a tree, the tree committed to the store.
 */

#ifndef HULL_PULL_H
#define HULL_PULL_H

#include <stddef.h>

#include "store.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * `image` as docker reads one: [REGISTRY/]REPOSITORY[:TAG][@sha256:HEX],
 * docker.io and `library/` where none is named, `latest` where no tag
 * is. An index is narrowed to linux on `arch`, OCI's name for it (amd64,
 * arm64, ...). Every manifest, the config and each layer is checked
 * against its digest as it comes, and a layer against its size. The tree
 * is committed as `ref`, with the config's defaults as A.4's runtime and
 * A.5's provenance: `builder`, the digests fetched and the registry. 0,
 * or -1 with `why`.
 */
int hull_pull(hull_store *store, const char *image, const char *ref, const char *arch,
              const char *builder, hull_commit *out, char *why, size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* HULL_PULL_H */
