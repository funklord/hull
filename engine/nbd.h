/*
 * nbd.h -- a disc or disk collection served to qemu by NBD, read where its
 * files lie (project.md section 3.13), no image made: read-only, qemu's
 * snapshot taking the machine's writes; or read-write, the writes kept by
 * the block in an overlay for the engine to take back into the files.
 */
#ifndef HULL_NBD_H
#define HULL_NBD_H

#include <pthread.h>
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

/* The guest's writes to a served disk, kept apart from its files: a sparse
 * file `overlay` as long as the disk, and a bit a block of HULL_NBD_BLOCK
 * bytes, set where the block is the overlay's -- filled from the map at
 * its first write, so a block is whole in one place or the other. */
#define HULL_NBD_BLOCK 4096u

typedef struct hull_nbd_writes {
	int       overlay;
	uint8_t  *written;  /* (bytes + HULL_NBD_BLOCK - 1) / HULL_NBD_BLOCK bits */
	uint64_t  count;    /* blocks written */
} hull_nbd_writes;

/* As hull_nbd_serve, read-write, the writes into `writes`: the thread
 * takes `fd` alone, and leaves `map` and `writes` the caller's, who joins
 * `*thread` before reading either -- once the client has gone. */
int hull_nbd_serve_writes(hull_media_map *map, uint64_t bytes, int fd, hull_nbd_writes *writes,
                          pthread_t *thread);

#ifdef __cplusplus
}
#endif

#endif
