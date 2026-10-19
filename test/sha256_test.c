/*
 * sha256_test.c -- SHA-256 against FIPS 180-4's examples, and the same
 * input fed in pieces of every size meeting the same digest.
 */

#include <stdio.h>
#include <string.h>

#include "sha256.h"

static unsigned cases;
static unsigned failures;

static void check(const char *what, const char *input, size_t n, unsigned repeat,
                  const char *want)
{
	hull_sha256 h;
	uint8_t     digest[32];
	char        hex[65];

	cases++;
	hull_sha256_init(&h);
	for (unsigned i = 0u; i < repeat; i++)
		hull_sha256_update(&h, input, n);
	hull_sha256_final(&h, digest);
	hull_sha256_hex(digest, hex);
	if (strcmp(hex, want) != 0) {
		failures++;
		printf("FAIL %s: %s, wanted %s\n", what, hex, want);
	}
}

int main(void)
{
	static const char two_blocks[] =
	    "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
	const char *long_one =
	    "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1";
	hull_sha256 whole;
	uint8_t     want[32];

	check("empty", "", 0u, 1u,
	      "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
	check("abc", "abc", 3u, 1u,
	      "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
	check("448 bits", two_blocks, strlen(two_blocks), 1u, long_one);
	check("a million a", "a", 1u, 1000000u,
	      "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
	/* 55, 56 and 64 bytes: the lengths where the padding takes a block
	 * of its own or does not. */
	check("55 bytes", "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", 55u, 1u,
	      "9f4390f8d30c2dd92ec9f095b65e2b9ae9b0a925a5258e241c9f1e910f734318");
	check("56 bytes", "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", 56u, 1u,
	      "b35439a4ac6f0948b6d6f9e3c6af0f5f590ce20f1bde7090ef7970686ec6738a");
	check("64 bytes", "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", 64u, 1u,
	      "ffe054fe7ae0cb6dc65c3af9b61d5209f439851db43d0ba5997337df154668eb");

	/* The two-block example fed in pieces of 1 to 56 bytes. */
	hull_sha256_init(&whole);
	hull_sha256_update(&whole, two_blocks, strlen(two_blocks));
	hull_sha256_final(&whole, want);
	for (size_t piece = 1u; piece <= strlen(two_blocks); piece++) {
		hull_sha256 h;
		uint8_t     got[32];

		cases++;
		hull_sha256_init(&h);
		for (size_t at = 0u; at < strlen(two_blocks); at += piece)
			hull_sha256_update(&h, two_blocks + at,
			                   strlen(two_blocks) - at < piece ? strlen(two_blocks) - at : piece);
		hull_sha256_final(&h, got);
		if (memcmp(got, want, sizeof got) != 0) {
			failures++;
			printf("FAIL pieces of %zu\n", piece);
		}
	}
	printf("sha256: %u cases, %u failed\n", cases, failures);
	return failures != 0u;
}
