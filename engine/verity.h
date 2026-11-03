/*
 * verity.h -- a file's fs-verity digest, computed as the kernel computes
 * it (Documentation/filesystems/fsverity.rst), so the store can write
 * the digest composefs's metacopy carries without a filesystem that
 * supports verity: SHA-256, 4096-byte blocks, no salt (project.md
 * section 3.2).
 */

#ifndef HULL_VERITY_H
#define HULL_VERITY_H

#include <stddef.h>
#include <stdint.h>

#include "sha256.h"

#ifdef __cplusplus
extern "C" {
#endif

#define HULL_VERITY_BLOCK  4096u
#define HULL_VERITY_LEVELS 8u      /* 128^8 blocks: more than any file */

typedef struct hull_verity {
	uint64_t    size;                 /* the file's, given up front */
	uint64_t    fed;                  /* bytes given so far */
	unsigned    levels;               /* the tree's, 0 for a file of one block or none */
	hull_sha256 block;                /* the data block being hashed */
	size_t      block_held;
	uint8_t     level[HULL_VERITY_LEVELS][HULL_VERITY_BLOCK];
	size_t      level_held[HULL_VERITY_LEVELS];
	uint8_t     root[32];
} hull_verity;

/* For a file of `size` bytes, which `update` must then give exactly. */
void hull_verity_init(hull_verity *v, uint64_t size);
void hull_verity_update(hull_verity *v, const void *data, size_t n);
/* 0 and the digest, or -1 where `update` gave other than `size` bytes. */
int hull_verity_final(hull_verity *v, uint8_t digest[32]);

#ifdef __cplusplus
}
#endif

#endif /* HULL_VERITY_H */
