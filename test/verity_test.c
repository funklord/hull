/*
 * verity_test.c -- the fs-verity digest against fsverity-utils' own
 * (`fsverity digest`, 1.6), over sizes at each edge of the tree: none, a
 * part block, one block, one past it, a full block of hashes and one past
 * that, and two full levels and one past them, which takes a third. Each
 * fed in pieces of an awkward size, so a block is never given whole.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "verity.h"

static unsigned cases;
static unsigned failures;

/* The pattern the vectors were made from: byte i is i*7 + i/4096. */
static uint8_t at(uint64_t i)
{
	return (uint8_t)(i * 7u + i / 4096u);
}

static void check(uint64_t size, const char *want)
{
	static hull_verity v;
	uint8_t            piece[3001];
	uint8_t            digest[32];
	char               hex[65];

	cases++;
	hull_verity_init(&v, size);
	for (uint64_t done = 0u; done < size;) {
		size_t n = size - done < sizeof piece ? (size_t)(size - done) : sizeof piece;

		for (size_t i = 0u; i < n; i++)
			piece[i] = at(done + i);
		hull_verity_update(&v, piece, n);
		done += n;
	}
	if (hull_verity_final(&v, digest) != 0) {
		failures++;
		printf("FAIL %llu bytes: refused\n", (unsigned long long)size);
		return;
	}
	hull_sha256_hex(digest, hex);
	if (strcmp(hex, want) != 0) {
		failures++;
		printf("FAIL %llu bytes: %s, wanted %s\n", (unsigned long long)size, hex, want);
	}
}

int main(void)
{
	static hull_verity v;
	uint8_t            digest[32];

	check(0u, "3d248ca542a24fc62d1c43b916eae5016878e2533c88238480b26128a1f1af95");
	check(1u, "b803429503d95915829b29fdbc8bbad142f3abfd11b1cadf5526582e685c0551");
	check(4095u, "b37a93497ff6ed2821409ced1dc20f4c1002cc361adf2f22ec444a22c06fe857");
	check(4096u, "87a706bdba32be89a173dc11d6a91c08165b4dcfcf743988c94595e82934a41a");
	check(4097u, "5c6077f10edc31c27b2a65cbaa199e2cfd36998bf104ccd2f7395eb509682064");
	check(8192u, "870c5a8845344e284913be24101260588059e053bc58a58aa1fb4072b4d486f2");
	check(524288u, "9613b285b37589320574d16f1447444fb68289259e661f4418dc6374ef3c5b9f");
	check(524289u, "355acec90f25617bed6ee0e57380add05b4abb6855b8c7a9224c142cf027a98a");
	check(528389u, "58409609e3ccadf76f6878b729b44ab968307dc6dac89969e37c3211b1cbc6cd");
	check(67108864u, "d87af4dff916f4cefac8587cf6c2444afe8b0d1597385eac0ba1c2426f8b5e67");
	check(67108865u, "d2c0354a967d44d8061ca1ddd2170a11ef0d8df7cecde45312af9602773a4022");

	/* Fewer bytes than the size given: refused, not a digest of them. */
	cases++;
	hull_verity_init(&v, 10u);
	hull_verity_update(&v, "abc", 3u);
	if (hull_verity_final(&v, digest) == 0) {
		failures++;
		printf("FAIL a short file gave a digest\n");
	}

	printf("verity: %u cases, %u failed\n", cases, failures);
	return failures == 0u ? EXIT_SUCCESS : EXIT_FAILURE;
}
