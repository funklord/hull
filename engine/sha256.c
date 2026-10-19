/*
 * sha256.c -- SHA-256, as FIPS 180-4 states it.
 */

#include <stdio.h>
#include <string.h>

#include "sha256.h"

static const uint32_t k[64] = {
	0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u,
	0x923f82a4u, 0xab1c5ed5u, 0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
	0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u, 0xe49b69c1u, 0xefbe4786u,
	0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
	0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u,
	0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
	0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u, 0xa2bfe8a1u, 0xa81a664bu,
	0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
	0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au,
	0x5b9cca4fu, 0x682e6ff3u, 0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
	0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u,
};

static uint32_t rotate(uint32_t x, unsigned n)
{
	return (x >> n) | (x << (32u - n));
}

static void compress(hull_sha256 *h, const uint8_t *block)
{
	uint32_t w[64];
	uint32_t v[8];

	for (unsigned i = 0u; i < 16u; i++)
		w[i] = (uint32_t)block[4u * i] << 24 | (uint32_t)block[4u * i + 1u] << 16 |
		       (uint32_t)block[4u * i + 2u] << 8 | (uint32_t)block[4u * i + 3u];
	for (unsigned i = 16u; i < 64u; i++) {
		uint32_t s0 = rotate(w[i - 15u], 7u) ^ rotate(w[i - 15u], 18u) ^ (w[i - 15u] >> 3);
		uint32_t s1 = rotate(w[i - 2u], 17u) ^ rotate(w[i - 2u], 19u) ^ (w[i - 2u] >> 10);

		w[i] = w[i - 16u] + s0 + w[i - 7u] + s1;
	}
	memcpy(v, h->state, sizeof v);
	for (unsigned i = 0u; i < 64u; i++) {
		uint32_t s1 = rotate(v[4], 6u) ^ rotate(v[4], 11u) ^ rotate(v[4], 25u);
		uint32_t ch = (v[4] & v[5]) ^ (~v[4] & v[6]);
		uint32_t t1 = v[7] + s1 + ch + k[i] + w[i];
		uint32_t s0 = rotate(v[0], 2u) ^ rotate(v[0], 13u) ^ rotate(v[0], 22u);
		uint32_t mj = (v[0] & v[1]) ^ (v[0] & v[2]) ^ (v[1] & v[2]);

		v[7] = v[6];
		v[6] = v[5];
		v[5] = v[4];
		v[4] = v[3] + t1;
		v[3] = v[2];
		v[2] = v[1];
		v[1] = v[0];
		v[0] = t1 + s0 + mj;
	}
	for (unsigned i = 0u; i < 8u; i++)
		h->state[i] += v[i];
}

void hull_sha256_init(hull_sha256 *h)
{
	static const uint32_t initial[8] = {
		0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
		0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u,
	};

	memcpy(h->state, initial, sizeof initial);
	h->length = 0u;
	h->held   = 0u;
}

void hull_sha256_update(hull_sha256 *h, const void *data, size_t n)
{
	const uint8_t *p = data;

	h->length += n;
	if (h->held != 0u) {
		size_t take = 64u - h->held < n ? 64u - h->held : n;

		memcpy(h->block + h->held, p, take);
		h->held += take;
		p += take;
		n -= take;
		if (h->held < 64u)
			return;
		compress(h, h->block);
		h->held = 0u;
	}
	for (; n >= 64u; p += 64, n -= 64u)
		compress(h, p);
	memcpy(h->block, p, n);
	h->held = n;
}

void hull_sha256_final(hull_sha256 *h, uint8_t digest[32])
{
	uint64_t bits = h->length * 8u;

	h->block[h->held++] = 0x80u;
	if (h->held > 56u) {
		memset(h->block + h->held, 0, 64u - h->held);
		compress(h, h->block);
		h->held = 0u;
	}
	memset(h->block + h->held, 0, 56u - h->held);
	for (unsigned i = 0u; i < 8u; i++)
		h->block[56u + i] = (uint8_t)(bits >> (56u - 8u * i));
	compress(h, h->block);
	for (unsigned i = 0u; i < 8u; i++) {
		digest[4u * i]      = (uint8_t)(h->state[i] >> 24);
		digest[4u * i + 1u] = (uint8_t)(h->state[i] >> 16);
		digest[4u * i + 2u] = (uint8_t)(h->state[i] >> 8);
		digest[4u * i + 3u] = (uint8_t)h->state[i];
	}
}

void hull_sha256_hex(const uint8_t digest[32], char out[65])
{
	for (unsigned i = 0u; i < 32u; i++)
		snprintf(out + 2u * i, 3u, "%02x", digest[i]);
}
