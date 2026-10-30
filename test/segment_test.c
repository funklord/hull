/*
 * segment_test.c -- core/segment.c's switch, through its ports alone, as
 * qemu's `-netdev stream` would use them: frames written with their
 * lengths, and what each port is given read back. Each wait for a frame
 * gives up after a second, and each wait for none after a tenth; the
 * impairments, then, the same way, with the switch's counts beside.
 */

#define _GNU_SOURCE

#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
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

/* --- hull at .1 ------------------------------------------------------------ */

static const uint8_t hull_mac[6] = { 0x02, 'h', 'u', 'l', 'l', 0x01 };

static void put16(uint8_t *at, unsigned value)
{
	at[0] = (uint8_t)(value >> 8);
	at[1] = (uint8_t)value;
}

static unsigned get16(const uint8_t *at)
{
	return (unsigned)(at[0] << 8 | at[1]);
}

/* The ones' complement sum, folded: 0xffff over data whose sum is right. */
static unsigned sum16(uint32_t sum, const uint8_t *data, size_t length)
{
	for (size_t i = 0u; i + 1u < length; i += 2u)
		sum += get16(data + i);
	if (length % 2u != 0u)
		sum += (uint32_t)data[length - 1u] << 8;
	while (sum >> 16)
		sum = (sum & 0xffffu) + (sum >> 16);
	return sum;
}

/* `frame`, `length` bytes, framed onto `fd`. */
static void send_raw(int fd, const uint8_t *frame, size_t length)
{
	uint8_t data[4u + 1514u];

	data[0] = 0u;
	data[1] = 0u;
	put16(data + 2, (unsigned)length);
	memcpy(data + 4, frame, length);
	if (write(fd, data, 4u + length) != (ssize_t)(4u + length))
		fail("a write", strerror(errno));
}

/* The next frame `fd` is given within `ms`, into `out`: its length, or 0. */
static size_t next_raw(int fd, uint8_t *out, size_t cap, int ms)
{
	struct pollfd p = { fd, POLLIN, 0 };
	uint8_t       head[4];
	size_t        got = 0u;
	size_t        length;

	while (got < 4u) {
		ssize_t n;

		if (poll(&p, 1u, ms) <= 0 || (n = read(fd, head + got, 4u - got)) <= 0)
			return 0u;
		got += (size_t)n;
	}
	length = (size_t)head[2] << 8 | head[3];
	if (length > cap)
		return 0u;
	for (got = 0u; got < length;) {
		ssize_t n;

		if (poll(&p, 1u, ms) <= 0 || (n = read(fd, out + got, length - got)) <= 0)
			return 0u;
		got += (size_t)n;
	}
	return length;
}

/* An ARP request from MAC ...:`me`, address 10.55.0.`me`, for 10.55.0.`whom`. */
static void send_arp(int fd, uint8_t me, uint8_t whom)
{
	uint8_t f[60] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x52, 0x54, 0, 0, 0, me, 0x08, 0x06,
	                  0, 1, 0x08, 0, 6, 4, 0, 1, 0x52, 0x54, 0, 0, 0, me, 10, 55, 0, me,
	                  0, 0, 0, 0, 0, 0, 10, 55, 0, whom };

	send_raw(fd, f, sizeof f);
}

#define SUM_NONE      0   /* no UDP sum, as IPv4 allows */
#define SUM_UDP       1   /* a UDP sum, right */
#define SUM_UDP_WRONG 2   /* a UDP sum, one bit off */
#define SUM_IP_WRONG  3   /* the IPv4 header's sum one bit off */

static void send_dns_summed(int fd, uint8_t me, const char *name, unsigned type, int sum);

/* A DNS question for `name` of `type`, from MAC and address ...`me` to
 * hull's .1, from UDP port 4000. */
static void send_dns(int fd, uint8_t me, const char *name, unsigned type)
{
	send_dns_summed(fd, me, name, type, SUM_NONE);
}

/* The same, its sums as `sum` says. */
static void send_dns_summed(int fd, uint8_t me, const char *name, unsigned type, int sum)
{
	uint8_t f[300] = { 0 };
	size_t  at = 14u + 28u + 12u;
	size_t  dns;

	memcpy(f, hull_mac, 6u);
	f[6] = 0x52;
	f[7] = 0x54;
	f[11] = me;
	put16(f + 12, 0x0800u);
	for (const char *label = name; *label != '\0';) {
		size_t length = strcspn(label, ".");

		f[at++] = (uint8_t)length;
		memcpy(f + at, label, length);
		at += length;
		label += length + (label[length] == '.');
	}
	f[at++] = 0u;
	put16(f + at, type);
	put16(f + at + 2u, 1u);
	at += 4u;
	dns = at - 14u - 28u;
	f[42] = 0x12;
	f[43] = 0x34;
	f[44] = 0x01;   /* recursion desired */
	put16(f + 46, 1u);
	f[14] = 0x45;
	put16(f + 16, (unsigned)(28u + dns));
	f[22] = 64u;
	f[23] = 17u;
	f[26] = 10;
	f[27] = 55;
	f[29] = me;
	f[30] = 10;
	f[31] = 55;
	f[33] = 1;
	put16(f + 24, ~sum16(0u, f + 14, 20u) & 0xffffu);
	put16(f + 34, 4000u);
	put16(f + 36, 53u);
	put16(f + 38, (unsigned)(8u + dns));
	if (sum == SUM_UDP || sum == SUM_UDP_WRONG) {
		uint8_t  pseudo[12];
		uint32_t total;

		memcpy(pseudo, f + 26, 8u);
		pseudo[8] = 0u;
		pseudo[9] = 17u;
		put16(pseudo + 10, (unsigned)(8u + dns));
		total = sum16(0u, pseudo, 12u);
		put16(f + 40, ~sum16(total, f + 34, 8u + dns) & 0xffffu);
		if (sum == SUM_UDP_WRONG)
			f[41] ^= 1u;
	}
	if (sum == SUM_IP_WRONG)
		f[25] ^= 1u;
	send_raw(fd, f, at);
}

/* The answer to a send_dns() on `fd`: its rcode, and where it has one,
 * its data into `data`; -1 where none came or its sums or addressing are
 * wrong. */
static int dns_reply(int fd, uint8_t me, uint8_t *data, size_t *data_length)
{
	uint8_t  f[1514];
	size_t   length = next_raw(fd, f, sizeof f, 1000);
	size_t   udp;
	size_t   at;
	uint32_t sum;

	if (length < 14u + 28u + 12u || memcmp(f + 6, hull_mac, 6u) != 0 || f[5] != me ||
	    get16(f + 12) != 0x0800u || sum16(0u, f + 14, 20u) != 0xffffu || f[29] != 1u ||
	    f[33] != me || get16(f + 34) != 53u || get16(f + 36) != 4000u)
		return -1;
	udp = get16(f + 38);
	sum = sum16(0u, f + 26, 8u) + 17u + (uint32_t)udp;
	if (sum16(sum, f + 34, udp) != 0xffffu || get16(f + 42) != 0x1234u || (f[44] & 0x81u) != 0x81u)
		return -1;
	*data_length = 0u;
	if (get16(f + 48) == 1u) {
		/* The question, then a pointer, type, class, TTL and the data. */
		for (at = 14u + 28u + 12u; f[at] != 0u; at += 1u + f[at])
			;
		at += 5u + 10u;
		*data_length = get16(f + at);
		memcpy(data, f + at + 2u, *data_length);
	}
	return f[45] & 0x0f;
}

