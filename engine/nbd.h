/*
 * nbd.h -- a disc or disk collection served to qemu by NBD, read where its
 * files lie (project.md section 3.13): no image made, read-only, qemu's
 * snapshot taking the machine's writes.
 */
#ifndef HULL_NBD_H
#define HULL_NBD_H

#include <stdint.h>

#include "media.h"

#ifdef __cplusplus
extern "C" {
#endif

/* `map`, `bytes` long, served on the connected stream `fd` by a thread of
 * its own, which takes both and closes and frees them when the client
 * disconnects or its end closes. 0, or -1 where no thread could start --
 * and then the caller still owns both. */
int hull_nbd_serve(hull_media_map *map, uint64_t bytes, int fd);

#ifdef __cplusplus
}
#endif

#endif
