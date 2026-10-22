/*
 * segment_test.c -- core/segment.c's switch, through its ports alone, as
 * qemu's `-netdev stream` would use them: frames written with their
 * lengths, and what each port is given read back. Each wait for a frame
 * gives up after a second, and each wait for none after a tenth.
 */

#define _GNU_SOURCE

#include <errno.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "segment.h"

static unsigned cases;
static unsigned failures;

static void fail(const char *what, const char *detail)
{
	failures++;
	printf("FAIL %s: %s\n", what, detail);
}

static int port(const char *key)
{
	char why[256] = "";
	int  fd = hull_segment_port(key, why, sizeof why);

	if (fd < 0)
		fail("a port", why);
	return fd;
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
	printf("segment: %u cases, %u failed\n", cases, failures);
	return failures != 0;
}
