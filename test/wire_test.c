/*
 * wire_test.c -- the frontend protocol on a socket: what each side lets
 * through, held to core/frontend.situ, and the numbers it reads.
 */

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "wire.h"

static unsigned cases;
static unsigned failures;

#define FAIL(...) do { \
	failures++; \
	printf("FAIL %s:%d: ", __FILE__, __LINE__); \
	printf(__VA_ARGS__); \
	printf("\n"); \
} while (0)

/* Send `body` from one end of a socketpair; read it at the other, as
 * `from`. */
static hull_wire_status pass(const char *body, size_t length, hull_wire_from from)
{
	int              pair[2];
	hull_wire        wire;
	hull_msg         m = { 0 };
	hull_sexpr_doc   doc;
	hull_wire_status status;

	if (socketpair(AF_UNIX, SOCK_STREAM, 0, pair) != 0 || hull_wire_open(&wire, pair[1], from) != 0)
		return HULL_WIRE_BROKEN;
	hull_msg_bytes(&m, (const uint8_t *)body, length);
	if (hull_wire_send(pair[0], &m) != 0)
		status = HULL_WIRE_BROKEN;
	else
		status = hull_wire_read(&wire, &doc);
	hull_msg_free(&m);
	close(pair[0]);
	hull_wire_close(&wire);
	return status;
}

#define LETS(from, body) do { \
	cases++; \
	if (pass(body, sizeof body - 1u, HULL_WIRE_FROM_##from) != HULL_WIRE_OK) \
		FAIL("refused %s", body); \
} while (0)
#define REFUSES(from, body) do { \
	cases++; \
	if (pass(body, sizeof body - 1u, HULL_WIRE_FROM_##from) != HULL_WIRE_BROKEN) \
		FAIL("let through %s", body); \
} while (0)

static void test_directions(void)
{
	LETS(CLIENT, "(5:hello(2:ch1:0)(5:proto1:1))");
	LETS(CLIENT, "(4:list(2:ch1:6))");
	LETS(CLIENT, "(4:ping(2:ch1:0))");  /* unknown, and answerable */
	LETS(CORE, "(5:hello(2:ch1:0)(12:core_version3:0.1)(5:proto1:1))");
	LETS(CORE, "(4:done(2:ch1:5))");

	/* Each side refuses the other's messages and anything malformed. */
	REFUSES(CLIENT, "(5:hello(2:ch1:0)(12:core_version3:0.1)(5:proto1:1))");
	REFUSES(CORE, "(5:hello(2:ch1:0)(5:proto1:1))");
	REFUSES(CLIENT, "(4:list(2:ch1:6)(4:what3:all))");
	REFUSES(CORE, "(4:data(3:buf2:hi)(2:ch1:1)(2:fd1:0))");
	REFUSES(CLIENT, "(5:hello (2:ch1:0)(5:proto1:1))");  /* not canonical */
	REFUSES(CLIENT, "(5:hello(2:ch1:0)(5:proto1:1))(4:list(2:ch1:6))");  /* two forms */
}

static void test_numbers(void)
{
	static const struct { const char *body; int ok; long long value; } numbers[] = {
		{ "(1:m(1:n2:12))", 1, 12 }, { "(1:m(1:n1:0))", 1, 0 }, { "(1:m(1:n2:-7))", 1, -7 },
		{ "(1:m(1:n3:007))", 0, 0 }, { "(1:m(1:n2:-0))", 0, 0 }, { "(1:m(1:n2:+1))", 0, 0 },
		{ "(1:m(1:n0:))", 0, 0 }, { "(1:m(1:n2:1x))", 0, 0 },
	};

	for (unsigned i = 0u; i < sizeof numbers / sizeof numbers[0]; i++) {
		hull_sexpr_doc  doc;
		hull_sexpr_form value;
		long long       n = 0;
		int             ok;

		cases++;
		if (hull_sexpr_open(&doc, (const uint8_t *)numbers[i].body, strlen(numbers[i].body),
		                    HULL_SEXPR_CANONICAL, NULL) != HULL_SEXPR_OK ||
		    !hull_msg_find(&doc, "n", &value)) {
			FAIL("%s did not open", numbers[i].body);
			continue;
		}
		ok = hull_msg_long(&doc, &value, &n);
		if (ok != numbers[i].ok || (ok && n != numbers[i].value))
			FAIL("%s read as %d, %lld", numbers[i].body, ok, n);
	}
}

int main(void)
{
	alarm(60);
	test_directions();
	test_numbers();
	printf("wire: %u cases, %u failed\n", cases, failures);
	return failures == 0u ? 0 : 1;
}
