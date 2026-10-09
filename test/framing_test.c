/*
 * framing_test.c -- B.2's framing, held to what the protocol needs of it.
 *
 * The cases that matter most are the hostile ones: a length above the
 * maximum must be refused when its four bytes arrive, before anything is
 * read for it, and a body that is not one canonical s-expression must
 * never reach a caller.
 */

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "framing.h"

static unsigned cases;
static unsigned failures;

#define FAIL(...) do { \
	failures++; \
	printf("FAIL %s:%d: ", __FILE__, __LINE__); \
	printf(__VA_ARGS__); \
	printf("\n"); \
} while (0)

#define EXPECT(got, want, what) do { \
	cases++; \
	if ((got) != (want)) \
		FAIL("%s: got %d, wanted %d", what, (int)(got), (int)(want)); \
} while (0)

static uint8_t buffer[HULL_AGENT_READER_SIZE];
static uint8_t wire[HULL_AGENT_READER_SIZE + 64];
static uint8_t body[HULL_AGENT_READER_SIZE];
static uint8_t large[HULL_AGENT_READER_SIZE + 64];

static uint32_t put_length(uint8_t *out, uint32_t length)
{
	out[0] = (uint8_t)(length >> 24);
	out[1] = (uint8_t)(length >> 16);
	out[2] = (uint8_t)(length >> 8);
	out[3] = (uint8_t)length;
	return 4u;
}

/* A framed message with `text` as its body, written by hand rather than
 * by the code under test. */
static uint32_t message(uint8_t *out, const char *text)
{
	uint32_t n = (uint32_t)strlen(text);

	put_length(out, n);
	memcpy(out + 4, text, n);
	return 4u + n;
}

/* A body of exactly `size` bytes: one canonical atom. */
static uint32_t atom_of_size(uint8_t *out, uint32_t size)
{
	char     prefix[16];
	uint32_t digits = 1u;
	uint32_t n;

	/* The prefix's own width counts toward the size. */
	for (;;) {
		n = (uint32_t)snprintf(prefix, sizeof prefix, "%u:", size - digits - 1u);
		if (n == digits + 1u)
			break;
		digits++;
	}
	memcpy(out, prefix, n);
	memset(out + n, 'x', size - n);
	return size;
}

static void test_whole_and_split(void)
{
	hull_agent_reader reader;
	hull_sexpr_doc    doc;
	hull_sexpr_form   form;
	uint32_t          n = message(wire, "(4:ping)");

	EXPECT(hull_agent_reader_init(&reader, buffer, sizeof buffer), HULL_AGENT_OK, "init");
	EXPECT(hull_agent_reader_next(&reader, &doc, NULL), HULL_AGENT_WAIT, "empty stream");

	/* One byte at a time: nothing until the last byte arrives. */
	for (uint32_t i = 0; i < n; i++) {
		EXPECT(hull_agent_reader_push(&reader, wire + i, 1u), HULL_AGENT_OK, "push a byte");
		EXPECT(hull_agent_reader_next(&reader, &doc, NULL),
		       i + 1u == n ? HULL_AGENT_OK : HULL_AGENT_WAIT, "split message");
	}
	cases++;
	if (!hull_sexpr_first(&doc, &form) || form.kind != HULL_SEXPR_LIST ||
	    !hull_sexpr_child(&doc, &form, &form) || !hull_sexpr_is(&doc, &form, "ping"))
		FAIL("the body is not (ping)");
	EXPECT(hull_agent_reader_next(&reader, &doc, NULL), HULL_AGENT_WAIT,
	       "the message is not handed out twice");
}

static void test_two_in_one_push(void)
{
	hull_agent_reader reader;
	hull_sexpr_doc    doc;
	hull_sexpr_form   form;
	uint32_t          n;

	n  = message(wire, "(5:hello)");
	n += message(wire + n, "4:stop");
	hull_agent_reader_init(&reader, buffer, sizeof buffer);
	EXPECT(hull_agent_reader_push(&reader, wire, n), HULL_AGENT_OK, "push two");
	EXPECT(hull_agent_reader_next(&reader, &doc, NULL), HULL_AGENT_OK, "first of two");
	cases++;
	if (!hull_sexpr_first(&doc, &form) || form.kind != HULL_SEXPR_LIST)
		FAIL("the first of two is not the list");
	EXPECT(hull_agent_reader_next(&reader, &doc, NULL), HULL_AGENT_OK, "second of two");
	cases++;
	if (!hull_sexpr_first(&doc, &form) || !hull_sexpr_is(&doc, &form, "stop"))
		FAIL("the second of two is not stop");
	EXPECT(hull_agent_reader_next(&reader, &doc, NULL), HULL_AGENT_WAIT, "nothing after two");
}

