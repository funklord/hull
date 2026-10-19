/*
 * sha256.h -- SHA-256 (FIPS 180-4), for naming content by its digest.
 * The docker adapter names a directory's import by it (project.md
 * section 4.9); the store's objects will be named the same way (3.5).
 */

#ifndef HULL_SHA256_H
#define HULL_SHA256_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct hull_sha256 {
	uint32_t state[8];
	uint64_t length;      /* bytes hashed so far */
	uint8_t  block[64];
	size_t   held;        /* bytes of `block` waiting */
} hull_sha256;

void hull_sha256_init(hull_sha256 *h);
void hull_sha256_update(hull_sha256 *h, const void *data, size_t n);
void hull_sha256_final(hull_sha256 *h, uint8_t digest[32]);

/* `digest` as 64 lowercase hex digits and a NUL. */
void hull_sha256_hex(const uint8_t digest[32], char out[65]);

#ifdef __cplusplus
}
#endif

#endif /* HULL_SHA256_H */