/* An ICMP message of `type` from MAC and address ...`me` to .1, its data
 * `data`, its sum one bit off where `wrong`. */
static void send_ping(int fd, uint8_t me, unsigned type, const char *data, int wrong)
{
	uint8_t f[200] = { 0 };
	size_t  length = strlen(data);
	size_t  icmp = 8u + length;

	memcpy(f, hull_mac, 6u);
	f[6] = 0x52;
	f[7] = 0x54;
	f[11] = me;
	put16(f + 12, 0x0800u);
	f[14] = 0x45;
	put16(f + 16, (unsigned)(20u + icmp));
	f[22] = 64u;
	f[23] = 1u;
	f[26] = 10;
	f[27] = 55;
	f[29] = me;
	f[30] = 10;
	f[31] = 55;
	f[33] = 1;
	put16(f + 24, ~sum16(0u, f + 14, 20u) & 0xffffu);
	f[34] = (uint8_t)type;
	put16(f + 38, 0x4242u);
	put16(f + 40, 7u);
	memcpy(f + 42, data, length);
	put16(f + 36, ~sum16(0u, f + 34, icmp) & 0xffffu);
	if (wrong)
		f[37] ^= 1u;
	send_raw(fd, f, 14u + 20u + icmp < 60u ? 60u : 14u + 20u + icmp);
}

/* The echo reply to a send_ping() of `data` on `fd`: 1 where it came from
 * .1 to ...`me` with its sums right, its identifier, sequence and data as
 * sent; 0 otherwise. */
static int ping_reply(int fd, uint8_t me, const char *data)
{
	uint8_t f[1514];
	size_t  length = next_raw(fd, f, sizeof f, 1000);
	size_t  icmp = 8u + strlen(data);

	return length >= 14u + 20u + icmp && memcmp(f + 6, hull_mac, 6u) == 0 && f[5] == me &&
	       get16(f + 12) == 0x0800u && sum16(0u, f + 14, 20u) == 0xffffu && f[23] == 1u &&
	       f[29] == 1u && f[33] == me && get16(f + 16) == 20u + icmp && f[34] == 0u &&
	       f[35] == 0u && sum16(0u, f + 34, icmp) == 0xffffu && get16(f + 38) == 0x4242u &&
	       get16(f + 40) == 7u && memcmp(f + 42, data, strlen(data)) == 0;
}

/* An IPv6 frame from MAC and address fd42:55::`me` to `to` at `mac`, of
 * `next`, `payload` its `length` bytes, its ICMPv6 or UDP sum at `sum_at`
 * in the payload taken over the pseudo-header unless `sum_at` is past it;
 * a hop limit of 255. */
static const uint8_t only6_net[8] = { 0xfd, 0x42, 0, 0x55, 0, 0, 0, 0 };

static void send_v6(int fd, uint8_t me, const uint8_t mac[6], const uint8_t to[16], uint8_t next,
                    uint8_t *payload, size_t length, size_t sum_at)
{
	uint8_t  f[600] = { 0 };
	uint32_t sum = 0u;

	memcpy(f, mac, 6u);
	f[6] = 0x52;
	f[7] = 0x54;
	f[11] = me;
	put16(f + 12, 0x86ddu);
	f[14] = 0x60;
	put16(f + 18, (unsigned)length);
	f[20] = next;
	f[21] = 255u;
	memcpy(f + 22, only6_net, 8u);
	f[37] = me;
	memcpy(f + 38, to, 16u);
	if (sum_at + 2u <= length) {
		payload[sum_at] = 0u;
		payload[sum_at + 1u] = 0u;
		sum = sum16(0u, f + 22, 32u) + (uint32_t)length + next;
		put16(payload + sum_at, ~sum16(sum, payload, length) & 0xffffu);
	}
	memcpy(f + 54, payload, length);
	send_raw(fd, f, 54u + length < 60u ? 60u : 54u + length);
}

/* Set, send_tcp() sends its segment with its sum one bit off. */
static int tcp_sum_wrong;

/* A TCP segment from MAC and address ...`me`, port 4100, to .1's 53:
 * `flags`, `seq`, `ack`, and `data`. */
static void send_tcp(int fd, uint8_t me, unsigned flags, uint32_t seq, uint32_t ack,
                     const uint8_t *data, size_t length)
{
	uint8_t  f[1514] = { 0 };
	uint8_t  pseudo[12];
	uint32_t sum;

	memcpy(f, hull_mac, 6u);
	f[6] = 0x52;
	f[7] = 0x54;
	f[11] = me;
	put16(f + 12, 0x0800u);
	f[14] = 0x45;
	put16(f + 16, (unsigned)(40u + length));
	f[22] = 64u;
	f[23] = 6u;
	f[26] = 10;
	f[27] = 55;
	f[29] = me;
	f[30] = 10;
	f[31] = 55;
	f[33] = 1;
	put16(f + 24, ~sum16(0u, f + 14, 20u) & 0xffffu);
	put16(f + 34, 4100u);
	put16(f + 36, 53u);
	put16(f + 38, seq >> 16);
	put16(f + 40, seq & 0xffffu);
	put16(f + 42, ack >> 16);
	put16(f + 44, ack & 0xffffu);
	f[46] = 5u << 4;
	f[47] = (uint8_t)flags;
	put16(f + 48, 65535u);
	memcpy(f + 54, data, length);
	memcpy(pseudo, f + 26, 8u);
	pseudo[8] = 0u;
	pseudo[9] = 6u;
	put16(pseudo + 10, (unsigned)(20u + length));
	sum = sum16(0u, pseudo, 12u);
	put16(f + 50, ~sum16(sum, f + 34, 20u + length) & 0xffffu);
	if (tcp_sum_wrong)
		f[51] ^= 1u;
	send_raw(fd, f, 54u + length < 60u ? 60u : 54u + length);
}

/* The next TCP segment .1 sends `fd` within `ms`: its flags, or -1 where
 * none came or its sums or ports are wrong; its sequence, acknowledgment
 * and data into the rest. */
