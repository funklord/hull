/*
 * verity.c -- the fs-verity digest of a file's bytes; see verity.h.
 *
 * The tree: each 4096-byte data block, the last padded with zeros, hashed;
 * the hashes packed 128 to a block, each such block padded and hashed in
 * turn, until one block is left, whose hash is the root. A file of one
 * block has that block's hash as its root, an empty file zeros. The
 * digest is the hash of the 256-byte descriptor naming the root, the
 * algorithm, the block size and the file's size. The number of levels
 * follows from the size, so the tree is built as the bytes arrive, one
 * block of each level held at a time.
 */

#include <string.h>

#include "verity.h"

#define PER_BLOCK (HULL_VERITY_BLOCK / 32u)

static const uint8_t zeros[HULL_VERITY_BLOCK];

/* A hash into `at`, or the root where `at` is above the top level. */
static void add(hull_verity *v, unsigned at, const uint8_t hash[32])
{
	if (at == v->levels) {
		memcpy(v->root, hash, 32u);
		return;
	}
	memcpy(v->level[at] + v->level_held[at], hash, 32u);
	v->level_held[at] += 32u;
	if (v->level_held[at] == HULL_VERITY_BLOCK) {
		hull_sha256 h;
		uint8_t     up[32];

		hull_sha256_init(&h);
		hull_sha256_update(&h, v->level[at], HULL_VERITY_BLOCK);
		hull_sha256_final(&h, up);
		v->level_held[at] = 0u;
		add(v, at + 1u, up);
	}
}

static void end_block(hull_verity *v)
{
	uint8_t hash[32];

	if (v->block_held < HULL_VERITY_BLOCK)
		hull_sha256_update(&v->block, zeros, HULL_VERITY_BLOCK - v->block_held);
	hull_sha256_final(&v->block, hash);
	hull_sha256_init(&v->block);
	v->block_held = 0u;
	add(v, 0u, hash);
}

void hull_verity_init(hull_verity *v, uint64_t size)
{
	uint64_t blocks = (size + HULL_VERITY_BLOCK - 1u) / HULL_VERITY_BLOCK;

	memset(v->level_held, 0, sizeof v->level_held);
	memset(v->root, 0, sizeof v->root);
	v->size = size;
	v->fed = 0u;
	v->block_held = 0u;
	v->levels = 0u;
	/* A level for each packing of the hashes below it, until one block
	 * holds them: none where the data is one block. */
	while (blocks > 1u && v->levels < HULL_VERITY_LEVELS) {
		blocks = (blocks + PER_BLOCK - 1u) / PER_BLOCK;
		v->levels++;
	}
	hull_sha256_init(&v->block);
}

void hull_verity_update(hull_verity *v, const void *data, size_t n)
{
	const uint8_t *at = data;

	v->fed += n;
	while (n > 0u) {
		size_t take = HULL_VERITY_BLOCK - v->block_held;

		if (take > n)
			take = n;
		hull_sha256_update(&v->block, at, take);
		v->block_held += take;
		at += take;
		n -= take;
		if (v->block_held == HULL_VERITY_BLOCK)
			end_block(v);
	}
}

int hull_verity_final(hull_verity *v, uint8_t digest[32])
{
	uint8_t     descriptor[256];
	hull_sha256 h;

	if (v->fed != v->size)
		return -1;
	if (v->block_held > 0u)
		end_block(v);
	/* Each level's last, partial block, padded, lowest first, so the
	 * hash it gives lands in the level above before that one ends. */
	for (unsigned at = 0u; at < v->levels; at++) {
		if (v->level_held[at] > 0u) {
			uint8_t up[32];

			memset(v->level[at] + v->level_held[at], 0, HULL_VERITY_BLOCK - v->level_held[at]);
			hull_sha256_init(&h);
			hull_sha256_update(&h, v->level[at], HULL_VERITY_BLOCK);
			hull_sha256_final(&h, up);
			v->level_held[at] = 0u;
			add(v, at + 1u, up);
		}
	}
	/* struct fsverity_descriptor: version 1, SHA-256 (1), log2 of the
	 * block size, no salt, the size little-endian, the root. */
	memset(descriptor, 0, sizeof descriptor);
	descriptor[0] = 1u;
	descriptor[1] = 1u;
	descriptor[2] = 12u;
	for (unsigned i = 0u; i < 8u; i++)
		descriptor[8 + i] = (uint8_t)(v->size >> (8u * i));
	memcpy(descriptor + 16, v->root, 32u);
	hull_sha256_init(&h);
	hull_sha256_update(&h, descriptor, sizeof descriptor);
	hull_sha256_final(&h, digest);
	return 0;
}