static void test_sizes(void)
{
	hull_agent_reader reader;
	hull_sexpr_doc    doc;
	uint32_t          n;

	/* Exactly the maximum body arrives and opens. */
	n  = put_length(wire, HULL_AGENT_MAX_BODY);
	n += atom_of_size(wire + n, HULL_AGENT_MAX_BODY);
	hull_agent_reader_init(&reader, buffer, sizeof buffer);
	EXPECT(hull_agent_reader_push(&reader, wire, n), HULL_AGENT_OK, "push the largest");
	EXPECT(hull_agent_reader_next(&reader, &doc, NULL), HULL_AGENT_OK, "the largest message");

	/* One byte over is refused from the length alone, before any of the
	 * body has been sent. */
	put_length(wire, HULL_AGENT_MAX_BODY + 1u);
	hull_agent_reader_init(&reader, buffer, sizeof buffer);
	hull_agent_reader_push(&reader, wire, 4u);
	EXPECT(hull_agent_reader_next(&reader, &doc, NULL), HULL_AGENT_OVERSIZE,
	       "one byte over, refused at its length");

	/* And with a buffer larger than the largest message, where situ's
	 * reader alone would wait for the body: the refusal has to come from
	 * the length, not from running out of room. */
	put_length(wire, HULL_AGENT_MAX_BODY + 1u);
	hull_agent_reader_init(&reader, large, sizeof large);
	hull_agent_reader_push(&reader, wire, 4u);
	EXPECT(hull_agent_reader_next(&reader, &doc, NULL), HULL_AGENT_OVERSIZE,
	       "one byte over, in a buffer that could hold it");

	/* A length that would wrap a u32 addition. */
	put_length(wire, 0xFFFFFFFFu);
	hull_agent_reader_init(&reader, buffer, sizeof buffer);
	hull_agent_reader_push(&reader, wire, 4u);
	EXPECT(hull_agent_reader_next(&reader, &doc, NULL), HULL_AGENT_OVERSIZE,
	       "a length that would wrap");

	/* A partial length is not yet a length. */
	hull_agent_reader_init(&reader, buffer, sizeof buffer);
	hull_agent_reader_push(&reader, wire, 3u);
	EXPECT(hull_agent_reader_next(&reader, &doc, NULL), HULL_AGENT_WAIT, "three bytes of length");

	EXPECT(hull_agent_reader_init(&reader, buffer, sizeof buffer - 1u), HULL_AGENT_SMALL,
	       "a buffer one byte short of the largest message");

	/* A buffer that is full refuses more, and takes it once a message
	 * has gone. */
	hull_agent_reader_init(&reader, buffer, sizeof buffer);
	n  = put_length(wire, HULL_AGENT_MAX_BODY);
	n += atom_of_size(wire + n, HULL_AGENT_MAX_BODY);
	hull_agent_reader_push(&reader, wire, n);
	EXPECT(hull_agent_reader_push(&reader, wire, 1u), HULL_AGENT_FULL, "push into a full buffer");
	EXPECT(hull_agent_reader_next(&reader, &doc, NULL), HULL_AGENT_OK, "the message that filled it");
	EXPECT(hull_agent_reader_push(&reader, wire, 1u), HULL_AGENT_OK, "push once it has gone");
}

static void malformed(const char *text, const char *what)
{
	hull_agent_reader reader;
	hull_sexpr_doc    doc;
	uint32_t          n = message(wire, text);

	hull_agent_reader_init(&reader, buffer, sizeof buffer);
	hull_agent_reader_push(&reader, wire, n);
	EXPECT(hull_agent_reader_next(&reader, &doc, NULL), HULL_AGENT_MALFORMED, what);
}

static void test_malformed(void)
{
	malformed("", "an empty body");
	malformed("(4:ping 3:abc)", "whitespace in canonical form");
	malformed("(4:ping)x", "bytes after the one form");
	malformed("(ping)", "readable syntax on the wire");
	malformed("04:ping", "a length that is not minimal");
}

static void test_frame(void)
{
	hull_agent_reader reader;
	hull_sexpr_doc    doc;
	hull_sexpr_form   form;
	uint32_t          wrote = 0;
	uint32_t          n;
	const char       *text = "(4:exec(4:argv2:ls))";

	n = (uint32_t)strlen(text);
	EXPECT(hull_agent_frame(wire, sizeof wire, (const uint8_t *)text, n, &wrote),
	       HULL_AGENT_OK, "frame a message");
	cases++;
	if (wrote != 4u + n || wire[0] != 0 || wire[1] != 0 || wire[2] != 0 || wire[3] != n ||
	    memcmp(wire + 4, text, n) != 0)
		FAIL("the framed bytes are not a big-endian length and the body");

	/* What the writer wrote, the reader reads back. */
	hull_agent_reader_init(&reader, buffer, sizeof buffer);
	hull_agent_reader_push(&reader, wire, wrote);
	EXPECT(hull_agent_reader_next(&reader, &doc, NULL), HULL_AGENT_OK, "read back what was framed");
	cases++;
	if (doc.length != n || memcmp(doc.data, text, n) != 0)
		FAIL("the body read back is not the body framed");
	cases++;
	if (!hull_sexpr_first(&doc, &form) || !hull_sexpr_child(&doc, &form, &form) ||
	    !hull_sexpr_is(&doc, &form, "exec"))
		FAIL("the body read back does not open with exec");

	EXPECT(hull_agent_frame(wire, n + 3u, (const uint8_t *)text, n, &wrote),
	       HULL_AGENT_SMALL, "frame into a buffer one byte short");
	EXPECT(hull_agent_frame(wire, sizeof wire, (const uint8_t *)"(ping)", 6u, &wrote),
	       HULL_AGENT_MALFORMED, "frame a readable body");
	n = atom_of_size(body, HULL_AGENT_MAX_BODY);
	EXPECT(hull_agent_frame(wire, sizeof wire, body, n, &wrote), HULL_AGENT_OK,
	       "frame the largest body");
	EXPECT(hull_agent_frame(wire, sizeof wire, body, n + 1u, &wrote), HULL_AGENT_OVERSIZE,
	       "frame one byte over");
}

int main(void)
{
	/* A fault in what is tested can loop rather than fail, and a suite
	 * nobody is watching would wait on it for ever. SIGALRM ends it. */
	alarm(60);
	test_whole_and_split();
	test_two_in_one_push();
	test_sizes();
	test_malformed();
	test_frame();

	if (cases == 0) {
		printf("FAIL: no cases ran\n");
		return 1;
	}
	printf("framing: %u cases, %u failed\n", cases, failures);
	return failures == 0 ? 0 : 1;
}