static int next_tcp(int fd, uint8_t me, uint32_t *seq, uint32_t *ack, uint8_t *data,
                    size_t *length, int ms)
{
	uint8_t  f[1514];
	uint8_t  pseudo[12];
	size_t   got = next_raw(fd, f, sizeof f, ms);
	size_t   segment;
	size_t   offset;
	uint32_t sum;

	if (got < 54u || f[0] != 0x52 || f[5] != me || memcmp(f + 6, hull_mac, 6u) != 0 ||
	    get16(f + 12) != 0x0800u || f[23] != 6u || f[33] != me ||
	    sum16(0u, f + 14, 20u) != 0xffffu || get16(f + 34) != 53u || get16(f + 36) != 4100u)
		return -1;
	segment = get16(f + 16) - 20u;
	memcpy(pseudo, f + 26, 8u);
	pseudo[8] = 0u;
	pseudo[9] = 6u;
	put16(pseudo + 10, (unsigned)segment);
	sum = sum16(0u, pseudo, 12u);
	if (sum16(sum, f + 34, segment) != 0xffffu)
		return -1;
	offset = (size_t)(f[46] >> 4) * 4u;
	*seq = (uint32_t)get16(f + 38) << 16 | get16(f + 40);
	*ack = (uint32_t)get16(f + 42) << 16 | get16(f + 44);
	*length = segment - offset;
	memcpy(data, f + 34 + offset, *length);
	return f[47];
}

/* A DNS question for `name`, type A, as TCP carries it: after its length. */
static size_t tcp_query(uint8_t *out, const char *name)
{
	size_t at = 2u + 12u;

	memset(out, 0, 14u);
	out[2] = 0x56;
	out[3] = 0x78;
	out[4] = 0x01;
	out[7] = 1u;
	for (const char *label = name; *label != '\0';) {
		size_t length = strcspn(label, ".");

		out[at++] = (uint8_t)length;
		memcpy(out + at, label, length);
		at += length;
		label += length + (label[length] == '.');
	}
	out[at++] = 0u;
	put16(out + at, 1u);
	put16(out + at + 2u, 1u);
	at += 4u;
	put16(out, (unsigned)(at - 2u));
	return at;
}

/* This process's open descriptors, which are the switch's too. */
static int descriptors(void)
{
	DIR *dir = opendir("/proc/self/fd");
	int  n = 0;

	if (dir == NULL)
		return -1;
	while (readdir(dir) != NULL)
		n++;
	closedir(dir);
	return n;
}

/* descriptors(), once it holds still: the cases before close their ports'
 * ends, and the switch its own a moment later. Two seconds at most. */
static int settled_descriptors(void)
{
	int last = descriptors();

	for (uint64_t until = ms_now() + 2000u; ms_now() < until;) {
		struct timespec pause = { 0, 200000000 };
		int             now;

		nanosleep(&pause, NULL);
		if ((now = descriptors()) == last)
			break;
		last = now;
	}
	return last;
}

/* A resolver of the test's own on the loopback, for .1 to ask on to:
 * its socket, its port in `*port`. */
static int resolver_of(unsigned *port)
{
	struct sockaddr_in at = { .sin_family = AF_INET };
	socklen_t          length = sizeof at;
	int                fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);

	at.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	if (fd < 0 || bind(fd, (struct sockaddr *)&at, sizeof at) != 0 ||
	    getsockname(fd, (struct sockaddr *)&at, &length) != 0) {
		fail("a resolver of the test's", strerror(errno));
		return -1;
	}
	*port = ntohs(at.sin_port);
	return fd;
}

/* The question the switch asked `fd`, within `ms`: its length, the
 * datagram in `out` and who sent it in `from`; 0 where none came. */
static size_t asked_of(int fd, uint8_t *out, size_t cap, struct sockaddr_in *from, int ms)
{
	struct pollfd p = { fd, POLLIN, 0 };
	socklen_t     length = sizeof *from;
	ssize_t       n;

	if (fd < 0 || poll(&p, 1, ms) != 1)
		return 0u;
	n = recvfrom(fd, out, cap, 0, (struct sockaddr *)from, &length);
	return n > 0 ? (size_t)n : 0u;
}

/* `question` answered from `fd` to `to`: an A of 192.0.2.7, or where
 * `padding` is given, that many bytes of TXT besides, past a frame. */
static void answer_from(int fd, const uint8_t *question, size_t length,
                        const struct sockaddr_in *to, size_t padding)
{
	uint8_t reply[4096];
	size_t  at = length;

	memcpy(reply, question, length);
	reply[2] = 0x81;   /* an answer, recursion desired */
	reply[3] = 0x80;   /* recursion available, no error */
	put16(reply + 6, padding != 0u ? 2u : 1u);
	put16(reply + at, 0xc00cu);
	put16(reply + at + 2u, 1u);
	put16(reply + at + 4u, 1u);
	put16(reply + at + 6u, 0u);
	put16(reply + at + 8u, 60u);
	put16(reply + at + 10u, 4u);
	reply[at + 12u] = 192;
	reply[at + 13u] = 0;
	reply[at + 14u] = 2;
	reply[at + 15u] = 7;
	at += 16u;
	if (padding != 0u) {
		put16(reply + at, 0xc00cu);
		put16(reply + at + 2u, 16u);
		put16(reply + at + 4u, 1u);
		put16(reply + at + 6u, 0u);
		put16(reply + at + 8u, 60u);
		put16(reply + at + 10u, (unsigned)padding);
		at += 12u;
		memset(reply + at, 'x', padding);
		for (size_t i = 0u; i < padding; i += 200u)
			reply[at + i] = (uint8_t)(padding - i < 200u ? padding - i - 1u : 199u);
		at += padding;
	}
	sendto(fd, reply, at, 0, (const struct sockaddr *)to, sizeof *to);
}

/* The frames in pcap file `path`, their tags (each frame's 15th byte) into
 * `tags`: how many, or -1 where its header is not pcap's of Ethernet with
 * nanosecond times, or a record is not whole. */
