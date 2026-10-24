/*
 * segment_test.c -- core/segment.c's switch, through its ports alone, as
 * qemu's `-netdev stream` would use them: frames written with their
 * lengths, and what each port is given read back. Each wait for a frame
 * gives up after a second, and each wait for none after a tenth; the
 * impairments, then, the same way, with the switch's counts beside.
 */

#define _GNU_SOURCE

#include <errno.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "segment.h"

static unsigned cases;
static unsigned failures;

static void fail(const char *what, const char *detail)
{
	failures++;
	printf("FAIL %s: %s\n", what, detail);
}

static int port_of(const char *key, const char *machine)
{
	char why[256] = "";
	int  fd = hull_segment_port(key, machine, why, sizeof why);

	if (fd < 0)
		fail("a port", why);
	return fd;
}

static int port(const char *key)
{
	return port_of(key, "m");
}

static void impair(const char *key, const char *machine, int direction, const hull_impairment *m)
{
	char why[256] = "";

	if (hull_segment_impair(key, machine, direction, m, why, sizeof why) != 0)
		fail("an impairment", why);
}

static void set_tamper(const char *key, const char *machine, int direction, const hull_tamper *t)
{
	char why[256] = "";

	if (hull_segment_tamper(key, machine, direction, t, why, sizeof why) != 0)
		fail("a tamper", why);
}

/* `machine`'s counts on `key`, one way. */
static hull_segment_count counted(const char *key, const char *machine, int direction)
{
	hull_segment_counts counts[8];
	hull_segment_count  none = { 0 };
	int                 n = hull_segment_counts_of(key, counts, 8u);

	for (int i = 0; i < n; i++)
		if (strcmp(counts[i].machine, machine) == 0)
			return counts[i].way[direction];
	return none;
}

static void check(const char *what, int ok)
{
	cases++;
	if (!ok)
		fail(what, "no");
}

static uint64_t ms_now(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return (uint64_t)t.tv_sec * 1000u + (uint64_t)t.tv_nsec / 1000000u;
}

/* A frame from `source` to `destination`, its payload `tag`, framed. */
static size_t frame(uint8_t *out, uint8_t destination, uint8_t source, char tag)
{
	static const uint8_t base[6] = { 0x52, 0x54, 0x00, 0x00, 0x00, 0x00 };
	size_t               length = 14u + 1u;

	out[0] = 0u;
	out[1] = 0u;
	out[2] = 0u;
	out[3] = (uint8_t)length;
	memcpy(out + 4, base, 6u);
	out[4 + 5] = destination;
	if (destination == 0xffu)
		memset(out + 4, 0xff, 6u);
	memcpy(out + 10, base, 6u);
	out[10 + 5] = source;
	out[16] = 0x08u;
	out[17] = 0x00u;
	out[18] = (uint8_t)tag;
	return 4u + length;
}

static void send_frame(int fd, uint8_t destination, uint8_t source, char tag)
{
	uint8_t data[64];
	size_t  length = frame(data, destination, source, tag);

	if (write(fd, data, length) != (ssize_t)length)
		fail("a write", strerror(errno));
}

/* The tag of the next frame `fd` is given within `ms`, or 0 for none. */
static char next_tag(int fd, int ms)
{
	struct pollfd p = { fd, POLLIN, 0 };
	uint8_t       data[64];
	size_t        got = 0u;

	while (got < 19u) {
		ssize_t n;

		if (poll(&p, 1u, ms) <= 0)
			return 0;
		n = read(fd, data + got, 19u - got);
		if (n <= 0)
			return 0;
		got += (size_t)n;
	}
	return (char)data[18];
}

static void expect(const char *what, int fd, char want)
{
	char got = next_tag(fd, want != 0 ? 1000 : 100);

	cases++;
	if (got != want) {
		char detail[64];

		snprintf(detail, sizeof detail, "given '%c', wanted '%c'", got != 0 ? got : '-',
		         want != 0 ? want : '-');
		fail(what, detail);
	}
}

/* Which of `n` frames from `from` reach `to` with `seed` and `loss`, as a
 * pattern of bits, on a segment of their own. */