static int captured(const char *path, char *tags, int cap)
{
	FILE    *file = fopen(path, "rb");
	uint32_t header[6];
	uint32_t record[4];
	uint8_t  frame[65536];
	int      n = 0;

	if (file == NULL || fread(header, sizeof header, 1u, file) != 1u || header[0] != 0xa1b23c4du ||
	    header[1] != (2u | 4u << 16) || header[5] != 1u) {
		if (file != NULL)
			fclose(file);
		return -1;
	}
	while (fread(record, sizeof record, 1u, file) == 1u) {
		if (record[1] >= 1000000000u || record[2] != record[3] || record[2] > sizeof frame ||
		    fread(frame, record[2], 1u, file) != 1u) {
			n = -1;
			break;
		}
		if (n < cap)
			tags[n] = record[2] > 14u ? (char)frame[14] : 0;
		n++;
	}
	fclose(file);
	return n;
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
	{
		/* The switch injecting its own frame: to one target, counted
		 * injected and not reaching another port; then a random flood
		 * to all, counted on each. */
		uint8_t  data[64];
		size_t   len = frame(data, 0xffu, 9u, 'I');
		int      a = port_of("inj", "a");
		int      b = port_of("inj", "b");
		char     why[256] = "";

		if (hull_segment_inject("inj", "b", data + 4, len - 4u, 0, 0u, 1u, 0u, 0u, why,
		                        sizeof why) != 0)
			fail("an inject", why);
		expect("an injected frame reaches its target", b, 'I');
		expect("an injected frame spares another port", a, 0);
		check("the inject counted on its target",
		      counted("inj", "b", HULL_SEGMENT_IN).injected == 1u &&
		      counted("inj", "a", HULL_SEGMENT_IN).injected == 0u);
		if (hull_segment_inject("inj", NULL, NULL, 0u, 1, 20u, 3u, 0u, 1u, why, sizeof why) != 0)
			fail("a flood", why);
		for (int i = 0; i < 3; i++) {
			(void)next_tag(a, 1000);
			(void)next_tag(b, 1000);
		}
		check("a flood counted on every port",
		      counted("inj", "a", HULL_SEGMENT_IN).injected == 3u &&
		      counted("inj", "b", HULL_SEGMENT_IN).injected == 4u);
		close(a);
		close(b);
	}
	{
		/* Addressed joins (Appendix D): each file draws its own machines'
		 * addresses, so the switch refuses a machine of another id taking
		 * one already held, or another subnet for the segment. 10.99.0.0/24
		 * is 0x0a630000. */
		char     why[256] = "";
		int      web = hull_segment_port_addressed("named", "web", 0x0a630045u, 24u, 0, why,
		                                           sizeof why);
		int      db = hull_segment_port_addressed("named", "db", 0x0a63001fu, 24u, 0, why,
		                                          sizeof why);
		int      raw = hull_segment_port("named", "raw", why, sizeof why);
		int      again = hull_segment_port_addressed("named", "web", 0x0a630045u, 24u, 0, why,
		                                             sizeof why);
		int      taken;
		int      wider;
		int      later = -1;
		uint64_t until;

		check("two machines addressed apart, and a bare one, on one segment",
		      web >= 0 && db >= 0 && raw >= 0);
		check("a machine's own address again, a vm's second attempt", again >= 0);
		why[0] = '\0';
		taken = hull_segment_port_addressed("named", "other", 0x0a630045u, 24u, 0, why, sizeof why);
		check("another file's machine on a held address refused", taken < 0);
		check("its reason names the holder",
		      strcmp(why, "its address on segment named, 10.99.0.69, is machine web's") == 0);
		why[0] = '\0';
		wider = hull_segment_port_addressed("named", "other", 0x0a620007u, 16u, 0, why, sizeof why);
		check("another subnet for the segment refused", wider < 0);
		check("its reason names both subnets",
		      strcmp(why, "segment named is 10.99.0.0/24, as machine web has it, and this "
		                  "machine gives it 10.98.0.0/16") == 0);
		/* Once the holder's ports have gone, the address is free. */
		close(web);
		close(again);
		for (until = ms_now() + 2000u; later < 0 && ms_now() < until;) {
			later = hull_segment_port_addressed("named", "other", 0x0a630045u, 24u, 0, why,
			                                    sizeof why);
			if (later < 0)
				usleep(10000u);
		}
		check("an address free once its holder has gone", later >= 0);
		if (taken >= 0)
			close(taken);
		if (wider >= 0)
			close(wider);
		if (later >= 0)
			close(later);
		if (db >= 0)
			close(db);
		if (raw >= 0)
			close(raw);
	}
	{
		/* hull at .1 of a segment it addresses, 10.55.0.0/24: ARP for .1
		 * answered to its asker alone; A for an addressed machine's id and
		 * id.hull, PTR for its address, NXDOMAIN for a name or address it
		 * has not, REFUSED for one outside, each with right sums; a machine
		 * joining later named, and one gone not. */
		char    why[256] = "";
		int     web = hull_segment_port_addressed("dnsd", "web", 0x0a37000au, 24u, 0, why,
		                                          sizeof why);
		int     db = hull_segment_port_addressed("dnsd", "db", 0x0a370014u, 24u, 0, why, sizeof why);
		int     late;
		int     bare = port_of("dnsb", "x");
		int     peer = port_of("dnsb", "y");
		uint8_t f[1514];
		uint8_t data[300];
		size_t  length;
		int     rcode;

		send_arp(web, 10u, 1u);
		length = next_raw(web, f, sizeof f, 1000);
		check("ARP for .1 answered by hull's MAC",
		      length >= 42u && get16(f + 12) == 0x0806u && get16(f + 20) == 2u &&
		      memcmp(f + 22, hull_mac, 6u) == 0 && f[28] == 10u && f[31] == 1u &&
		      f[5] == 10u && f[41] == 10u);
		check("ARP for .1, not to another port", next_raw(db, f, sizeof f, 100) == 0u);
		send_dns(web, 10u, "db", 1u);
		rcode = dns_reply(web, 10u, data, &length);
		check("A for an addressed machine's id", rcode == 0 && length == 4u && data[0] == 10u &&
		                                         data[1] == 55u && data[3] == 20u);
		send_dns(web, 10u, "DB.hull", 1u);
		rcode = dns_reply(web, 10u, data, &length);
		check("A for id.hull, in any case", rcode == 0 && length == 4u && data[3] == 20u);
		send_dns(web, 10u, "20.0.55.10.in-addr.arpa", 12u);
		rcode = dns_reply(web, 10u, data, &length);
		check("PTR for an addressed machine's address",
		      rcode == 0 && length == 9u && memcmp(data, "\2db\4hull", 9u) == 0);
		send_dns(web, 10u, "db", 28u);
		rcode = dns_reply(web, 10u, data, &length);
		check("AAAA for a named machine, no data", rcode == 0 && length == 0u);
		{
			/* A segment with an IPv6 network too: AAAA for a machine its
			 * plan names, live or not, and none for one it gives none;
			 * PTR under ip6.arpa back to its name. */
			static const uint8_t six_web[16] = { 0xfd, 0x42, 0, 0x55, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x0a };
			static const uint8_t six_db[16]  = { 0xfd, 0x42, 0, 0x55, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x01, 0x14 };
			hull_segment_host    hosts[3] = { { "web", 0x0a37000au, 1, { 0 } },
			                                  { "db", 0x0a370014u, 1, { 0 } },
			                                  { "old", 0x0a37001eu, 0, { 0 } } };
			int                  six;
			char                 reverse[80];
			size_t               at = 0u;

			memcpy(hosts[0].address6, six_web, 16u);
			memcpy(hosts[1].address6, six_db, 16u);
			six = hull_segment_port_named("dns6", "web", 0x0a37000au, 24u, six_web, 64u, 0, hosts,
			                              3u, why, sizeof why);
			if (six < 0) {
				check("a port with IPv6 names", 0);
			} else {
				send_dns(six, 10u, "db", 28u);
				rcode = dns_reply(six, 10u, data, &length);
				check("AAAA for a machine its plan gives an IPv6 address",
				      rcode == 0 && length == 16u && memcmp(data, six_db, 16u) == 0);
				send_dns(six, 10u, "web.hull", 28u);
				rcode = dns_reply(six, 10u, data, &length);
				check("AAAA for the live machine itself",
				      rcode == 0 && length == 16u && memcmp(data, six_web, 16u) == 0);
				send_dns(six, 10u, "old", 28u);
				rcode = dns_reply(six, 10u, data, &length);
				check("AAAA for one given none, no data", rcode == 0 && length == 0u);
				for (int k = 15; k >= 0; k--)
					at += (size_t)snprintf(reverse + at, sizeof reverse - at, "%x.%x.",
					                       six_db[k] & 15u, six_db[k] >> 4);
				snprintf(reverse + at, sizeof reverse - at, "ip6.arpa");
				send_dns(six, 10u, reverse, 12u);
				rcode = dns_reply(six, 10u, data, &length);
				check("PTR under ip6.arpa for a machine's IPv6 address",
				      rcode == 0 && length == 9u && memcmp(data, "\2db\4hull", 9u) == 0);
				close(six);
			}
		}
		{
			/* A segment of IPv6 alone: hull at ::1 answers neighbour
			 * discovery for itself and no other, ping, and DNS over UDP,
			 * a datagram with no sum refused as IPv6 has it. */
			static const uint8_t solicited[6] = { 0x33, 0x33, 0xff, 0, 0, 1 };
			static const uint8_t group[16] = { 0xff, 0x02, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0xff, 0, 0, 1 };
			uint8_t              self6[16] = { 0xfd, 0x42, 0, 0x55, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1 };
			uint8_t              web6[16];
			uint8_t              db6[16];
			hull_segment_host    hosts[2] = { { "web", 0u, 1, { 0 } }, { "db", 0u, 1, { 0 } } };
			int                  w;
			int                  d;

			memcpy(web6, self6, 16u);
			web6[15] = 10u;
			memcpy(db6, self6, 16u);
			db6[15] = 20u;
			memcpy(hosts[0].address6, web6, 16u);
			memcpy(hosts[1].address6, db6, 16u);
			w = hull_segment_port_named("only6", "web", 0u, 0u, web6, 64u, 0, hosts, 2u, why,
			                            sizeof why);
			d = hull_segment_port_named("only6", "db", 0u, 0u, db6, 64u, 0, hosts, 2u, why,
			                            sizeof why);
			if (w < 0 || d < 0) {
				check("two ports on a segment of IPv6 alone", 0);
			} else {
				uint8_t ns[32] = { 135, 0, 0, 0, 0, 0, 0, 0 };
				uint8_t echo[12] = { 128, 0, 0, 0, 0x42, 0x42, 0, 1, 'p', 'i', 'n', 'g' };
				uint8_t udp[64] = { 0x0f, 0xa0, 0, 53 };
				uint8_t r[1514];
				size_t  n;
				size_t  at = 8u + 12u;

				memcpy(ns + 8, self6, 16u);
				ns[24] = 1u;   /* source link-layer address */
				ns[25] = 1u;
				memcpy(ns + 26, "\x52\x54\x00\x00\x00\x0a", 6u);
				send_v6(w, 10u, solicited, group, 58u, ns, sizeof ns, 2u);
				n = next_raw(w, r, sizeof r, 1000);
				check("a neighbour solicitation for ::1, advertised with hull's MAC",
				      n >= 54u + 32u && get16(r + 12) == 0x86ddu && r[20] == 58u && r[21] == 255u &&
				      r[54] == 136u && (r[58] & 0x60u) == 0x60u && memcmp(r + 62, self6, 16u) == 0 &&
				      r[78] == 2u && memcmp(r + 80, hull_mac, 6u) == 0 &&
				      memcmp(r + 38, only6_net, 8u) == 0 && r[53] == 10u &&
				      sum16(sum16(0u, r + 22, 32u) + 32u + 58u, r + 54, 32u) == 0xffffu);
				check("the solicitation, not to another port", next_raw(d, r, sizeof r, 100) == 0u);
				memcpy(ns + 8, db6, 16u);
				send_v6(w, 10u, solicited, group, 58u, ns, sizeof ns, 2u);
				check("one for another target, not answered", next_raw(w, r, sizeof r, 200) == 0u);
				send_v6(w, 10u, hull_mac, self6, 58u, echo, sizeof echo, 2u);
				n = next_raw(w, r, sizeof r, 1000);
				check("ping ::1, answered",
				      n >= 54u + 12u && r[54] == 129u && memcmp(r + 58, "\x42\x42\x00\x01ping", 8u) == 0 &&
				      sum16(sum16(0u, r + 22, 32u) + 12u + 58u, r + 54, 12u) == 0xffffu);
				/* An AAAA question for db, over UDP to [::1]:53. */
				udp[8] = 0x12;
				udp[9] = 0x34;
				udp[10] = 0x01;
				udp[13] = 1u;
				memcpy(udp + at, "\2db\0\0\x1c\0\1", 8u);
				at += 8u;
				put16(udp + 4, (unsigned)at);
				send_v6(w, 10u, hull_mac, self6, 17u, udp, at, 6u);
				n = next_raw(w, r, sizeof r, 1000);
				check("AAAA asked of ::1 over UDP, answered over IPv6",
				      n >= 54u + at + 28u && r[20] == 17u && get16(r + 54) == 53u &&
				      get16(r + 62) == 0x1234u && (r[64] & 0x80u) != 0u && get16(r + 68) == 1u &&
				      memcmp(r + 54 + at + 12u, db6, 16u) == 0 &&
				      sum16(sum16(0u, r + 22, 32u) + get16(r + 58) + 17u, r + 54, get16(r + 58)) ==
				          0xffffu);
				udp[6] = 0u;   /* the sum the last send wrote, gone */
				udp[7] = 0u;
				send_v6(w, 10u, hull_mac, self6, 17u, udp, at, at);
				check("one with no UDP sum, refused over IPv6", next_raw(w, r, sizeof r, 200) == 0u);
				{
					/* A machine with a way out, asking a name outside: asked
					 * on, and the answer given back to it over IPv6. */
					unsigned           up_port = 0u;
					int                up = resolver_of(&up_port);
					const char        *up_server = "127.0.0.1";
					int                o;
					struct sockaddr_in up_from;
					uint8_t            q[600];
					size_t             got;
					uint8_t            out6[16];

					memcpy(out6, self6, 16u);
					out6[15] = 30u;
					o = hull_segment_port_named("only6", "out", 0u, 0u, out6, 64u, 1, NULL, 0u, why,
					                            sizeof why);
					hull_segment_resolvers(&up_server, &up_port, 1u);
					at = 8u + 12u;
					memcpy(udp + at, "\7example\3com\0\0\1\0\1", 17u);
					at += 17u;
					put16(udp + 4, (unsigned)at);
					if (o >= 0)
						send_v6(o, 30u, hull_mac, self6, 17u, udp, at, 6u);
					got = asked_of(up, q, sizeof q, &up_from, 1000);
					check("a name outside, asked on for an IPv6 asker", got != 0u);
					if (got != 0u)
						answer_from(up, q, got, &up_from, 0u);
					n = o >= 0 ? next_raw(o, r, sizeof r, 1000) : 0u;
					check("its answer, back over IPv6 under the machine's id",
					      n > 54u + 8u + 12u && get16(r + 12) == 0x86ddu && r[20] == 17u &&
					      r[53] == 30u && get16(r + 62) == 0x1234u &&
					      sum16(sum16(0u, r + 22, 32u) + get16(r + 58) + 17u, r + 54, get16(r + 58)) ==
					          0xffffu);
					hull_segment_resolvers(NULL, NULL, 0u);
					if (o >= 0)
						close(o);
					if (up >= 0)
						close(up);
				}
			}
			if (w >= 0)
				close(w);
			if (d >= 0)
				close(d);
		}
		send_dns(web, 10u, "nobody", 1u);
		check("a name it has not, NXDOMAIN", dns_reply(web, 10u, data, &length) == 3);
		/* Sums, read through core/frame.situ before a field is: a UDP sum
		 * that is right is answered, and one bit off in it or in the IPv4
		 * header's, nothing, as a host's stack drops such a datagram. */
		send_dns_summed(web, 10u, "db", 1u, SUM_UDP);
		check("a question with its UDP sum, answered", dns_reply(web, 10u, data, &length) == 0);
		send_dns_summed(web, 10u, "db", 1u, SUM_UDP_WRONG);
		check("a question whose UDP sum is wrong, not answered",
		      next_raw(web, f, sizeof f, 300) == 0u);
		send_dns_summed(web, 10u, "db", 1u, SUM_IP_WRONG);
		check("a question whose IPv4 header sum is wrong, not answered",
		      next_raw(web, f, sizeof f, 300) == 0u);
		/* Ping: an echo request answered with its data, through the
		 * asker's own way in; one with its sum wrong, and an echo reply
		 * sent to .1, not. */
		send_ping(web, 10u, 8u, "hull ping, an odd length", 0);
		check("ping .1, answered with its data", ping_reply(web, 10u, "hull ping, an odd length"));
		send_ping(web, 10u, 8u, "", 0);
		check("ping .1 with no data, answered", ping_reply(web, 10u, ""));
		send_ping(web, 10u, 8u, "wrong", 1);
		check("a ping whose ICMP sum is wrong, not answered",
		      next_raw(web, f, sizeof f, 300) == 0u);
		send_ping(web, 10u, 0u, "reply", 0);
		check("an echo reply sent to .1, not answered", next_raw(web, f, sizeof f, 300) == 0u);
		check("the pings, not to another port", next_raw(db, f, sizeof f, 100) == 0u);
		{
			hull_impairment slow = { 0 };
			uint64_t        sent;

			slow.delay_ms = 80u;
			impair("dnsd", "web", HULL_SEGMENT_IN, &slow);
			sent = ms_now();
			send_ping(web, 10u, 8u, "slow", 0);
			check("ping .1 over a link delayed 80 ms, its reply 80 ms late",
			      ping_reply(web, 10u, "slow") && ms_now() - sent >= 75u);
			slow.delay_ms = 0u;
			impair("dnsd", "web", HULL_SEGMENT_IN, &slow);
		}
		send_dns(web, 10u, "99.0.55.10.in-addr.arpa", 12u);
		check("an address it has not, NXDOMAIN", dns_reply(web, 10u, data, &length) == 3);
		send_dns(web, 10u, "example.com", 1u);
		check("a name outside, REFUSED", dns_reply(web, 10u, data, &length) == 5);
		send_dns(web, 10u, "9.9.9.9.in-addr.arpa", 12u);
		check("an address outside, REFUSED", dns_reply(web, 10u, data, &length) == 5);
		check("the questions, not to another port", next_raw(db, f, sizeof f, 100) == 0u);
		late = hull_segment_port_addressed("dnsd", "late", 0x0a37001eu, 24u, 0, why, sizeof why);
		send_dns(web, 10u, "late", 1u);
		rcode = dns_reply(web, 10u, data, &length);
		check("a machine joining later, named", rcode == 0 && length == 4u && data[3] == 30u);
		close(db);
		rcode = 0;
		for (uint64_t until = ms_now() + 2000u; rcode != 3 && ms_now() < until;) {
			send_dns(web, 10u, "db", 1u);
			rcode = dns_reply(web, 10u, data, &length);
		}
		check("a machine gone, no longer named", rcode == 3);
		/* A bare segment has no hull at .1: its ARP floods as any other. */
		send_arp(bare, 10u, 1u);
		check("no .1 on a bare segment", next_raw(bare, f, sizeof f, 200) == 0u &&
		                                 next_raw(peer, f, sizeof f, 1000) == 60u);
		close(web);
		close(late);
		close(bare);
		close(peer);
	}
	{
		/* hull at .1 asking on, for a machine with a way out: a name
		 * outside, an address outside and a single label it has not each
		 * asked of the host's resolver, here the test's, under an id of
		 * the switch's, and its answer given back under the machine's own;
		 * hull's own names still answered by it, and asked of nobody, and
		 * so anything under .hull it has not; an answer from another than
		 * the one asked, not taken; one past a frame sent in IP fragments
		 * that make it whole; and a machine of (network none) on the
		 * same segment still refused, asking nobody; and once the segment
		 * is gone, the socket it asked on from gone with it. */
		int                before = settled_descriptors();
		int                after = -1;
		char               why[256] = "";
		unsigned           resolver_port = 0u;
		int                resolver = resolver_of(&resolver_port);
		int                stranger = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
		const char        *server = "127.0.0.1";
		int                web = hull_segment_port_addressed("dnsf", "web", 0x0a37000au, 24u, 1,
		                                                     why, sizeof why);
		int                db = hull_segment_port_addressed("dnsf", "db", 0x0a370014u, 24u, 0,
		                                                    why, sizeof why);
		uint8_t            f[1514];
		uint8_t            q[600];
		uint8_t            data[300];
		size_t             length;
		size_t             got;
		struct sockaddr_in from;
		int                rcode;

		hull_segment_resolvers(&server, &resolver_port, 1u);
		send_dns(web, 10u, "example.com", 1u);
		got = asked_of(resolver, q, sizeof q, &from, 1000);
		check("a name outside, asked on",
		      got == 12u + 13u + 4u && memcmp(q + 12, "\7example\3com", 13u) == 0);
		if (got != 0u)
			answer_from(resolver, q, got, &from, 0u);
		rcode = dns_reply(web, 10u, data, &length);
		check("its answer, back under the machine's id",
		      rcode == 0 && length == 4u && data[0] == 192u && data[3] == 7u);
		send_dns(web, 10u, "9.9.9.9.in-addr.arpa", 12u);
		check("an address outside, asked on",
		      asked_of(resolver, q, sizeof q, &from, 1000) != 0u);
		send_dns(web, 10u, "nobody", 1u);
		got = asked_of(resolver, q, sizeof q, &from, 1000);
		check("a single label it has not, asked on", got != 0u);
		if (got != 0u && stranger >= 0) {
			sendto(stranger, q, got, 0, (struct sockaddr *)&from, sizeof from);
			check("an answer from another than the one asked, not taken",
			      next_raw(web, f, sizeof f, 200) == 0u);
		}
		send_dns(web, 10u, "db", 1u);
		rcode = dns_reply(web, 10u, data, &length);
		check("its own names, still its", rcode == 0 && length == 4u && data[3] == 20u &&
		                                  asked_of(resolver, q, sizeof q, &from, 100) == 0u);
		send_dns(web, 10u, "no.such.hull", 1u);
		check("under .hull, NXDOMAIN and asked of nobody",
		      dns_reply(web, 10u, data, &length) == 3 &&
		      asked_of(resolver, q, sizeof q, &from, 100) == 0u);
		send_dns(web, 10u, "big.example", 16u);
		got = asked_of(resolver, q, sizeof q, &from, 1000);
		if (got != 0u)
			answer_from(resolver, q, got, &from, 1800u);
		{
			/* Two IP fragments, the first full and saying more come,
			 * the second at its offset: put back together, the whole
			 * answer, its UDP sum right over all of it, under the
			 * machine's id. */
			uint8_t  second[1514];
			uint8_t  whole[4096];
			size_t   first_length = next_raw(web, f, sizeof f, 1000);
			size_t   second_length = next_raw(web, second, sizeof second, 1000);
			size_t   n = 0u;
			uint32_t sum;
			int      shaped = first_length == 1514u && second_length > 34u &&
			                  (get16(f + 20) & 0x2000u) != 0u && (get16(f + 20) & 0x1fffu) == 0u &&
			                  (get16(second + 20) & 0x2000u) == 0u &&
			                  (get16(second + 20) & 0x1fffu) == 1480u / 8u &&
			                  get16(f + 18) == get16(second + 18) &&
			                  sum16(0u, f + 14, 20u) == 0xffffu &&
			                  sum16(0u, second + 14, 20u) == 0xffffu;

			if (shaped) {
				size_t tail = get16(second + 16) - 20u;

				memcpy(whole, f + 34, 1480u);
				memcpy(whole + 1480u, second + 34, tail);
				n = 1480u + tail;
			}
			sum = sum16(0u, f + 26, 8u) + 17u + (uint32_t)n;
			check("an answer past a frame, in fragments that make it whole",
			      shaped && get16(whole + 4) == n && sum16(sum, whole, n) == 0xffffu &&
			      get16(whole + 8) == 0x1234u && get16(whole + 14) == 2u &&
			      n == 8u + got + 16u + 12u + 1800u);
		}
		send_dns(db, 20u, "example.com", 1u);
		check("a machine of (network none), refused",
		      dns_reply(db, 20u, data, &length) == 5 &&
		      asked_of(resolver, q, sizeof q, &from, 100) == 0u);
		{
			/* A host whose resolver is IPv6 alone, `nameserver ::1`: asked
			 * on there, and its answer given back. */
			struct sockaddr_in6 at6 = { .sin6_family = AF_INET6, .sin6_addr = IN6ADDR_LOOPBACK_INIT };
			struct sockaddr_in6 from6;
			socklen_t           at6_length = sizeof at6;
			int                 resolver6 = socket(AF_INET6, SOCK_DGRAM | SOCK_CLOEXEC, 0);
			unsigned            port6 = 0u;
			const char         *server6 = "::1";

			if (resolver6 >= 0 && bind(resolver6, (struct sockaddr *)&at6, sizeof at6) == 0 &&
			    getsockname(resolver6, (struct sockaddr *)&at6, &at6_length) == 0) {
				struct pollfd p = { resolver6, POLLIN, 0 };
				socklen_t     from6_length = sizeof from6;
				ssize_t       n = 0;

				port6 = ntohs(at6.sin6_port);
				hull_segment_resolvers(&server6, &port6, 1u);
				send_dns(web, 10u, "example.com", 1u);
				if (poll(&p, 1u, 1000) == 1)
					n = recvfrom(resolver6, q, sizeof q, 0, (struct sockaddr *)&from6,
					             &from6_length);
				check("a name outside, asked on of an IPv6 resolver",
				      n == 12 + 13 + 4 && memcmp(q + 12, "\7example\3com", 13u) == 0);
				if (n > 0) {
					uint8_t reply[600];

					memcpy(reply, q, (size_t)n);
					reply[2] = 0x81;
					reply[3] = 0x83;   /* NXDOMAIN, as an answer of its own */
					sendto(resolver6, reply, (size_t)n, 0, (struct sockaddr *)&from6,
					       from6_length);
				}
				check("its answer, back from the IPv6 resolver",
				      dns_reply(web, 10u, data, &length) == 3);
			} else {
				check("a resolver of the test's on ::1", 0);
			}
			if (resolver6 >= 0)
				close(resolver6);
		}
		hull_segment_resolvers(NULL, NULL, 0u);
		close(web);
		close(db);
		if (resolver >= 0)
			close(resolver);
		if (stranger >= 0)
			close(stranger);
		for (uint64_t until = ms_now() + 2000u; after != before && ms_now() < until;) {
			struct timespec pause = { 0, 50000000 };

			nanosleep(&pause, NULL);
			after = descriptors();
		}
		check("the socket asked on from, gone with the segment", before > 0 && after == before);
	}
	{
		/* DNS over TCP at .1: a connection opened by SYN and SYN-ACK, a
		 * query after its length answered after its length, the query
		 * sent again answered again, an end by FIN each way, a segment
		 * for no connection refused by RST, and a name outside asked on
		 * as a datagram's is, its answer back over TCP. */
		char     why[256] = "";
		unsigned resolver_port = 0u;
		int      resolver = resolver_of(&resolver_port);
		const char *server = "127.0.0.1";
		int      web = hull_segment_port_addressed("dnst", "web", 0x0a37000au, 24u, 1, why,
		                                           sizeof why);
		int      db = hull_segment_port_addressed("dnst", "db", 0x0a370014u, 24u, 0, why,
		                                          sizeof why);
		uint8_t  data[1514];
		uint8_t  query[300];
		uint8_t  q[600];
		struct sockaddr_in from;
		size_t   length = 0u;
		size_t   n;
		size_t   first;
		size_t   got;
		uint32_t seq = 0u;
		uint32_t ack = 0u;
		uint32_t theirs;
		int      flags;

		hull_segment_resolvers(&server, &resolver_port, 1u);
		tcp_sum_wrong = 1;
		send_tcp(web, 10u, 0x02u, 1000u, 0u, NULL, 0u);
		tcp_sum_wrong = 0;
		check("a SYN whose TCP sum is wrong, not answered",
		      next_tcp(web, 10u, &seq, &ack, data, &length, 300) < 0);
		send_tcp(web, 10u, 0x02u, 1000u, 0u, NULL, 0u);
		flags = next_tcp(web, 10u, &seq, &ack, data, &length, 1000);
		check("a SYN to .1:53, answered SYN-ACK", flags == 0x12 && ack == 1001u);
		theirs = seq + 1u;
		n = tcp_query(query, "db");
		send_tcp(web, 10u, 0x18u, 1001u, theirs, query, n);
		do
			flags = next_tcp(web, 10u, &seq, &ack, data, &length, 1000);
		while (flags >= 0 && length == 0u);
		check("a query on it, answered after its length",
		      flags >= 0 && seq == theirs && ack == 1001u + n && length == 2u + get16(data) &&
		      get16(data + 2) == 0x5678u && (data[4] & 0x80u) != 0u && get16(data + 8) == 1u &&
		      data[length - 1u] == 20u);
		send_tcp(web, 10u, 0x18u, 1001u, theirs, query, n);
		do
			flags = next_tcp(web, 10u, &seq, &ack, data, &length, 1000);
		while (flags >= 0 && length == 0u);
		check("the query sent again, as on a lossy link: its answer sent again",
		      flags >= 0 && seq == theirs && length == 2u + get16(data));
		theirs += (uint32_t)length;
		first = n;
		n = tcp_query(query, "example.com");
		send_tcp(web, 10u, 0x18u, 1001u + (uint32_t)first, theirs, query, n);
		got = asked_of(resolver, q, sizeof q, &from, 1000);
		check("a name outside, asked on from TCP", got != 0u);
		if (got != 0u)
			answer_from(resolver, q, got, &from, 0u);
		do
			flags = next_tcp(web, 10u, &seq, &ack, data, &length, 1000);
		while (flags >= 0 && length == 0u);
		check("its answer, back over TCP under the machine's id",
		      flags >= 0 && seq == theirs && get16(data + 2) == 0x5678u &&
		      data[length - 4u] == 192u && data[length - 1u] == 7u);
		theirs += (uint32_t)length;
		send_tcp(web, 10u, 0x11u, 1001u + (uint32_t)(first + n), theirs, NULL, 0u);
		flags = next_tcp(web, 10u, &seq, &ack, data, &length, 1000);
		check("its FIN, answered by .1's", flags == 0x11 && seq == theirs &&
		                                    ack == 1002u + (uint32_t)(first + n));
		send_tcp(web, 10u, 0x10u, 1002u + (uint32_t)(first + n), theirs + 1u, NULL, 0u);
		send_tcp(web, 10u, 0x10u, 5000u, 77u, NULL, 0u);
		flags = next_tcp(web, 10u, &seq, &ack, data, &length, 1000);
		check("a segment for no connection, refused by RST", flags == 0x04 && seq == 77u);
		send_tcp(db, 20u, 0x02u, 1000u, 0u, NULL, 0u);
		check("hull's own names on TCP for a machine of (network none) too",
		      next_tcp(db, 20u, &seq, &ack, data, &length, 1000) == 0x12);
		hull_segment_resolvers(NULL, NULL, 0u);
		close(web);
		close(db);
		if (resolver >= 0)
			close(resolver);
	}
	{
		/* A capture: x's way out, what it sent before its link's loss, and
		 * y's way in, what it was given after it, each a pcap file of the
		 * frames whole; one machine's alone; a count that closes it; and
		 * a capture ended, or asked of a segment no machine is on or of a
		 * path not whole, refused. */
		char            why[256] = "";
		char            tags[16];
		char            out_path[] = "/tmp/hull-capture-XXXXXX";
		char            in_path[] = "/tmp/hull-capture-XXXXXX";
		hull_impairment lossy = { 0 };
		int             x = port_of("cap", "x");
		int             y = port_of("cap", "y");
		int             z = port_of("cap", "z");
		int             fd = mkstemp(out_path);
		int             n;

		/* Two files of this test's own, removed by name at its end. */
		if (fd >= 0)
			close(fd);
		if ((fd = mkstemp(in_path)) >= 0)
			close(fd);
		check("a capture of x's way out started",
		      hull_segment_capture("cap", "x", HULL_SEGMENT_OUT, out_path, 2u, why, sizeof why) == 0);
		/* Each awaited before the next: two ports' frames have no order
		 * between them. */
		send_frame(x, 2u, 1u, 'a');
		expect("a captured frame still reaches y", y, 'a');
		send_frame(z, 2u, 3u, 'z');   /* another machine's, not x's */
		expect("z's frame reaches y", y, 'z');
		lossy.loss = 1.0;
		impair("cap", "x", HULL_SEGMENT_OUT, &lossy);
		send_frame(x, 2u, 1u, 'b');
		expect("x's lost frame, not to y", y, 0);
		send_frame(x, 2u, 1u, 'c');   /* past the capture's two */
		expect("x's next lost frame, not to y", y, 0);
		n = captured(out_path, tags, 16);
		check("x's way out captured its two frames, lost or not, and not z's",
		      n == 2 && tags[0] == 'a' && tags[1] == 'b');
		check("a capture of y's way in started",
		      hull_segment_capture("cap", NULL, HULL_SEGMENT_IN, in_path, 10u, why, sizeof why) == 0);
		send_frame(x, 2u, 1u, 'd');   /* lost on x's way out */
		send_frame(z, 2u, 3u, 'e');
		expect("z's frame reaches y again", y, 'e');
		check("a capture ended", hull_segment_capture("cap", NULL, HULL_SEGMENT_IN, NULL, 0u, why,
		                                              sizeof why) == 0);
		send_frame(z, 2u, 3u, 'f');
		expect("z's last frame reaches y", y, 'f');
		n = captured(in_path, tags, 16);
		/* y has sent nothing, so z's frame is flooded: x and y are each
		 * given it, and each port's way in is captured. */
		check("every port's way in captured what it was given, not what was lost, until ended",
		      n == 2 && tags[0] == 'e' && tags[1] == 'e');
		check("a capture of a segment no machine is on refused",
		      hull_segment_capture("nocap", NULL, HULL_SEGMENT_OUT, out_path, 1u, why,
		                           sizeof why) != 0);
		check("a capture's path not whole refused",
		      hull_segment_capture("cap", NULL, HULL_SEGMENT_OUT, "relative.pcap", 1u, why,
		                           sizeof why) != 0);
		unlink(out_path);
		unlink(in_path);
		close(x);
		close(y);
		close(z);
	}
	{
		/* Inject onto a segment no machine is on is refused. */
		char why[256] = "";

		cases++;
		if (hull_segment_inject("void", NULL, NULL, 0u, 1, 20u, 1u, 0u, 0u, why, sizeof why) == 0)
			fail("an inject with no machine", "was taken");
	}
	printf("segment: %u cases, %u failed\n", cases, failures);
	return failures != 0;
}