static uint64_t pattern(const char *key, uint64_t seed, double loss, unsigned n)
{
	hull_impairment m = { 0 };
	uint64_t        got = 0u;
	int             from = port_of(key, "from");
	int             to = port_of(key, "to");

	m.loss = loss;
	m.seed = seed;
	impair(key, "from", HULL_SEGMENT_OUT, &m);
	for (unsigned i = 0u; i < n; i++)
		send_frame(from, 0xffu, 1u, (char)('A' + i % 26u));
	for (unsigned i = 0u; i < n; i++) {
		char tag;

		/* What reaches `to` arrives in order; each one sent either
		 * came or did not. */
		if ((tag = next_tag(to, 100)) == 0)
			break;
		for (unsigned k = i; k < n; k++)
			if ((char)('A' + k % 26u) == tag) {
				got |= 1ull << k;
				i = k;
				break;
			}
	}
	close(from);
	close(to);
	return got;
}

int main(void)
{
	int a = port("wire");
	int b = port("wire");
	int c = port("wire");
	int d = port("wire");
	int other = port("elsewhere");

	if (a < 0 || b < 0 || c < 0 || d < 0 || other < 0)
		return 1;
	/* A broadcast reaches every other port of its segment, and none of
	 * another's, nor its sender. */
	send_frame(a, 0xffu, 1u, 'B');
	expect("a broadcast, to b", b, 'B');
	expect("a broadcast, to c", c, 'B');
	expect("a broadcast, to d", d, 'B');
	expect("a broadcast, not back to its sender", a, 0);
	expect("a broadcast, not to another segment", other, 0);
	/* An address not yet seen is flooded too; once its owner has sent,
	 * it goes to that port alone. */
	send_frame(a, 2u, 1u, 'U');
	expect("an unknown address, flooded to b", b, 'U');
	expect("an unknown address, flooded to c", c, 'U');
	expect("an unknown address, flooded to d", d, 'U');
	send_frame(b, 1u, 2u, 'R');
	expect("a reply to a learned address, to a", a, 'R');
	expect("a reply to a learned address, not to c", c, 0);
	expect("a reply to a learned address, not to d", d, 0);
	send_frame(a, 2u, 1u, 'L');
	expect("a learned address, to b", b, 'L');
	expect("a learned address, not to c", c, 0);
	expect("a learned address, not to d", d, 0);
	/* A frame for the port it came from goes nowhere. */
	send_frame(b, 2u, 2u, 'S');
	expect("a frame for its own port, not to a", a, 0);
	expect("a frame for its own port, not to c", c, 0);
	expect("a frame for its own port, not to d", d, 0);
	expect("a frame for its own port, not back to it", b, 0);
	/* A port that closes is gone: what was learned on it is forgotten, so
	 * its address floods again, to both of the rest, where were it not
	 * forgotten it would go to the port moved into the gone one's place
	 * alone; and the rest carry on. */
	close(b);
	send_frame(a, 2u, 1u, 'G');
	expect("a gone port's address, flooded to c", c, 'G');
	expect("a gone port's address, flooded to d", d, 'G');
	send_frame(c, 1u, 3u, 'C');
	expect("after a port goes, a to c still", a, 'C');
	/* Another segment of its own. */
	{
		int more = port("elsewhere");

		send_frame(other, 0xffu, 9u, 'E');
		expect("another segment's broadcast, to its own", more, 'E');
		expect("another segment's broadcast, not to wire", a, 0);
		close(more);
	}
	/* A frame's length that no frame has is the port's end. */
	{
		uint8_t       bad[4] = { 0xffu, 0xffu, 0xffu, 0xffu };
		uint8_t       left;
		struct pollfd p = { c, POLLIN, 0 };

		cases++;
		if (write(c, bad, sizeof bad) != (ssize_t)sizeof bad)
			fail("a bad length's write", strerror(errno));
		if (poll(&p, 1u, 1000) <= 0 || read(c, &left, 1u) != 0)
			fail("a bad length", "the port was not closed");
	}
	close(a);
	close(c);
	close(d);
	close(other);

	/* --- impairments, each on a segment of its own --- */
	{
		hull_impairment m = { 0 };
		char            why[256] = "";

		cases++;
		if (hull_segment_impair("nowhere", NULL, HULL_SEGMENT_OUT, &m, why, sizeof why) == 0)
			fail("an impairment of no segment", "was taken");
	}
	{
		/* Loss of 1 on one way drops every frame there, counted, and
		 * leaves the other way a wire. */
		hull_impairment m = { 0 };
		int             x = port_of("lossy", "x");
		int             y = port_of("lossy", "y");

		m.loss = 1.0;
		impair("lossy", "x", HULL_SEGMENT_OUT, &m);
		send_frame(x, 0xffu, 1u, 'X');
		expect("loss 1, nothing to y", y, 0);
		send_frame(y, 0xffu, 2u, 'Y');
		expect("loss 1 one way, the other a wire", x, 'Y');
		check("loss counted out of x", counted("lossy", "x", HULL_SEGMENT_OUT).lost == 1u &&
		      counted("lossy", "x", HULL_SEGMENT_OUT).frames == 1u &&
		      counted("lossy", "x", HULL_SEGMENT_OUT).passed == 0u);
		check("y's way out counted", counted("lossy", "y", HULL_SEGMENT_OUT).passed == 1u &&
		      counted("lossy", "y", HULL_SEGMENT_OUT).bytes == 15u &&
		      counted("lossy", "x", HULL_SEGMENT_IN).passed == 1u);
		/* Changed during the run: a wire again. */
		memset(&m, 0, sizeof m);
		impair("lossy", "x", HULL_SEGMENT_OUT, &m);
		send_frame(x, 0xffu, 1u, 'H');
		expect("a link healed", y, 'H');
		/* Down on y's way in: a cut, every frame to it dropped. */
		m.down = 1;
		impair("lossy", "y", HULL_SEGMENT_IN, &m);
		send_frame(x, 0xffu, 1u, 'D');
		expect("down, nothing to y", y, 0);
		check("down counted as lost into y", counted("lossy", "y", HULL_SEGMENT_IN).lost == 1u &&
		      counted("lossy", "x", HULL_SEGMENT_OUT).passed == 2u);
		close(x);
		close(y);
	}
	{
		/* Every machine's rule, a machine's own over it, and a port
		 * that joins later taking the rule. */
		hull_impairment cut = { 0 };
		hull_impairment wire = { 0 };
		int             x = port_of("ruled", "x");
		int             y;

		cut.down = 1;
		impair("ruled", NULL, HULL_SEGMENT_IN, &cut);
		impair("ruled", "x", HULL_SEGMENT_IN, &wire);
		y = port_of("ruled", "y");
		send_frame(y, 0xffu, 2u, 'R');
		expect("a machine's own rule over every machine's", x, 'R');
		send_frame(x, 0xffu, 1u, 'J');
		expect("a port joining later takes every machine's rule", y, 0);
		close(x);
		close(y);
	}
	{
		/* A frame longer than the mtu after its header, dropped. */
		hull_impairment m = { 0 };
		int             x = port_of("narrow", "x");
		int             y = port_of("narrow", "y");

		m.mtu = 1u;
		impair("narrow", "y", HULL_SEGMENT_IN, &m);
		send_frame(x, 0xffu, 1u, 'M');
		expect("a frame inside the mtu", y, 'M');
		{
			uint8_t data[64];
			size_t  length = frame(data, 0xffu, 1u, 'T');

			/* Two bytes of payload: one past the mtu. */
			data[3] = (uint8_t)(data[3] + 1u);
			data[length] = 'u';
			if (write(x, data, length + 1u) != (ssize_t)(length + 1u))
				fail("a write", strerror(errno));
		}
		expect("a frame past the mtu, dropped", y, 0);
		check("too big counted", counted("narrow", "y", HULL_SEGMENT_IN).too_big == 1u);
		close(x);
		close(y);
	}
	{
		/* Duplicate 1: two of each. Corrupt 1: the payload's bit
		 * flipped, the frame still at its port. */
		hull_impairment m = { 0 };
		int             x = port_of("noisy", "x");
		int             y = port_of("noisy", "y");

		m.duplicate = 1.0;
		impair("noisy", "x", HULL_SEGMENT_OUT, &m);
		send_frame(x, 0xffu, 1u, 'W');
		expect("duplicate 1, the first", y, 'W');
		expect("duplicate 1, the second", y, 'W');
		expect("duplicate 1, no third", y, 0);
		memset(&m, 0, sizeof m);
		m.corrupt = 1.0;
		impair("noisy", "x", HULL_SEGMENT_OUT, &m);
		send_frame(x, 0xffu, 1u, 'C');
		{
			char tag = next_tag(y, 1000);

			check("corrupt 1, a bit of the payload flipped",
			      tag != 0 && tag != 'C' && __builtin_popcount((unsigned)(uint8_t)(tag ^ 'C')) == 1);
		}
		check("duplicated and corrupted counted",
		      counted("noisy", "x", HULL_SEGMENT_OUT).duplicated == 1u &&
		      counted("noisy", "x", HULL_SEGMENT_OUT).corrupted == 1u);
		close(x);
		close(y);
	}
	{
		/* Delay: held 300 ms. With reorder 1 a frame is not held, and
		 * overtakes. A limit of 1 drops the second held frame. */
		hull_impairment m = { 0 };
		int             x = port_of("slow", "x");
		int             y = port_of("slow", "y");
		uint64_t        sent;
		char            tag;

		m.delay_ms = 300u;
		impair("slow", "x", HULL_SEGMENT_OUT, &m);
		sent = ms_now();
		send_frame(x, 0xffu, 1u, 'S');
		expect("a delay of 300 ms, not within 100", y, 0);
		tag = next_tag(y, 1000);
		check("a delay of 300 ms, then the frame", tag == 'S' && ms_now() - sent >= 290u);
		m.reorder = 1.0;
		impair("slow", "x", HULL_SEGMENT_OUT, &m);
		send_frame(x, 0xffu, 1u, 'O');
		/* Within a tenth, where a held frame is three tenths behind. */
		check("reorder 1, not held", next_tag(y, 100) == 'O');
		memset(&m, 0, sizeof m);
		m.delay_ms = 300u;
		m.limit = 1u;
		impair("slow", "x", HULL_SEGMENT_OUT, &m);
		send_frame(x, 0xffu, 1u, '1');
		send_frame(x, 0xffu, 1u, '2');
		expect("a limit of 1, the first held frame", y, '1');
		expect("a limit of 1, the second dropped", y, 0);
		check("overflow counted", counted("slow", "x", HULL_SEGMENT_OUT).overflow == 1u);
		close(x);
		close(y);
	}
	{
		/* A rate: 8000 bits a second, 15 bytes a frame, 15 ms each, so
		 * the tenth of ten sent at once is at least 135 ms behind. */
		hull_impairment m = { 0 };
		int             x = port_of("thin", "x");
		int             y = port_of("thin", "y");
		uint64_t        sent;
		unsigned        got = 0u;

		m.rate = 8000u;
		impair("thin", "x", HULL_SEGMENT_OUT, &m);
		sent = ms_now();
		for (unsigned i = 0u; i < 10u; i++)
			send_frame(x, 0xffu, 1u, (char)('a' + i));
		while (got < 10u && next_tag(y, 1000) != 0)
			got++;
		check("a rate: ten frames, the last at least 135 ms on",
		      got == 10u && ms_now() - sent >= 130u);
		close(x);
		close(y);
	}
	{
		/* netem's Gilbert-Elliott: with p 1 and r 0, the first frame
		 * finds the good state and passes, and the model is then bad
		 * for good, losing every frame with 1-h of 1. */
		hull_impairment m = { 0 };
		int             x = port_of("bursty", "x");
		int             y = port_of("bursty", "y");

		m.gemodel = 1;
		m.p = 1.0;
		m.r = 0.0;
		m.bad_loss = 1.0;
		impair("bursty", "x", HULL_SEGMENT_OUT, &m);
		send_frame(x, 0xffu, 1u, 'g');
		send_frame(x, 0xffu, 1u, 'b');
		send_frame(x, 0xffu, 1u, 'c');
		expect("gemodel, the good state's frame", y, 'g');
		expect("gemodel, the bad state's, lost", y, 0);
		check("gemodel, two lost", counted("bursty", "x", HULL_SEGMENT_OUT).lost == 2u);
		close(x);
		close(y);
	}
	{
		/* Seeded: the same seed loses the same frames; another seed,
		 * other ones. */
		uint64_t first = pattern("seeded1", 7u, 0.5, 26u);
		uint64_t again = pattern("seeded2", 7u, 0.5, 26u);
		uint64_t other = pattern("seeded3", 8u, 0.5, 26u);

		check("a seed's losses, again the same", first == again);
		check("a seed's losses, some and not all", first != 0u && first != (1ull << 26) - 1u);
		check("another seed's losses, other ones", first != other);
	}
	/* --- targeted faults, each on a segment of its own. The frame's
	 * payload begins at the tag byte, so offset 0 is the tag. --- */
	{
		/* A match drops just the matching frames; the rest pass. */
		hull_tamper t = { 0 };
		int         x = port_of("tdrop", "x");
		int         y = port_of("tdrop", "y");

		t.match[0] = 'A';
		t.match_length = 1u;
		t.drop = 1.0;
		set_tamper("tdrop", "x", HULL_SEGMENT_OUT, &t);
		send_frame(x, 0xffu, 1u, 'A');
		expect("a matched frame dropped", y, 0);
		send_frame(x, 0xffu, 1u, 'B');
		expect("an unmatched frame passed", y, 'B');
		check("the match and drop counted",
		      counted("tdrop", "x", HULL_SEGMENT_OUT).matched == 1u &&
		      counted("tdrop", "x", HULL_SEGMENT_OUT).tampered == 1u);
		close(x);
		close(y);
	}
	{
		/* No match pattern matches every frame; a rewrite overwrites a
		 * payload byte, a flip turns one bit. */
		hull_tamper t = { 0 };
		int         x = port_of("tset", "x");
		int         y = port_of("tset", "y");
		char        tag;

		t.rewrite = 1.0;
		t.set[0] = 'Z';
		t.set_length = 1u;
		set_tamper("tset", "x", HULL_SEGMENT_OUT, &t);
		send_frame(x, 0xffu, 1u, 'C');
		expect("a rewritten frame reaches its port changed", y, 'Z');
		memset(&t, 0, sizeof t);
		t.flip = 1.0;
		set_tamper("tset", "x", HULL_SEGMENT_OUT, &t);
		send_frame(x, 0xffu, 1u, 'C');
		tag = next_tag(y, 1000);
		check("a flipped frame differs by one bit",
		      tag != 0 && tag != 'C' && __builtin_popcount((unsigned)(uint8_t)(tag ^ 'C')) == 1);
		close(x);
		close(y);
	}
	{
		/* Replay keeps copies of a matched frame and re-sends them: the
		 * original, then two more. */
		hull_tamper t = { 0 };
		int         x = port_of("treplay", "x");
		int         y = port_of("treplay", "y");

		t.replay = 1.0;
		t.copies = 2u;
		t.every_ms = 30u;
		set_tamper("treplay", "x", HULL_SEGMENT_OUT, &t);
		send_frame(x, 0xffu, 1u, 'P');
		expect("a replayed frame, the original", y, 'P');
		expect("a replayed frame, the first copy", y, 'P');
		expect("a replayed frame, the second copy", y, 'P');
		expect("a replayed frame, no third", y, 0);
		check("the replays counted", counted("treplay", "x", HULL_SEGMENT_OUT).replayed == 2u);
		close(x);
		close(y);
	}
	{
		/* A match at an offset past the frame never matches. */
		hull_tamper t = { 0 };
		int         x = port_of("tfar", "x");
		int         y = port_of("tfar", "y");

		t.match_offset = 100u;
		t.match[0] = 'A';
		t.match_length = 1u;
		t.drop = 1.0;
		set_tamper("tfar", "x", HULL_SEGMENT_OUT, &t);
		send_frame(x, 0xffu, 1u, 'A');
		expect("a match past the frame does not match", y, 'A');
		check("nothing matched", counted("tfar", "x", HULL_SEGMENT_OUT).matched == 0u);
		close(x);
		close(y);
	}
	printf("segment: %u cases, %u failed\n", cases, failures);
	return failures != 0;
}
