/*
 * segment.c -- Appendix D's segments: a learning switch in the core for
 * each, which a machine's NIC joins by a port. See segment.h.
 *
 * A port is a stream socket, one end the core's and the other the
 * machine's, framed as qemu's `-netdev stream` frames it: a 32-bit length
 * in network order, then the frame. A stream rather than datagrams, since
 * the far end closing is then an end of file, which is how a port goes.
 *
 * One thread serves every segment: a poll() over every port and a wake
 * pipe, which a new port writes. A frame goes to the port its destination
 * was last seen on, or to every other port where it is a broadcast, a
 * multicast or not yet seen, as a switch does. Nothing waits on a port:
 * one whose socket is full keeps what it could not take, and is passed
 * frames no more until it has taken that, so a machine that stops reading
 * loses its own frames and holds back no one else's.
 *
 * Each direction of a port has an impairment, a wire by default, applied
 * where the frame enters the switch (out) and where it leaves (in): so a
 * frame from A to B crosses A's out and then B's in, and two machines'
 * links can differ each way. A frame an impairment holds back waits in a
 * queue of its direction's, ordered by when it is due, and the poll()
 * wakes for the first that is.
 */

#define _GNU_SOURCE

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <netinet/in.h>
#include <net/if.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "frame_build.h"
#include "segment.h"

#define MAX_SEGMENTS 64u
#define MAX_PORTS    256u    /* a /24's 253 machines, and room */
#define MAX_LEARNED  1024u
#define MAX_RULES    32u
#define MAX_SOURCES  16u
#define MAX_FRAME    (65536u + 64u)
#define QUEUE_FRAMES 1000u   /* netem's limit, where none is given */

/* A frame held back, its length before it, due at `due` ns. */
typedef struct held {
	struct held *next;
	uint64_t     due;
	size_t       length;
	uint8_t      framed[];
} held;

/* One direction of a port: what it does to frames, its draws, the frames
 * it holds, and what it has counted. */
typedef struct way {
	hull_impairment    impairment;
	int                impaired;   /* anything but a wire */
	hull_tamper        tamper;
	int                tampering;  /* a tamper is set */
	uint64_t           random;     /* the draws' state */
	int                bursting;   /* the Gilbert-Elliott model in its bad state */
	uint64_t           link_free;  /* when the rate's link is next free, ns */
	held              *queue;
	unsigned           queued;
	hull_segment_count count;
} way;

/* A port's way out through .1: its passt's socket, framed as a port's,
 * and what is left of a frame each way. */
typedef struct uplink {
	int     fd;
	uint8_t in[4u + MAX_FRAME];
	size_t  in_length;
	uint8_t out[4u + MAX_FRAME];
	size_t  out_length;
} uplink;

typedef struct port {
	int      fd;
	char     machine[64];
	uint32_t address;   /* hull's address for it, host order (Appendix D) */
	unsigned prefix;    /* its subnet's; 0 where the machine sets its own */
	uint8_t  address6[16];  /* its IPv6 address, where the segment has an IPv6 network */
	unsigned prefix6;   /* that network's; 0 where it has none */
	int      advertise; /* hull's router advertisements for it, on its solicitation */
	int      forwards;  /* its machine has a way out, so .1 asks on for it */
	uplink  *up;        /* its way out through .1, where its segment is a gateway */
	hull_segment_host *hosts;   /* the names its plan gives the segment */
	unsigned           host_count;
	uint8_t  in[4u + MAX_FRAME];
	size_t   in_length;
	uint8_t  out[4u + MAX_FRAME];
	size_t   out_length;
	way      way[2];
} port;

typedef struct learned {
	uint8_t  mac[6];
	unsigned port;
	int      used;
} learned;

#define KIND_IMPAIR 0
#define KIND_TAMPER 1

/* An impairment or a tamper asked for a machine's ports, or every
 * machine's where `machine` is empty: given to each port that joins. */
typedef struct rule {
	char            machine[64];
	int             direction;
	int             kind;          /* KIND_IMPAIR or KIND_TAMPER */
	hull_impairment impairment;
	hull_tamper     tamper;
	int             used;
} rule;

/* The switch putting frames into a segment itself, as an attacker would:
 * toward one machine (empty = every port), a fixed frame or fresh random
 * bytes, `left` more to send, `every_ms` apart, next due at `next`. */
typedef struct source {
	char     machine[64];
	uint8_t  frame[MAX_FRAME];
	size_t   length;
	int      randomize;
	unsigned size;
	uint64_t left;
	unsigned every_ms;
	uint64_t next;
	uint64_t random;
	int      used;
} source;

/* A capture of one way of a segment's ports, or one machine's, into a pcap
 * file: `left` more frames before it closes. */
typedef struct capture {
	int      on;
	int      fd;
	char     machine[64];   /* empty = every port */
	int      direction;
	uint64_t left;
} capture;

typedef struct segment {
	char     key[512];
	port    *ports[MAX_PORTS];
	unsigned port_count;
	learned  table[MAX_LEARNED];
	unsigned next;    /* the entry a new address replaces, round the table */
	rule     rules[MAX_RULES];
	source   sources[MAX_SOURCES];
	capture  capture;
} segment;

/* A question .1 asked on for a machine, until its answer or MAX_ASKED_NS:
 * the id it went with, and where its answer goes back to -- the port, by
 * its segment, machine and socket, since either may be gone by then, and
 * the asker's MAC, address and UDP port. */
#define MAX_ASKED    256u
#define MAX_ASKED_NS 5000000000u

/* An end of what .1 says or is asked: a machine, or .1 itself, by IPv4 or
 * by IPv6. */
typedef struct peer {
	int      six;
	uint32_t v4;          /* host order */
	uint8_t  v6[16];
} peer;

typedef struct asked {
	int                used;
	uint64_t           at;
	uint16_t           id;
	uint16_t            their_id;
	struct sockaddr_in6 server;
	char               key[512];
	char               machine[64];
	int                fd;
	uint8_t            mac[6];
	peer               asker;
	uint8_t            udp_port[2];
	int                conn;            /* the TCP connection it came on, or -1 */
	uint64_t           conn_generation;
} asked;

/*
 * DNS over TCP at .1 (RFC 7766): a connection a machine opened to .1's
 * port 53, the queries on it each after its length as 16 bits, and each
 * answer sent back the same way, on the connection it came on. Small
 * enough for the switch to hold itself, as .1's UDP is: no window but the
 * asker's, which a DNS answer fits, and unacknowledged bytes sent again
 * when the asker shows it lacks them, so an impaired link still carries
 * them. A connection idle MAX_CONN_NS goes.
 */
#define MAX_CONNS   64u
#define MAX_CONN_NS 30000000000u
#define CONN_IN     (2u + 4096u)     /* the queries not yet answered */
#define CONN_OUT    (2u + 65535u)    /* the answers not yet acknowledged */
#define TCP_MSS     1460u
#define CONN_RTO_NS 200000000u       /* the first wait before sending again */
#define CONN_TRIES  7u               /* doubling each time: about 50 s in all */

#define TCP_FIN 0x01u
#define TCP_SYN 0x02u
#define TCP_RST 0x04u
#define TCP_PSH 0x08u
#define TCP_ACK 0x10u

typedef struct conn {
	int      used;
	uint64_t generation;
	uint64_t at;
	char     key[512];
	char     machine[64];
	int      fd;
	uint8_t  mac[6];
	peer     asker;          /* its address */
	uint16_t their_port;
	uint32_t rcv_nxt;        /* the next byte the asker sends */
	uint32_t out_seq;        /* the sequence of out[0] */
	uint32_t snd_nxt;        /* the next byte .1 sends */
	int      syn_acked;      /* the asker has acknowledged .1's SYN */
	int      fin_sent;
	uint64_t resend_at;      /* when what is unacknowledged goes again, or 0 */
	unsigned backoff;
	uint8_t  in[CONN_IN];
	size_t   in_length;
	uint8_t  out[CONN_OUT];
	size_t   out_length;
} conn;

static pthread_mutex_t    lock = PTHREAD_MUTEX_INITIALIZER;
static segment           *segments[MAX_SEGMENTS];
static int                wake[2] = { -1, -1 };
static int                running;
static int                upstream = -1;   /* the socket .1 asks on from, while a segment is */
static asked              asking[MAX_ASKED];
static conn              *conns[MAX_CONNS];
static uint64_t           conn_generation;
static uint64_t           asking_random;
static unsigned           asking_next;     /* the resolver the next question goes to */
static struct sockaddr_in6 resolvers_given[3];
static unsigned           resolvers_given_count;

static void say(char *why, size_t cap, const char *reason)
{
	if (why != NULL && cap > 0u)
		snprintf(why, cap, "%s", reason);
}

static uint64_t now_ns(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return (uint64_t)t.tv_sec * 1000000000u + (uint64_t)t.tv_nsec;
}

/* splitmix64: small, and the same everywhere for a seed. */
/* splitmix64 from `*state`, which it advances: small, and the same
 * everywhere for a seed. */
static uint64_t splitmix(uint64_t *state)
{
	uint64_t z = (*state += 0x9e3779b97f4a7c15u);

	z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9u;
	z = (z ^ (z >> 27)) * 0x94d049bb133111ebu;
	return z ^ (z >> 31);
}

static uint64_t draw(way *w)
{
	return splitmix(&w->random);
}

/* Whether an event of probability `p` happens, from 53 bits of a draw. */
static int happens(way *w, double p)
{
	if (p <= 0.0)
		return 0;
	if (p >= 1.0)
		return 1;
	return (double)(draw(w) >> 11) * (1.0 / 9007199254740992.0) < p;
}

/* `impairment` for `w` of `machine`'s, its draws seeded afresh. */
static void impair_way(way *w, const char *machine, int direction,
                       const hull_impairment *impairment)
{
	const hull_impairment none = { 0 };
	uint64_t              seed = 0xcbf29ce484222325u;

	w->impairment = *impairment;
	w->impaired   = memcmp(impairment, &none, sizeof none) != 0;
	for (const char *c = machine; *c != '\0'; c++)
		seed = (seed ^ (uint8_t)*c) * 0x100000001b3u;
	w->random    = impairment->seed ^ seed ^ (uint64_t)direction << 63;
	w->bursting  = 0;
	w->link_free = 0u;
}

/* The tamper `tamper` for `w` of `machine`'s, its draws seeded afresh. */
static void tamper_way(way *w, const char *machine, int direction, const hull_tamper *tamper)
{
	const hull_tamper none = { 0 };
	uint64_t          seed = 0xcbf29ce484222325u;

	w->tamper    = *tamper;
	w->tampering = memcmp(tamper, &none, sizeof none) != 0;
	for (const char *c = machine; *c != '\0'; c++)
		seed = (seed ^ (uint8_t)*c) * 0x100000001b3u;
	/* A direction impaired and tampered shares one draw stream, seeded
	 * by the impairment; a tamper alone seeds it here. */
	if (!w->impaired)
		w->random = seed ^ (uint64_t)direction << 63;
}

/* The rule `p`'s `direction` takes of `kind`: its machine's own, else
 * every machine's, else none. */
static const rule *rule_for(const segment *s, const port *p, int direction, int kind)
{
	const rule *every = NULL;

	for (unsigned k = 0u; k < MAX_RULES; k++) {
		const rule *r = &s->rules[k];

		if (!r->used || r->direction != direction || r->kind != kind)
			continue;
		if (r->machine[0] == '\0')
			every = r;
		else if (strcmp(r->machine, p->machine) == 0)
			return r;
	}
	return every;
}

/* `p`'s `direction` given the impairment and tamper it takes, if any. */
static void apply_rule(const segment *s, port *p, int direction)
{
	const rule *impair = rule_for(s, p, direction, KIND_IMPAIR);
	const rule *tamper = rule_for(s, p, direction, KIND_TAMPER);

	if (impair != NULL)
		impair_way(&p->way[direction], p->machine, direction, &impair->impairment);
	if (tamper != NULL)
		tamper_way(&p->way[direction], p->machine, direction, &tamper->tamper);
}

/* Where the port on `s` that `mac` was last seen on is, learning `from`
 * for the frame's own source on the way; -1 where it is to be flooded. */
static int lookup(segment *s, const uint8_t *frame, unsigned from)
{
	const uint8_t *destination = frame;
	const uint8_t *source = frame + 6;
	int            found = -1;
	int            known = 0;

	for (unsigned i = 0u; i < MAX_LEARNED; i++) {
		learned *l = &s->table[i];

		if (!l->used)
			continue;
		if (memcmp(l->mac, source, 6u) == 0) {
			l->port = from;
			known = 1;
		}
		if ((destination[0] & 1u) == 0u && memcmp(l->mac, destination, 6u) == 0)
			found = (int)l->port;
	}
	/* A multicast source is no address of a port's. */
	if (!known && (source[0] & 1u) == 0u) {
		learned *l = &s->table[s->next];

		memcpy(l->mac, source, 6u);
		l->port = from;
		l->used = 1;
		s->next = (s->next + 1u) % MAX_LEARNED;
	}
	return found == (int)from ? -2 : found;
}

/* What is left of `p`'s output, as much as its socket takes now. */
static void drain(port *p)
{
	ssize_t n;

	if (p->out_length == 0u)
		return;
	n = send(p->fd, p->out, p->out_length, MSG_DONTWAIT | MSG_NOSIGNAL);
	if (n > 0) {
		memmove(p->out, p->out + n, p->out_length - (size_t)n);
		p->out_length -= (size_t)n;
	}
}

/* `s`'s capture closed, its file left as written. */
static void end_capture(segment *s)
{
	if (s->capture.on)
		close(s->capture.fd);
	s->capture.on = 0;
}

/* The frame, as it crosses `p`'s way `direction`, recorded where `s` is
 * capturing that way of that port: a pcap record, the time to the
 * nanosecond, then the frame whole. A write that fails ends the capture,
 * as its last frame does. */
static void capture_frame(segment *s, const port *p, int direction, const uint8_t *framed,
                          size_t length)
{
	capture        *c = &s->capture;
	uint8_t         head[16];
	struct timespec now;
	struct iovec    record[2];
	uint32_t        size = (uint32_t)(length - 4u);

	if (!c->on || c->direction != direction ||
	    (c->machine[0] != '\0' && strcmp(c->machine, p->machine) != 0))
		return;
	clock_gettime(CLOCK_REALTIME, &now);
	/* pcap's fields are the writer's order, which the magic tells. */
	memcpy(head, &(uint32_t){ (uint32_t)now.tv_sec }, 4u);
	memcpy(head + 4, &(uint32_t){ (uint32_t)now.tv_nsec }, 4u);
	memcpy(head + 8, &size, 4u);
	memcpy(head + 12, &size, 4u);
	record[0].iov_base = head;
	record[0].iov_len  = sizeof head;
	record[1].iov_base = (void *)(framed + 4);
	record[1].iov_len  = size;
	if (writev(c->fd, record, 2) != (ssize_t)(sizeof head + size) || --c->left == 0u)
		end_capture(s);
}

/* The frame, its length before it, written to `p`'s socket, or dropped
 * where `p` has not taken the last. */
static void write_port(segment *s, port *p, const uint8_t *framed, size_t length)
{
	way    *w = &p->way[HULL_SEGMENT_IN];
	ssize_t n;

	if (p->out_length != 0u) {
		w->count.full++;
		return;
	}
	capture_frame(s, p, HULL_SEGMENT_IN, framed, length);
	n = send(p->fd, framed, length, MSG_DONTWAIT | MSG_NOSIGNAL);
	if (n < 0)
		n = 0;
	if ((size_t)n < length) {
		memcpy(p->out, framed + n, length - (size_t)n);
		p->out_length = length - (size_t)n;
	}
	w->count.passed++;
	w->count.passed_bytes += length - 4u;
}

/* A frame the switch injects, written to `p`'s socket, counted apart
 * from what crosses the link: dropped where the socket will not take it. */
static void inject_to(segment *s, port *p, const uint8_t *framed, size_t length)
{
	ssize_t n;

	if (p->out_length != 0u)
		return;
	capture_frame(s, p, HULL_SEGMENT_IN, framed, length);
	n = send(p->fd, framed, length, MSG_DONTWAIT | MSG_NOSIGNAL);
	if (n < 0)
		n = 0;
	if ((size_t)n < length) {
		memcpy(p->out, framed + n, length - (size_t)n);
		p->out_length = length - (size_t)n;
	}
	p->way[HULL_SEGMENT_IN].count.injected++;
}

/* A segment's injectors fired for all that are due by `now`: a frame to
 * the target port, or every port where the target is empty. The first
 * still-pending injector's time, or UINT64_MAX. A spent one is freed. */
static uint64_t fire_sources(segment *s, uint64_t now)
{
	uint64_t first = UINT64_MAX;

	for (unsigned k = 0u; k < MAX_SOURCES; k++) {
		source *src = &s->sources[k];

		while (src->used && src->next <= now) {
			uint8_t  frame[4u + MAX_FRAME];
			size_t   length = src->randomize ? 4u + src->size : 4u + src->length;

			frame[0] = (uint8_t)((length - 4u) >> 24);
			frame[1] = (uint8_t)((length - 4u) >> 16);
			frame[2] = (uint8_t)((length - 4u) >> 8);
			frame[3] = (uint8_t)(length - 4u);
			if (src->randomize)
				for (unsigned b = 0u; b < src->size; b++)
					frame[4u + b] = (uint8_t)(splitmix(&src->random) & 0xffu);
			else
				memcpy(frame + 4u, src->frame, src->length);
			for (unsigned i = 0u; i < s->port_count; i++)
				if (src->machine[0] == '\0' || strcmp(s->ports[i]->machine, src->machine) == 0)
					inject_to(s, s->ports[i], frame, length);
			if (--src->left == 0u) {
				src->used = 0;
				break;
			}
			src->next += (uint64_t)src->every_ms * 1000000u;
		}
		if (src->used && src->next < first)
			first = src->next;
	}
	return first;
}

static void forward(segment *s, unsigned from, const uint8_t *framed, size_t length);

/* A frame onto `w`'s queue, in due order, unless it holds `limit` already
 * (netem's 1000 where 0): 1, or 0 where it is full or out of memory. */
static int schedule(way *w, const uint8_t *framed, size_t length, uint64_t due, unsigned limit)
{
	held  *h;
	held **into = &w->queue;

	if (w->queued >= (limit != 0u ? limit : QUEUE_FRAMES) || (h = malloc(sizeof *h + length)) == NULL)
		return 0;
	h->due    = due;
	h->length = length;
	memcpy(h->framed, framed, length);
	while (*into != NULL && (*into)->due <= due)
		into = &(*into)->next;
	h->next = *into;
	*into   = h;
	w->queued++;
	return 1;
}

/* A matched frame tampered: its copies re-sent later (replay), then the
 * frame dropped, rewritten or a bit flipped. `work` holds a copy a set or
 * a flip may change; `*frame` points at it where it did. 1 to drop the
 * frame, else 0. */
static int tamper(way *w, uint64_t now, const uint8_t **frame, uint8_t *work, size_t length)
{
	const hull_tamper *t = &w->tamper;
	size_t             payload = 4u + 14u;

	if (t->match_length != 0u) {
		if (length < payload + t->match_offset + t->match_length ||
		    memcmp(*frame + payload + t->match_offset, t->match, t->match_length) != 0)
			return 0;
	}
	w->count.matched++;
	for (unsigned c = 0u; c < t->copies && happens(w, t->replay); c++) {
		if (schedule(w, *frame, length, now + (uint64_t)(c + 1u) * t->every_ms * 1000000u,
		             w->impairment.limit))
			w->count.replayed++;
	}
	if (happens(w, t->drop)) {
		w->count.tampered++;
		return 1;
	}
	if (t->set_length != 0u && happens(w, t->rewrite) &&
	    length >= payload + t->set_offset + t->set_length) {
		memcpy(work, *frame, length);
		memcpy(work + payload + t->set_offset, t->set, t->set_length);
		*frame = work;
		w->count.tampered++;
	} else if (length > payload && happens(w, t->flip)) {
		uint64_t bit = draw(w) % ((length - payload) * 8u);

		if (*frame != work)
			memcpy(work, *frame, length);
		work[payload + bit / 8u] ^= (uint8_t)(1u << (bit % 8u));
		*frame = work;
		w->count.tampered++;
	}
	return 0;
}

/* A frame through `w`, which is port `at` of `s`'s, direction
 * `direction`: dropped, held, or handed on now. */
static void cross(segment *s, unsigned at, int direction, const uint8_t *framed, size_t length)
{
	port                  *p = s->ports[at];
	way                   *w = &p->way[direction];
	const hull_impairment *m = &w->impairment;
	unsigned               copies = 1u;
	uint64_t               now = now_ns();
	uint8_t                work[4u + MAX_FRAME];

	w->count.frames++;
	w->count.bytes += length - 4u;
	/* What a machine sent, before its link does anything to it. */
	if (direction == HULL_SEGMENT_OUT)
		capture_frame(s, p, direction, framed, length);
	/* A targeted fault first: it may drop the frame, or hand on a
	 * rewritten one, and schedule its replays. */
	if (w->tampering && tamper(w, now, &framed, work, length))
		return;
	if (!w->impaired) {
		if (direction == HULL_SEGMENT_OUT) {
			w->count.passed++;
			w->count.passed_bytes += length - 4u;
			forward(s, at, framed, length);
		} else {
			write_port(s, p, framed, length);
		}
		return;
	}
	if (m->down) {
		w->count.lost++;
		return;
	}
	if (m->mtu != 0u && length - 4u > 14u + (size_t)m->mtu) {
		w->count.too_big++;
		return;
	}
	/* As netem's: the frame's fate by the state it finds, then the
	 * state's move. */
	if (m->gemodel) {
		int lost = happens(w, w->bursting ? m->bad_loss : m->good_loss);

		if (happens(w, w->bursting ? m->r : m->p))
			w->bursting = !w->bursting;
		if (lost) {
			w->count.lost++;
			return;
		}
	} else if (happens(w, m->loss)) {
		w->count.lost++;
		return;
	}
	if (happens(w, m->duplicate)) {
		copies = 2u;
		w->count.duplicated++;
	}
	for (unsigned c = 0u; c < copies; c++) {
		uint8_t  copy[4u + MAX_FRAME];
		uint64_t due = now;

		memcpy(copy, framed, length);
		/* A bit of the payload, past the Ethernet header, so that the
		 * frame still reaches its port damaged rather than another. */
		if (length > 4u + 14u && happens(w, m->corrupt)) {
			uint64_t bit = draw(w) % ((length - 4u - 14u) * 8u);

			copy[4u + 14u + bit / 8u] ^= (uint8_t)(1u << (bit % 8u));
			w->count.corrupted++;
		}
		if ((m->delay_ms != 0u || m->jitter_ms != 0u) && !happens(w, m->reorder)) {
			int64_t spread = (int64_t)m->jitter_ms * 1000000;
			int64_t off    = (int64_t)m->delay_ms * 1000000;

			if (spread != 0)
				off += (int64_t)(draw(w) % (uint64_t)(2 * spread + 1)) - spread;
			due += off > 0 ? (uint64_t)off : 0u;
		}
		if (m->rate != 0u) {
			uint64_t takes = (uint64_t)(length - 4u) * 8u * 1000000000u / m->rate;

			if (w->link_free < due)
				w->link_free = due;
			w->link_free += takes;
			due = w->link_free;
		}
		if (due <= now) {
			if (direction == HULL_SEGMENT_OUT) {
				w->count.passed++;
				w->count.passed_bytes += length - 4u;
				forward(s, at, copy, length);
			} else {
				write_port(s, p, copy, length);
			}
			continue;
		}
		if (!schedule(w, copy, length, due, m->limit)) {
			w->count.overflow++;
			if (m->rate != 0u)
				w->link_free -= (uint64_t)(length - 4u) * 8u * 1000000000u / m->rate;
			continue;
		}
	}
}

/* --- hull at .1 ------------------------------------------------------------ */

/* Where a machine of the segment hull addresses asks .1, the switch is
 * the one answering (Appendix D): ARP for .1, and DNS on its port 53, the
 * names those of the ports hull addresses, so a machine that joins later
 * is named as soon as its port is. Its MAC is a locally administered
 * one, "hull" in it, which qemu's prefix is not. */
static const uint8_t hull_mac[6] = { 0x02, 'h', 'u', 'l', 'l', 0x01 };

#define DNS_TTL 5u   /* seconds: a name goes with its port */

static uint16_t get16(const uint8_t *at)
{
	return (uint16_t)(at[0] << 8 | at[1]);
}

static void put16(uint8_t *at, uint32_t value)
{
	at[0] = (uint8_t)(value >> 8);
	at[1] = (uint8_t)value;
}

static void put32(uint8_t *at, uint32_t value)
{
	put16(at, value >> 16);
	put16(at + 2, value);
}

/* The subnet hull addresses `s` in, from any port it addresses: 0 where
 * it addresses none, the segment then bare. */
static int subnet_of(const segment *s, uint32_t *network, unsigned *prefix)
{
	for (unsigned i = 0u; i < s->port_count; i++) {
		const port *p = s->ports[i];

		if (p->prefix != 0u) {
			*prefix  = p->prefix;
			*network = p->address & 0xffffffffu << (32u - p->prefix);
			return 1;
		}
	}
	return 0;
}

/* The segment's IPv6 network, as subnet_of() its IPv4 one. */
static int subnet6_of(const segment *s, uint8_t network6[16], unsigned *prefix6)
{
	for (unsigned i = 0u; i < s->port_count; i++) {
		const port *p = s->ports[i];

		if (p->prefix6 != 0u) {
			*prefix6 = p->prefix6;
			memcpy(network6, p->address6, 16u);
			for (unsigned bit = p->prefix6; bit < 128u; bit++)
				network6[bit / 8u] &= (uint8_t)~(0x80u >> (bit % 8u));
			return 1;
		}
	}
	return 0;
}

static int peer_same(const peer *a, const peer *b)
{
	return a->six == b->six && (a->six ? memcmp(a->v6, b->v6, 16u) == 0 : a->v4 == b->v4);
}

/* .1 of `s` in the family `six` names: its network's .1, or ::1. 0 where
 * the segment has no network of that family. */
static int self_of(const segment *s, int six, peer *self)
{
	uint32_t network;
	unsigned prefix;

	memset(self, 0, sizeof *self);
	self->six = six;
	if (six) {
		if (!subnet6_of(s, self->v6, &prefix))
			return 0;
		self->v6[15] |= 1u;
		return 1;
	}
	if (!subnet_of(s, &network, &prefix))
		return 0;
	self->v4 = network + 1u;
	return 1;
}

/* The pseudo-header UDP, TCP and ICMPv6 sum first, from `source` to
 * `destination`: IPv4's 12 bytes, or IPv6's 40 folded into 12 -- their
 * ones' complement sum in the first two, the rest zero -- as frame.situ
 * says, so that one structure serves both families. */
static void pseudo(uint8_t out[12], uint32_t source, uint32_t destination, uint8_t protocol,
                   uint32_t length);

static void pseudo_any(uint8_t out[12], const peer *source, const peer *destination,
                       uint8_t protocol, uint32_t length)
{
	uint32_t sum = 0u;

	if (!source->six) {
		pseudo(out, source->v4, destination->v4, protocol, length);
		return;
	}
	for (unsigned i = 0u; i < 16u; i += 2u)
		sum += (uint32_t)(source->v6[i] << 8 | source->v6[i + 1u]) +
		       (uint32_t)(destination->v6[i] << 8 | destination->v6[i + 1u]);
	sum += (length >> 16) + (length & 0xffffu) + protocol;
	while (sum >> 16)
		sum = (sum & 0xffffu) + (sum >> 16);
	memset(out, 0, 12u);
	out[0] = (uint8_t)(sum >> 8);
	out[1] = (uint8_t)sum;
}

/* Ethernet's and IP's headers into `out`, a frame of `cap`, around the
 * `payload` bytes of `protocol` already at ip_room() past its Ethernet
 * header, from `self` to `to` at `mac`: the frame's length, padded to
 * Ethernet's least, or 0. IPv4's are whole and not to be fragmented. */
static size_t ip_room(int six)
{
	return six ? 40u : 20u;
}

static size_t ip_frame(uint8_t *out, size_t cap, const uint8_t mac[6], const peer *self,
                       const peer *to, uint8_t protocol, uint32_t payload, uint8_t hops)
{
	static uint16_t ip_id;
	uint32_t        wrote = 0u;
	size_t          total = 14u + ip_room(self->six) + payload;

	if (total > cap ||
	    hull_frame_ethernet_header_build(out, 14u, mac, hull_mac, self->six ? 0x86ddu : 0x0800u,
	                                     &wrote) != SITU_OK)
		return 0u;
	if (self->six) {
		if (hull_frame_ipv6_header_build(out + 14, 40u, 6u, 0u, 0u, 0u, (uint16_t)payload,
		                                 protocol, hops, self->v6, to->v6, &wrote) != SITU_OK)
			return 0u;
	} else if (hull_frame_ipv4_header_build(out + 14, 20u, 4u, 0u, 0u, (uint16_t)(20u + payload),
	                                        ++ip_id, 1u, 0u, 0u, hops, protocol, self->v4, to->v4,
	                                        NULL, 0u, &wrote) != SITU_OK) {
		return 0u;
	}
	return total < 60u ? 60u : total;
}

/* The machine named `name`, or at `address` where `name` is NULL: an
 * addressed port's, or else one a port's plan names, which may not be on
 * the segment yet or any longer. Its id and address, or NULL. */
static const hull_segment_host *named(const segment *s, const char *name, uint32_t address,
                                      hull_segment_host *found)
{
	for (unsigned i = 0u; i < s->port_count; i++) {
		const port *p = s->ports[i];

		if ((name != NULL ? (p->prefix != 0u || p->prefix6 != 0u) &&
		                    strcasecmp(p->machine, name) == 0 :
		                    p->prefix != 0u && p->address == address)) {
			memset(found, 0, sizeof *found);
			snprintf(found->id, sizeof found->id, "%s", p->machine);
			found->address = p->address;
			/* Its IPv6 address, which its plan's own names give it. */
			for (unsigned k = 0u; k < p->host_count; k++)
				if (strcmp(p->hosts[k].id, p->machine) == 0 && p->hosts[k].has6) {
					found->has6 = 1;
					memcpy(found->address6, p->hosts[k].address6, 16u);
				}
			return found;
		}
	}
	for (unsigned i = 0u; i < s->port_count; i++) {
		const port *p = s->ports[i];

		for (unsigned k = 0u; k < p->host_count; k++) {
			const hull_segment_host *h = &p->hosts[k];

			if (name != NULL ? strcasecmp(h->id, name) == 0 : h->address == address) {
				*found = *h;
				return found;
			}
		}
	}
	return NULL;
}

/* The machine `s` names whose IPv6 address is `address6`, as named(). */
static const hull_segment_host *named6(const segment *s, const uint8_t address6[16],
                                       hull_segment_host *found)
{
	for (unsigned i = 0u; i < s->port_count; i++) {
		const port *p = s->ports[i];

		for (unsigned k = 0u; k < p->host_count; k++)
			if (p->hosts[k].has6 && memcmp(p->hosts[k].address6, address6, 16u) == 0) {
				*found = p->hosts[k];
				return found;
			}
	}
	return NULL;
}

#define DNS_A    1u
#define DNS_PTR  12u
#define DNS_AAAA 28u
#define DNS_ANY  255u

#define ASK_ON   SIZE_MAX
#define MAX_DNS  (1514u - 14u - 28u)   /* what one frame carries of an answer */

/* The answer to DNS `query` from the names `s` holds, into `out`: its
 * length, or 0 for none. hull's names are ID, ID.hull and the subnet's
 * reverse names, and anything else under .hull is not there. The rest is
 * ASK_ON where `forwards`, its question's end in `*question`, and refused
 * where not, there being nothing to ask on to. */
static size_t dns_answer(const segment *s, uint32_t network, unsigned prefix,
                         const uint8_t *query, size_t length, int forwards, uint8_t *out,
                         size_t cap, size_t *question)
{
	char        name[256];
	size_t      at = 12u;
	size_t      used = 0u;
	size_t      n;
	unsigned    type;
	unsigned    rcode = 0u;
	uint8_t     data[80];
	uint8_t     address6[16];
	size_t      data_length = 0u;
	unsigned    answer_type = 0u;
	const hull_segment_host *p = NULL;
	hull_segment_host        found;
	uint32_t    mask = prefix == 0u ? 0u : 0xffffffffu << (32u - prefix);

	/* A question, read through core/frame.situ's header: one, of QUERY. */
	{
		situ_msg_t  msg;
		situ_view_t head;

		if (length < 12u)
			return 0u;
		situ_msg_init(&msg, (uint8_t *)(uintptr_t)query, (uint32_t)length);
		if (hull_frame_dns_header_view(&msg, 0u, &head) != SITU_OK ||
		    hull_frame_dns_header_validate(head) != SITU_OK ||
		    hull_frame_dns_header_is_response_get(head) ||
		    hull_frame_dns_header_opcode_get(head) != 0u ||
		    hull_frame_dns_header_question_count_get(head) != 1u)
			return 0u;
	}
	/* The question's name, lowercased, its labels joined by dots; no
	 * pointer, since a question has nothing earlier to point at. */
	while (at < length && query[at] != 0u) {
		unsigned label = query[at];

		if (label > 63u || at + 1u + label >= length || used + label + 1u >= sizeof name)
			return 0u;
		if (used != 0u)
			name[used++] = '.';
		for (unsigned i = 0u; i < label; i++)
			name[used++] = (char)tolower(query[at + 1u + i]);
		at += 1u + label;
	}
	if (at + 5u > length)
		return 0u;
	name[used] = '\0';
	at++;
	type = get16(query + at);
	if (get16(query + at + 2u) != 1u)
		rcode = 5u;   /* not IN */
	at += 4u;
	n = used;
	if (rcode == 0u && n > 13u && strcmp(name + n - 13u, ".in-addr.arpa") == 0) {
		unsigned octet[4];
		char     tail;

		name[n - 13u] = '\0';
		if (sscanf(name, "%u.%u.%u.%u%c", &octet[3], &octet[2], &octet[1], &octet[0], &tail) != 4 ||
		    octet[0] > 255u || octet[1] > 255u || octet[2] > 255u || octet[3] > 255u) {
			rcode = 5u;
		} else {
			uint32_t address = octet[0] << 24 | octet[1] << 16 | octet[2] << 8 | octet[3];

			if (prefix == 0u || (address & mask) != network)
				rcode = 5u;   /* outside, or a segment of IPv6 alone */
			else if ((p = named(s, NULL, address, &found)) == NULL)
				rcode = 3u;
			else if (type == DNS_PTR || type == DNS_ANY) {
				size_t id = strlen(p->id);

				data[0] = (uint8_t)id;
				memcpy(data + 1, p->id, id);
				memcpy(data + 1u + id, "\4hull", 6u);
				data_length = 1u + id + 6u;
				answer_type = DNS_PTR;
			}
		}
	} else if (rcode == 0u && n > 9u && strcmp(name + n - 9u, ".ip6.arpa") == 0) {
		/* 32 nibbles, the last first: a machine's IPv6 address answered,
		 * any other asked on, as an address outside the subnet is, since
		 * the segment's IPv6 network is its plans' and not the switch's. */
		int ok = n - 9u == 63u;

		for (unsigned k = 0u; ok && k < 32u; k++) {
			char c = name[2u * k];
			int  v = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;

			ok = v >= 0 && (k == 31u || name[2u * k + 1u] == '.');
			if (ok && k % 2u == 0u)
				address6[15u - k / 2u] = (uint8_t)v;
			else if (ok)
				address6[15u - k / 2u] |= (uint8_t)(v << 4);
		}
		if (!ok || (p = named6(s, address6, &found)) == NULL)
			rcode = 5u;
		else if (type == DNS_PTR || type == DNS_ANY) {
			size_t id = strlen(p->id);

			data[0] = (uint8_t)id;
			memcpy(data + 1, p->id, id);
			memcpy(data + 1u + id, "\4hull", 6u);
			data_length = 1u + id + 6u;
			answer_type = DNS_PTR;
		}
	} else if (rcode == 0u) {
		int ours = n > 5u && strcmp(name + n - 5u, ".hull") == 0;

		if (ours)
			name[n - 5u] = '\0';
		if (ours && strchr(name, '.') != NULL)
			rcode = 3u;
		else if (strchr(name, '.') != NULL || name[0] == '\0')
			rcode = 5u;
		else if ((p = named(s, name, 0u, &found)) == NULL)
			rcode = ours || !forwards ? 3u : 5u;
		else if ((type == DNS_A || type == DNS_ANY) && p->address != 0u) {
			/* None for one on a segment of IPv6 alone. */
			put32(data, p->address);
			data_length = 4u;
			answer_type = DNS_A;
		} else if (type == DNS_AAAA && p->has6) {
			memcpy(data, p->address6, 16u);
			data_length = 16u;
			answer_type = DNS_AAAA;
		}
	}
	*question = at;
	if (rcode == 5u && forwards)
		return ASK_ON;
	if (at + 16u + data_length > cap)
		return 0u;
	/* The query's id and question back, as an authority's answer, the
	 * recursion it asked for echoed and none offered. */
	memcpy(out, query, at);
	out[2] = (uint8_t)(0x84u | (query[2] & 0x01u));
	out[3] = (uint8_t)rcode;
	put16(out + 6, data_length != 0u ? 1u : 0u);
	put16(out + 8, 0u);
	put16(out + 10, 0u);
	if (data_length == 0u)
		return at;
	put16(out + at, 0xc00cu);   /* the question's name */
	put16(out + at + 2u, answer_type);
	put16(out + at + 4u, 1u);
	put32(out + at + 6u, DNS_TTL);
	put16(out + at + 10u, (uint32_t)data_length);
	memcpy(out + at + 12u, data, data_length);
	return at + 12u + data_length;
}

/* DNS `dns` from hull at .1, `self`, to `asker` at `mac` and its UDP
 * `udp_port`, sent back through `s`'s port `to`'s way in, so the port's
 * impairment holds for it too. One past a frame goes as IP fragments,
 * which the asker's kernel puts back together: so an answer up to the
 * asker's EDNS size arrives whole, with no TCP at .1 to ask again on. */
/* The 12 bytes the UDP and TCP sums are taken over first. */
static void pseudo(uint8_t out[12], uint32_t source, uint32_t destination, uint8_t protocol,
                   uint32_t length)
{
	uint32_t wrote = 0u;

	if (hull_frame_pseudo_header_build(out, 12u, source, destination, 0u, protocol,
	                                   (uint16_t)length, &wrote) != SITU_OK)
		memset(out, 0, 12u);
}

/* Where `dns`'s question ends, past its header and one name: 0 where it
 * has none whole. */
static size_t question_end(const uint8_t *dns, size_t length)
{
	size_t at = 12u;

	while (at < length && dns[at] != 0u) {
		if (dns[at] > 63u)
			return 0u;
		at += 1u + dns[at];
	}
	return at + 5u <= length ? at + 5u : 0u;
}

/* The DNS answer `dns` to `asker` at `mac`, from .1 of its family, to its
 * UDP port. Over IPv4 one past a frame goes in fragments; over IPv6, which
 * a sender alone fragments and .1 does not, it goes as its question with
 * TC set, and the asker asks again over TCP. */
static void send_answer(segment *s, unsigned to, const uint8_t mac[6], const peer *asker,
                        const uint8_t udp_port[2], const uint8_t *dns, size_t length)
{
	static uint8_t  datagram[8u + 65535u];
	static uint16_t ip_id;
	uint8_t         reply[4u + 1514u];
	uint8_t        *out = reply + 4;
	uint8_t         first[12];
	uint8_t         cut[512];
	uint32_t        whole = 0u;
	uint32_t        wrote = 0u;
	peer            self;

	if (length > 65535u - 20u - 8u || !self_of(s, asker->six, &self))
		return;
	if (asker->six) {
		size_t total;

		if (8u + length > 1500u - 40u) {
			size_t question = question_end(dns, length);

			if (question == 0u || question > sizeof cut)
				return;
			memcpy(cut, dns, question);
			cut[2] |= 0x02u;   /* TC */
			memset(cut + 6, 0, 6u);
			dns = cut;
			length = question;
		}
		memset(reply, 0, sizeof reply);
		pseudo_any(first, &self, asker, 17u, 8u + (uint32_t)length);
		if (hull_frame_udp_datagram_build(out + 54, 1514u - 54u, 53u, (uint16_t)get16(udp_port),
		                                  first, sizeof first, dns, (uint32_t)length,
		                                  &whole) != SITU_OK ||
		    (total = ip_frame(out, 1514u, mac, &self, asker, 17u, whole, 64u)) == 0u)
			return;
		put32(reply, (uint32_t)total);
		cross(s, to, HULL_SEGMENT_IN, reply, 4u + total);
		return;
	}
	/* The UDP sum over its pseudo-header and the whole datagram, as a
	 * checking stack wants, before any of it is cut into fragments. */
	pseudo(first, self.v4, asker->v4, 17u, 8u + (uint32_t)length);
	if (hull_frame_udp_datagram_build(datagram, sizeof datagram, 53u, (uint16_t)get16(udp_port),
	                                  first, sizeof first, dns, (uint32_t)length,
	                                  &whole) != SITU_OK)
		return;
	ip_id++;
	for (uint32_t at = 0u; at < whole;) {
		uint32_t piece = whole - at;
		int      more  = 0;
		size_t   total;

		if (piece > 1480u) {
			piece = 1480u;   /* a multiple of 8, as every fragment but the last is */
			more  = 1;
		}
		memset(reply, 0, sizeof reply);
		/* Don't fragment where it is whole; else more-fragments and the
		 * offset, in 8-byte units. */
		if (hull_frame_ethernet_header_build(out, 14u, mac, hull_mac, 0x0800u, &wrote) != SITU_OK ||
		    hull_frame_ipv4_header_build(out + 14, 20u, 4u, 0u, 0u, (uint16_t)(20u + piece), ip_id,
		                                 at == 0u && !more, (uint8_t)more, (uint16_t)(at / 8u),
		                                 64u, 17u, self.v4, asker->v4, NULL, 0u, &wrote) != SITU_OK)
			return;
		memcpy(out + 34, datagram + at, piece);
		total = 14u + 20u + piece;
		if (total < 60u)
			total = 60u;
		put32(reply, (uint32_t)total);
		cross(s, to, HULL_SEGMENT_IN, reply, 4u + total);
		at += piece;
	}
}

/* .1's echo reply to `asker`'s request, ICMP's or ICMPv6's, in one frame
 * as the request came: its identifier, sequence and data as they were. */
static void send_echo(segment *s, unsigned to, const uint8_t mac[6], const peer *asker,
                      uint16_t identifier, uint16_t sequence, const uint8_t *data,
                      uint32_t length)
{
	uint8_t  reply[4u + 1514u];
	uint8_t *out = reply + 4;
	uint8_t  first[12];
	uint32_t wrote = 0u;
	size_t   room = ip_room(asker->six);
	size_t   total;
	peer     self;

	if (8u + length > 1500u - room || !self_of(s, asker->six, &self))
		return;
	memset(reply, 0, sizeof reply);
	if (asker->six) {
		pseudo_any(first, &self, asker, 58u, 8u + length);
		if (hull_frame_icmp6_echo_build(out + 14 + room, 8u + length, 129u, 0u, first,
		                                sizeof first, identifier, sequence, data, length,
		                                &wrote) != SITU_OK)
			return;
	} else if (hull_frame_icmp_echo_build(out + 14 + room, 8u + length, 0u, 0u, identifier,
	                                      sequence, data, length, &wrote) != SITU_OK) {
		return;
	}
	if ((total = ip_frame(out, 1514u, mac, &self, asker, asker->six ? 58u : 1u, 8u + length,
	                      64u)) == 0u)
		return;
	put32(reply, (uint32_t)total);
	cross(s, to, HULL_SEGMENT_IN, reply, 4u + total);
}

/* hull's link-local address, from its MAC as EUI-64 makes one: what its
 * router advertisements come from, as RFC 4861 4.2 asks of a router. */
static const uint8_t hull_link_local[16] = { 0xfe, 0x80, 0, 0, 0, 0, 0, 0,
                                             0x00, 'h', 'u', 0xff, 0xfe, 'l', 'l', 0x01 };

/* Whether hull advertises `s`'s IPv6 network: where any machine's plan
 * asks it, one advertiser on the segment whoever does. */
static int advertises(const segment *s)
{
	for (unsigned i = 0u; i < s->port_count; i++)
		if (s->ports[i]->advertise)
			return 1;
	return 0;
}

/* .1's neighbour advertisement for `target`, ::1 or hull's link-local
 * address, to `asker` at `mac`, as ARP's reply is for .1: solicited,
 * overriding, hull's MAC its option, from a hop limit of 255 as RFC 4861
 * asks. */
static void send_advert(segment *s, unsigned to, const uint8_t mac[6], const peer *asker,
                        const uint8_t target[16])
{
	uint8_t  reply[4u + 1514u];
	uint8_t *out = reply + 4;
	uint8_t  first[12];
	uint8_t  option[8] = { 2u, 1u };   /* target link-layer address, 8 bytes */
	uint32_t wrote = 0u;
	size_t   total;
	peer     self;

	memset(&self, 0, sizeof self);
	self.six = 1;
	memcpy(self.v6, target, 16u);
	memcpy(option + 2, hull_mac, 6u);
	memset(reply, 0, sizeof reply);
	pseudo_any(first, &self, asker, 58u, 32u);
	if (hull_frame_neighbor_advertisement_build(out + 54, 32u, 136u, 0u, first, sizeof first, 0u,
	                                            1u, 1u, target, option, sizeof option,
	                                            &wrote) != SITU_OK ||
	    (total = ip_frame(out, 1514u, mac, &self, asker, 58u, 32u, 255u)) == 0u)
		return;
	put32(reply, (uint32_t)total);
	cross(s, to, HULL_SEGMENT_IN, reply, 4u + total);
}

/* hull's router advertisement of `s`'s IPv6 network to `asker` at `mac`,
 * or to every node where it solicited from ::, from hull's link-local
 * address: a router lifetime of 0, a gateway's route being set
 * outright; the prefix, on-link and for SLAAC; ::1 as its DNS server
 * (RDNSS, RFC 8106) and
 * `hull` to search (DNSSL). The lifetimes are for ever, as the network's
 * is the segment's while it is there, so nothing is sent unasked. */
static void send_ra(segment *s, unsigned to, const uint8_t mac[6], const peer *asker)
{
	static const uint8_t all_nodes[16] = { 0xff, 0x02, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1 };
	static const uint8_t all_nodes_mac[6] = { 0x33, 0x33, 0, 0, 0, 1 };
	uint8_t              reply[4u + 1514u];
	uint8_t             *out = reply + 4;
	uint8_t              first[12];
	uint8_t              options[8u + 32u + 24u + 16u];
	uint8_t             *o = options;
	uint32_t             wrote = 0u;
	size_t               total;
	peer                 self = { .six = 1 };
	peer                 dns;
	peer                 to_peer = *asker;
	unsigned             prefix6;
	uint8_t              network6[16];

	if (!subnet6_of(s, network6, &prefix6) || !self_of(s, 1, &dns))
		return;
	memcpy(self.v6, hull_link_local, 16u);
	if (IN6_IS_ADDR_UNSPECIFIED((const struct in6_addr *)(const void *)asker->v6)) {
		memcpy(to_peer.v6, all_nodes, 16u);
		mac = all_nodes_mac;
	}
	memset(options, 0, sizeof options);
	/* Source link-layer address: hull's MAC. */
	o[0] = 1u;
	o[1] = 1u;
	memcpy(o + 2, hull_mac, 6u);
	o += 8;
	/* Prefix information: the network, on-link and autonomous. */
	o[0] = 3u;
	o[1] = 4u;
	o[2] = (uint8_t)prefix6;
	o[3] = 0xc0u;
	memset(o + 4, 0xff, 8u);
	memcpy(o + 16, network6, 16u);
	o += 32;
	/* RDNSS: ::1, hull's DNS. */
	o[0] = 25u;
	o[1] = 3u;
	memset(o + 4, 0xff, 4u);
	memcpy(o + 8, dns.v6, 16u);
	o += 24;
	/* DNSSL: hull, padded to eight. */
	o[0] = 31u;
	o[1] = 2u;
	memset(o + 4, 0xff, 4u);
	memcpy(o + 8, "\4hull", 6u);
	memset(reply, 0, sizeof reply);
	pseudo_any(first, &self, &to_peer, 58u, 16u + sizeof options);
	if (hull_frame_router_advertisement_build(out + 54, 16u + sizeof options, 134u, 0u, first,
	                                          sizeof first, 64u, 0u, 0u, 0u, 0u, 0u, options,
	                                          sizeof options, &wrote) != SITU_OK ||
	    (total = ip_frame(out, 1514u, mac, &self, &to_peer, 58u, 16u + sizeof options, 255u)) == 0u)
		return;
	put32(reply, (uint32_t)total);
	cross(s, to, HULL_SEGMENT_IN, reply, 4u + total);
}

static unsigned resolvers(struct sockaddr_in6 out[3]);
static void open_upstream(void);
static segment *find(const char *key);
static void conn_arm(conn *c);
static void ask_on(segment *s, unsigned from, const uint8_t mac[6], const peer *asker,
                   const uint8_t udp_port[2], const uint8_t *query, size_t length,
                   const struct sockaddr_in6 *server, int on);

/* `s`'s port that `c` came on, or -1 where it has gone. */
static int conn_port(const segment *s, const conn *c)
{
	for (unsigned i = 0u; i < s->port_count; i++)
		if (s->ports[i]->fd == c->fd && strcmp(s->ports[i]->machine, c->machine) == 0)
			return (int)i;
	return -1;
}

/* One TCP segment from .1 to `c`'s asker: `flags`, at `seq`, with `data`. */
static void tcp_send(segment *s, const conn *c, unsigned flags, uint32_t seq,
                     const uint8_t *data, size_t length)
{
	static const uint8_t mss[4] = { 2u, 4u, TCP_MSS >> 8, TCP_MSS & 0xffu };
	uint8_t              reply[4u + 1514u];
	uint8_t             *out = reply + 4;
	uint8_t              first[12];
	int                  syn = (flags & TCP_SYN) != 0u;
	uint32_t             segment_length = 20u + (syn ? 4u : 0u) + (uint32_t)length;
	uint32_t             window = CONN_IN - c->in_length > 0xffffu ? 0xffffu :
	                              (uint32_t)(CONN_IN - c->in_length);
	peer                 self;
	size_t               room = ip_room(c->asker.six);
	uint32_t             tcp = 0u;
	size_t               total;
	int                  to = conn_port(s, c);

	if (to < 0 || length > TCP_MSS || !self_of(s, c->asker.six, &self))
		return;
	memset(reply, 0, sizeof reply);
	pseudo_any(first, &self, &c->asker, 6u, segment_length);
	if (hull_frame_tcp_segment_build(out + 14 + room, (uint32_t)(1514u - 14u - room), 53u,
	                                 c->their_port, seq,
	                                 (flags & TCP_ACK) != 0u ? c->rcv_nxt : 0u, 0u, 0u, 0u,
	                                 (flags & TCP_ACK) != 0u, (flags & TCP_PSH) != 0u,
	                                 (flags & TCP_RST) != 0u, (uint8_t)syn,
	                                 (flags & TCP_FIN) != 0u, (uint16_t)window, first,
	                                 sizeof first, 0u, mss, syn ? sizeof mss : 0u, data,
	                                 (uint32_t)length, &tcp) != SITU_OK ||
	    (total = ip_frame(out, 1514u, c->mac, &self, &c->asker, 6u, tcp, 64u)) == 0u)
		return;
	put32(reply, (uint32_t)total);
	cross(s, (unsigned)to, HULL_SEGMENT_IN, reply, 4u + total);
}

/* What of `c`'s answers it has not yet been sent, sent, in segments. */
static void conn_flush(segment *s, conn *c)
{
	size_t sent = (size_t)(c->snd_nxt - c->out_seq);

	while (sent < c->out_length) {
		size_t piece = c->out_length - sent;

		if (piece > TCP_MSS)
			piece = TCP_MSS;
		tcp_send(s, c, TCP_ACK | TCP_PSH, c->snd_nxt, c->out + sent, piece);
		c->snd_nxt += (uint32_t)piece;
		sent += piece;
	}
}

/* Everything of `c`'s not yet acknowledged, sent again: its asker has
 * shown that it lacks some of it. */
static void conn_again(segment *s, conn *c)
{
	if (!c->syn_acked) {
		tcp_send(s, c, TCP_SYN | TCP_ACK, c->out_seq - 1u, NULL, 0u);
		return;
	}
	c->snd_nxt = c->out_seq;
	conn_flush(s, c);
	if (c->fin_sent)
		tcp_send(s, c, TCP_FIN | TCP_ACK, c->snd_nxt++, NULL, 0u);
}

/* An answer for `c`, after its length, sent. Dropped where it would not
 * fit what waits unacknowledged, as a full queue drops a frame. */
static void conn_answer(segment *s, conn *c, const uint8_t *dns, size_t length)
{
	uint32_t unsent = c->snd_nxt - c->out_seq;

	if (c->fin_sent || length > 0xffffu || c->out_length + 2u + length > sizeof c->out ||
	    unsent != c->out_length)
		return;
	put16(c->out + c->out_length, (uint32_t)length);
	memcpy(c->out + c->out_length + 2u, dns, length);
	c->out_length += 2u + length;
	conn_flush(s, c);
	conn_arm(c);
}

static void conn_drop(int i)
{
	free(conns[i]);
	conns[i] = NULL;
}

/* `c`'s resend time: none where the asker has all of .1's, and set where
 * something is unacknowledged and none is, so that what an impaired link
 * loses goes again whether or not the asker sends anything. */
static void conn_arm(conn *c)
{
	if (c->syn_acked && c->out_length == 0u && !c->fin_sent) {
		c->resend_at = 0u;
		c->backoff   = 0u;
	} else if (c->resend_at == 0u) {
		c->resend_at = now_ns() + ((uint64_t)CONN_RTO_NS << c->backoff);
	}
}

/* Each connection whose resend time has come sent again, or dropped
 * after CONN_TRIES; when the next is due, UINT64_MAX for none. */
static uint64_t conn_timers(uint64_t now)
{
	uint64_t first = UINT64_MAX;

	for (unsigned i = 0u; i < MAX_CONNS; i++) {
		conn    *c = conns[i];
		segment *s;

		if (c == NULL || c->resend_at == 0u)
			continue;
		if (c->resend_at <= now) {
			if (c->backoff + 1u >= CONN_TRIES || (s = find(c->key)) == NULL) {
				conn_drop((int)i);
				continue;
			}
			conn_again(s, c);
			c->backoff++;
			c->resend_at = now + ((uint64_t)CONN_RTO_NS << c->backoff);
		}
		if (c->resend_at < first)
			first = c->resend_at;
	}
	return first;
}

/* A TCP segment from port `from` for .1's port 53: a connection opened,
 * fed, acknowledged or closed, and each whole query on it answered, by
 * hull or asked on as a datagram's is. */
static void tcp_in(segment *s, unsigned from, const uint8_t mac[6], const peer *asker,
                   situ_view_t tcp, uint32_t network, unsigned prefix)
{
	uint16_t       their_port = hull_frame_tcp_segment_source_port_get(tcp);
	uint32_t       seq = hull_frame_tcp_segment_sequence_get(tcp);
	uint32_t       ack = hull_frame_tcp_segment_acknowledgement_get(tcp);
	unsigned       flags = (hull_frame_tcp_segment_fin_get(tcp) ? TCP_FIN : 0u) |
	                       (hull_frame_tcp_segment_syn_get(tcp) ? TCP_SYN : 0u) |
	                       (hull_frame_tcp_segment_reset_get(tcp) ? TCP_RST : 0u) |
	                       (hull_frame_tcp_segment_push_get(tcp) ? TCP_PSH : 0u) |
	                       (hull_frame_tcp_segment_ack_get(tcp) ? TCP_ACK : 0u);
	const uint8_t *data = hull_frame_tcp_segment_payload_ptr(tcp);
	size_t         length = hull_frame_tcp_segment_payload_len(tcp);
	uint64_t       now = now_ns();
	conn          *c = NULL;
	int            at = -1;

	for (unsigned i = 0u; i < MAX_CONNS; i++) {
		if (conns[i] != NULL && now - conns[i]->at > MAX_CONN_NS)
			conn_drop((int)i);
		if (c == NULL && conns[i] != NULL && conns[i]->their_port == their_port &&
		    peer_same(&conns[i]->asker, asker) && conns[i]->fd == s->ports[from]->fd &&
		    strcmp(conns[i]->key, s->key) == 0 &&
		    strcmp(conns[i]->machine, s->ports[from]->machine) == 0) {
			c = conns[i];
			at = (int)i;
		}
	}
	if ((flags & TCP_RST) != 0u) {
		if (c != NULL)
			conn_drop(at);
		return;
	}
	if (c == NULL || ((flags & TCP_SYN) != 0u && (flags & TCP_ACK) == 0u && seq + 1u != c->rcv_nxt)) {
		conn refused;

		if (c != NULL) {
			conn_drop(at);
			c = NULL;
		}
		if ((flags & TCP_SYN) != 0u && (flags & TCP_ACK) == 0u)
			for (unsigned i = 0u; i < MAX_CONNS && at < 0; i++)
				if (conns[i] == NULL && (conns[i] = calloc(1u, sizeof *conns[i])) != NULL)
					at = (int)i;
		if (at < 0 || conns[at] == NULL) {
			/* Not one .1 has, or no room: refused, as a host refuses. */
			memset(&refused, 0, sizeof refused);
			snprintf(refused.machine, sizeof refused.machine, "%s", s->ports[from]->machine);
			refused.fd = s->ports[from]->fd;
			memcpy(refused.mac, mac, 6u);
			refused.asker = *asker;
			refused.their_port = their_port;
			refused.rcv_nxt = seq + (uint32_t)length + ((flags & (TCP_SYN | TCP_FIN)) != 0u);
			if ((flags & TCP_ACK) != 0u)
				tcp_send(s, &refused, TCP_RST, ack, NULL, 0u);
			else
				tcp_send(s, &refused, TCP_RST | TCP_ACK, 0u, NULL, 0u);
			return;
		}
		c = conns[at];
		c->used = 1;
		c->generation = ++conn_generation;
		snprintf(c->key, sizeof c->key, "%s", s->key);
		snprintf(c->machine, sizeof c->machine, "%s", s->ports[from]->machine);
		c->fd = s->ports[from]->fd;
		memcpy(c->mac, mac, 6u);
		c->asker = *asker;
		c->their_port = their_port;
		c->rcv_nxt = seq + 1u;
		c->out_seq = (uint32_t)splitmix(&asking_random) + 1u;
		c->snd_nxt = c->out_seq;
	}
	c->at = now;
	if ((flags & TCP_SYN) != 0u) {
		/* Its SYN, first or again: .1's SYN back, with its MSS. */
		tcp_send(s, c, TCP_SYN | TCP_ACK, c->out_seq - 1u, NULL, 0u);
		conn_arm(c);
		return;
	}
	if ((flags & TCP_ACK) == 0u)
		return;
	/* What it has of .1's: the SYN, then answers, then the FIN. */
	if (!c->syn_acked && ack == c->out_seq) {
		c->syn_acked = 1;
		c->resend_at = 0u;
		c->backoff   = 0u;
	}
	if (c->syn_acked) {
		uint32_t through = ack - c->out_seq;

		if (through <= c->out_length) {
			memmove(c->out, c->out + through, c->out_length - through);
			c->out_length -= through;
			c->out_seq = ack;
			if (through != 0u) {
				/* Progress: the wait starts again from its first. */
				c->resend_at = 0u;
				c->backoff   = 0u;
			}
		} else if (c->fin_sent && through == c->out_length + 1u) {
			conn_drop(at);     /* .1's FIN acknowledged: closed */
			return;
		}
	}
	if (seq != c->rcv_nxt) {
		/* Something it sent before, again: what .1 has, said again, and
		 * what it has sent and the asker may lack, sent again. */
		tcp_send(s, c, TCP_ACK, c->snd_nxt, NULL, 0u);
		conn_again(s, c);
		conn_arm(c);
		return;
	}
	if (length != 0u) {
		if (c->in_length + length > sizeof c->in) {
			tcp_send(s, c, TCP_RST | TCP_ACK, c->snd_nxt, NULL, 0u);
			conn_drop(at);
			return;
		}
		memcpy(c->in + c->in_length, data, length);
		c->in_length += length;
		c->rcv_nxt += (uint32_t)length;
	}
	if ((flags & TCP_FIN) != 0u)
		c->rcv_nxt++;
	/* Each whole query, answered or asked on. */
	while (c->in_length >= 2u && c->in_length >= 2u + get16(c->in)) {
		size_t             query = get16(c->in);
		uint8_t            dns_out[MAX_DNS];
		size_t             question = 0u;
		size_t             dns;
		struct sockaddr_in6 servers[3];
		unsigned           count = 0u;

		if (s->ports[from]->forwards && (count = resolvers(servers)) != 0u && upstream < 0) {
			open_upstream();
			if (upstream < 0)
				count = 0u;
			else
				(void)!write(wake[1], "", 1u);
		}
		dns = dns_answer(s, network, prefix, c->in + 2u, query, count != 0u, dns_out,
		                 sizeof dns_out, &question);
		if (dns == ASK_ON && query <= MAX_DNS) {
			uint8_t their[2] = { (uint8_t)(c->their_port >> 8), (uint8_t)c->their_port };

			ask_on(s, from, mac, asker, their, c->in + 2u, query,
			       &servers[asking_next++ % count], at);
		} else if (dns != 0u && dns != ASK_ON) {
			conn_answer(s, c, dns_out, dns);
		}
		memmove(c->in, c->in + 2u + query, c->in_length - 2u - query);
		c->in_length -= 2u + query;
	}
	if ((flags & TCP_FIN) != 0u && !c->fin_sent) {
		/* Its end: .1's answers go first, then its own FIN. */
		c->fin_sent = 1;
		tcp_send(s, c, TCP_FIN | TCP_ACK, c->snd_nxt++, NULL, 0u);
	} else if (length != 0u || (flags & TCP_FIN) != 0u) {
		tcp_send(s, c, TCP_ACK, c->snd_nxt, NULL, 0u);
	}
	conn_arm(c);
}

/* The host's resolvers, IPv4, at most three: those a test gave, else
 * /etc/resolv.conf's, read each time, since the host's can change under
 * a long-lived core -- its loopback ones too, the core being the host's. */
/* A nameserver as resolv.conf names one, IPv4 or IPv6 with a scope after
 * `%` where it is link-local, into `out` as IPv6, an IPv4 one mapped, so
 * that one socket asks both and one comparison knows an answer's sender.
 * 1, or 0 where it is neither. */
static int resolver_address(const char *text, unsigned port, struct sockaddr_in6 *out)
{
	char           address[64];
	char          *scope;
	struct in_addr v4;

	memset(out, 0, sizeof *out);
	out->sin6_family = AF_INET6;
	out->sin6_port   = htons((uint16_t)port);
	snprintf(address, sizeof address, "%s", text);
	if ((scope = strchr(address, '%')) != NULL)
		*scope++ = '\0';
	if (inet_pton(AF_INET, address, &v4) == 1) {
		out->sin6_addr.s6_addr[10] = 0xffu;
		out->sin6_addr.s6_addr[11] = 0xffu;
		memcpy(&out->sin6_addr.s6_addr[12], &v4, 4u);
		return 1;
	}
	if (inet_pton(AF_INET6, address, &out->sin6_addr) != 1)
		return 0;
	if (scope != NULL && (out->sin6_scope_id = if_nametoindex(scope)) == 0u)
		out->sin6_scope_id = (uint32_t)strtoul(scope, NULL, 10);
	return 1;
}

/* The socket .1 asks on: IPv6 taking IPv4 too, or IPv4 alone where the
 * host has no IPv6, which then asks only its IPv4 resolvers. */
static int upstream_v6;

static void open_upstream(void)
{
	int off = 0;

	upstream = socket(AF_INET6, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
	upstream_v6 = upstream >= 0 &&
	              setsockopt(upstream, IPPROTO_IPV6, IPV6_V6ONLY, &off, sizeof off) == 0;
	if (upstream >= 0 && !upstream_v6) {
		close(upstream);
		upstream = -1;
	}
	if (upstream < 0)
		upstream = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
}

/* `server` sent to on the upstream socket, as its family takes it. */
static ssize_t send_upstream(const uint8_t *data, size_t length, const struct sockaddr_in6 *server)
{
	struct sockaddr_in v4 = { .sin_family = AF_INET };

	if (upstream_v6)
		return sendto(upstream, data, length, MSG_DONTWAIT, (const struct sockaddr *)server,
		              sizeof *server);
	if (!IN6_IS_ADDR_V4MAPPED(&server->sin6_addr)) {
		errno = EAFNOSUPPORT;
		return -1;
	}
	v4.sin_port = server->sin6_port;
	memcpy(&v4.sin_addr, &server->sin6_addr.s6_addr[12], 4u);
	return sendto(upstream, data, length, MSG_DONTWAIT, (const struct sockaddr *)&v4, sizeof v4);
}

static unsigned resolvers(struct sockaddr_in6 out[3])
{
	FILE    *conf;
	char     line[256];
	unsigned count = 0u;

	if (resolvers_given_count != 0u) {
		memcpy(out, resolvers_given, sizeof resolvers_given);
		return resolvers_given_count;
	}
	if ((conf = fopen("/etc/resolv.conf", "re")) == NULL)
		return 0u;
	while (count < 3u && fgets(line, sizeof line, conf) != NULL) {
		char server[64];

		if (sscanf(line, "nameserver %63s", server) == 1 &&
		    resolver_address(server, 53u, &out[count]))
			count++;
	}
	fclose(conf);
	return count;
}

/* `query`, which hull does not hold, asked on to a host resolver for the
 * machine behind `s`'s port `from`, its answer to go back as `answered`
 * finds it. A question with nowhere to wait is dropped, as a full queue
 * drops a frame, and its asker asks again. */
static void ask_on(segment *s, unsigned from, const uint8_t mac[6], const peer *asker,
                   const uint8_t udp_port[2], const uint8_t *query, size_t length,
                   const struct sockaddr_in6 *server, int on)
{
	uint64_t now = now_ns();
	asked   *a = NULL;
	uint8_t  sent[MAX_DNS];
	uint16_t id;
	int      taken;

	if (length > sizeof sent)
		return;
	for (unsigned i = 0u; i < MAX_ASKED && a == NULL; i++)
		if (!asking[i].used || now - asking[i].at > MAX_ASKED_NS)
			a = &asking[i];
	if (a == NULL)
		return;
	/* An id no question still waiting has, drawn, so an answer cannot be
	 * guessed from the last. */
	if (asking_random == 0u)
		asking_random = now ^ (uint64_t)getpid() << 32;
	do {
		id = (uint16_t)splitmix(&asking_random);
		taken = 0;
		for (unsigned i = 0u; i < MAX_ASKED && !taken; i++)
			taken = asking[i].used && &asking[i] != a && asking[i].id == id &&
			        now - asking[i].at <= MAX_ASKED_NS;
	} while (taken);
	memcpy(sent, query, length);
	put16(sent, id);
	if (send_upstream(sent, length, server) != (ssize_t)length)
		return;
	memset(a, 0, sizeof *a);
	a->used     = 1;
	a->at       = now;
	a->id       = id;
	a->their_id = get16(query);
	a->server   = *server;
	snprintf(a->key, sizeof a->key, "%s", s->key);
	snprintf(a->machine, sizeof a->machine, "%s", s->ports[from]->machine);
	a->fd       = s->ports[from]->fd;
	memcpy(a->mac, mac, 6u);
	a->asker    = *asker;
	memcpy(a->udp_port, udp_port, 2u);
	a->conn     = on;
	a->conn_generation = on >= 0 ? conns[on]->generation : 0u;
}

/* What the host's resolvers answered, each sent back to the machine that
 * asked, under its own id, where its port is still there, in fragments
 * where it is longer than a frame. */
static void answered(void)
{
	static uint8_t reply[65536];

	for (;;) {
		struct sockaddr_storage from;
		struct sockaddr_in6     sender = { .sin6_family = AF_INET6 };
		socklen_t               from_length = sizeof from;
		ssize_t                 n = recvfrom(upstream, reply, sizeof reply, MSG_DONTWAIT,
		                                     (struct sockaddr *)&from, &from_length);
		uint64_t                now = now_ns();
		asked                  *a = NULL;
		segment                *s;
		size_t                  length;

		if (n < 0)
			break;
		if (n < 12)
			continue;
		/* Who sent it, as the question's server is held: IPv6, an IPv4
		 * sender mapped. */
		if (from.ss_family == AF_INET6) {
			sender = *(const struct sockaddr_in6 *)&from;
		} else if (from.ss_family == AF_INET) {
			const struct sockaddr_in *v4 = (const struct sockaddr_in *)&from;

			sender.sin6_addr.s6_addr[10] = 0xffu;
			sender.sin6_addr.s6_addr[11] = 0xffu;
			memcpy(&sender.sin6_addr.s6_addr[12], &v4->sin_addr, 4u);
			sender.sin6_port = v4->sin_port;
		} else {
			continue;
		}
		for (unsigned i = 0u; i < MAX_ASKED && a == NULL; i++)
			if (asking[i].used && asking[i].id == get16(reply) &&
			    now - asking[i].at <= MAX_ASKED_NS &&
			    memcmp(&asking[i].server.sin6_addr, &sender.sin6_addr,
			           sizeof sender.sin6_addr) == 0 &&
			    asking[i].server.sin6_port == sender.sin6_port)
				a = &asking[i];
		if (a == NULL)
			continue;
		a->used = 0;
		if ((s = find(a->key)) == NULL)
			continue;
		length = (size_t)n;
		put16(reply, a->their_id);
		if (a->conn >= 0) {
			if (conns[a->conn] != NULL && conns[a->conn]->used &&
			    conns[a->conn]->generation == a->conn_generation)
				conn_answer(s, conns[a->conn], reply, length);
			continue;
		}
		for (unsigned i = 0u; i < s->port_count; i++) {
			if (s->ports[i]->fd == a->fd && strcmp(s->ports[i]->machine, a->machine) == 0) {
				send_answer(s, i, a->mac, &a->asker, a->udp_port, reply, length);
				break;
			}
		}
	}
}

/* A frame from port `from` that is for hull at .1, answered back through
 * the port's way in: 1 where it was, and goes no further. */
static int answer(segment *s, unsigned from, const uint8_t *framed, size_t length)
{
	const uint8_t *frame = framed + 4;
	size_t         size = length - 4u;
	uint8_t        reply[4u + 1514u];
	uint8_t       *out = reply + 4;
	uint32_t       network = 0u;
	unsigned       prefix = 0u;
	peer           self4;
	peer           self6;
	int            have4 = self_of(s, 0, &self4);
	int            have6 = self_of(s, 1, &self6);
	situ_msg_t     msg;
	situ_view_t    ether;
	situ_view_t    view;
	uint16_t       type;
	const uint8_t *mac;

	if ((!have4 && !have6) || size < 14u)
		return 0;
	if (have4)
		subnet_of(s, &network, &prefix);
	/* What the machine sent, read through core/frame.situ: nothing writes
	 * through these views, so the frame's constness holds. */
	situ_msg_init(&msg, (uint8_t *)(uintptr_t)frame, (uint32_t)size);
	if (hull_frame_ethernet_header_view(&msg, 0u, &ether) != SITU_OK)
		return 0;
	type = hull_frame_ethernet_header_ethertype_get(ether);
	mac  = hull_frame_ethernet_header_source_ptr(ether);
	/* ARP: who has .1. */
	if (type == 0x0806u && have4 && hull_frame_arp_packet_view(&msg, 14u, &view) == SITU_OK &&
	    hull_frame_arp_packet_validate(view) == SITU_OK &&
	    hull_frame_arp_packet_operation_get(view) == 1u &&
	    hull_frame_arp_packet_target_protocol_get(view) == self4.v4) {
		uint32_t wrote = 0u;

		memset(reply, 0, sizeof reply);
		if (hull_frame_ethernet_header_build(out, 14u, hull_frame_arp_packet_sender_hardware_ptr(view),
		                                     hull_mac, 0x0806u, &wrote) != SITU_OK ||
		    hull_frame_arp_packet_build(out + 14, 28u, 1u, 0x0800u, 6u, 4u, HULL_FRAME_ARP_OPERATION_REPLY,
		                                hull_mac, self4.v4,
		                                hull_frame_arp_packet_sender_hardware_ptr(view),
		                                hull_frame_arp_packet_sender_protocol_get(view),
		                                &wrote) != SITU_OK)
			return 1;
		put32(reply, 60u);
		cross(s, from, HULL_SEGMENT_IN, reply, 4u + 60u);
		return 1;
	}
	/* Neighbour discovery: who has ::1, asked of its solicited-node group
	 * rather than of hull's MAC, and so read before that is looked at. One
	 * for another target goes on as any multicast does, a machine's own
	 * address able to share ::1's group. */
	if (type == 0x86ddu && have6 && hull_frame_ipv6_header_view(&msg, 14u, &view) == SITU_OK &&
	    hull_frame_ipv6_header_validate(view) == SITU_OK &&
	    hull_frame_ipv6_header_next_header_get(view) == 58u &&
	    54u + hull_frame_ipv6_header_payload_length_get(view) <= size &&
	    size > 54u && frame[54] == 135u) {
		situ_view_t solicit;
		uint8_t     first[12];
		peer        asker = { .six = 1 };
		peer        to = { .six = 1 };
		uint16_t    payload = hull_frame_ipv6_header_payload_length_get(view);

		memcpy(asker.v6, hull_frame_ipv6_header_source_ptr(view), 16u);
		memcpy(to.v6, hull_frame_ipv6_header_destination_ptr(view), 16u);
		pseudo_any(first, &asker, &to, 58u, payload);
		const uint8_t *target;

		if (hull_frame_neighbor_solicitation_view(&msg, 54u, payload, &solicit) != SITU_OK ||
		    hull_frame_neighbor_solicitation_validate(solicit) != SITU_OK ||
		    hull_frame_neighbor_solicitation_checksum_check(solicit, first, sizeof first) != SITU_OK ||
		    hull_frame_ipv6_header_hop_limit_get(view) != 255u)
			return 0;
		/* For ::1, or for hull's link-local address where it advertises,
		 * which its advertisements come from. */
		target = hull_frame_neighbor_solicitation_target_ptr(solicit);
		if (memcmp(target, self6.v6, 16u) != 0 &&
		    !(advertises(s) && memcmp(target, hull_link_local, 16u) == 0))
			return 0;
		/* From ::, a machine's duplicate address detection of hull's own,
		 * which is not to be taken: nothing to answer it to. */
		if (!IN6_IS_ADDR_UNSPECIFIED((const struct in6_addr *)(const void *)asker.v6))
			send_advert(s, from, mac, &asker, target);
		return 1;
	}
	/* A router solicitation, where hull advertises the segment's network:
	 * its advertisement back to the machine that asked, which the kernel
	 * asks for as its interface comes up. Where it does not, it goes on as
	 * any multicast does, to whatever router a machine runs. */
	if (type == 0x86ddu && have6 && advertises(s) &&
	    hull_frame_ipv6_header_view(&msg, 14u, &view) == SITU_OK &&
	    hull_frame_ipv6_header_validate(view) == SITU_OK &&
	    hull_frame_ipv6_header_next_header_get(view) == 58u &&
	    54u + hull_frame_ipv6_header_payload_length_get(view) <= size &&
	    size > 54u && frame[54] == 133u) {
		situ_view_t solicit;
		uint8_t     first[12];
		peer        asker = { .six = 1 };
		peer        to = { .six = 1 };
		uint16_t    payload = hull_frame_ipv6_header_payload_length_get(view);

		memcpy(asker.v6, hull_frame_ipv6_header_source_ptr(view), 16u);
		memcpy(to.v6, hull_frame_ipv6_header_destination_ptr(view), 16u);
		pseudo_any(first, &asker, &to, 58u, payload);
		if (hull_frame_router_solicitation_view(&msg, 54u, payload, &solicit) != SITU_OK ||
		    hull_frame_router_solicitation_validate(solicit) != SITU_OK ||
		    hull_frame_router_solicitation_checksum_check(solicit, first, sizeof first) != SITU_OK ||
		    hull_frame_ipv6_header_hop_limit_get(view) != 255u)
			return 0;
		send_ra(s, from, mac, &asker);
		return 1;
	}
	if (memcmp(hull_frame_ethernet_header_destination_ptr(ether), hull_mac, 6u) == 0) {
		size_t             at;
		size_t             l4;
		unsigned           protocol;
		size_t             dns;
		size_t             question = 0u;
		uint8_t            dns_out[MAX_DNS];
		uint8_t            first[12];
		const uint8_t     *query;
		size_t             query_length;
		struct sockaddr_in6 servers[3];
		unsigned           count = 0u;
		situ_view_t        inner;
		peer               asker;
		peer              *self;

		memset(&asker, 0, sizeof asker);
		/* For .1 and not ARP, DNS, ping or neighbour discovery: nothing
		 * else answers there. A header that does not validate, its sum
		 * among it, is not read, nor IPv6 with an extension header. What
		 * is for beyond .1 or ::1 is forward()'s, out through it or
		 * nowhere. */
		if (type == 0x0800u && have4) {
			size_t header;
			size_t datagram;

			if (hull_frame_ipv4_header_view(&msg, 14u, (uint32_t)(size - 14u), &view) != SITU_OK ||
			    hull_frame_ipv4_header_validate(view) != SITU_OK ||
			    hull_frame_ipv4_header_header_checksum_check(view) != SITU_OK)
				return 1;
			/* For beyond .1: its way out's, where it is a gateway. */
			if (hull_frame_ipv4_header_destination_get(view) != self4.v4)
				return 0;
			if (hull_frame_ipv4_header_more_fragments_get(view) ||
			    hull_frame_ipv4_header_fragment_offset_get(view) != 0u)
				return 1;
			header = (size_t)hull_frame_ipv4_header_ihl_get(view) * 4u;
			datagram = hull_frame_ipv4_header_total_length_get(view);
			if (datagram < header || 14u + datagram > size)
				return 1;
			asker.v4 = hull_frame_ipv4_header_source_get(view);
			at       = 14u + header;
			l4       = datagram - header;
			protocol = hull_frame_ipv4_header_protocol_get(view);
			self     = &self4;
		} else if (type == 0x86ddu && have6) {
			if (hull_frame_ipv6_header_view(&msg, 14u, &view) != SITU_OK ||
			    hull_frame_ipv6_header_validate(view) != SITU_OK)
				return 1;
			if (memcmp(hull_frame_ipv6_header_destination_ptr(view), self6.v6, 16u) != 0)
				return 0;
			if (54u + hull_frame_ipv6_header_payload_length_get(view) > size)
				return 1;
			asker.six = 1;
			memcpy(asker.v6, hull_frame_ipv6_header_source_ptr(view), 16u);
			at       = 54u;
			l4       = hull_frame_ipv6_header_payload_length_get(view);
			protocol = hull_frame_ipv6_header_next_header_get(view);
			self     = &self6;
		} else {
			return 1;
		}
		pseudo_any(first, &asker, self, (uint8_t)protocol, (uint32_t)l4);
		if (protocol == 6u) {
			if (hull_frame_tcp_segment_view(&msg, (uint32_t)at, (uint32_t)l4, &inner) == SITU_OK &&
			    hull_frame_tcp_segment_validate(inner) == SITU_OK &&
			    hull_frame_tcp_segment_checksum_check(inner, first, sizeof first) == SITU_OK &&
			    hull_frame_tcp_segment_destination_port_get(inner) == 53u)
				tcp_in(s, from, mac, &asker, inner, network, prefix);
			return 1;
		}
		/* An echo request, ping's: answered, its sum right, back through
		 * the asker's own way in, so a link's impairment shows in it. */
		if (protocol == 1u && !asker.six) {
			if (hull_frame_icmp_echo_view(&msg, (uint32_t)at, (uint32_t)l4, &inner) == SITU_OK &&
			    hull_frame_icmp_echo_validate(inner) == SITU_OK &&
			    hull_frame_icmp_echo_checksum_check(inner) == SITU_OK &&
			    hull_frame_icmp_echo_type_get(inner) == 8u)
				send_echo(s, from, mac, &asker, hull_frame_icmp_echo_identifier_get(inner),
				          hull_frame_icmp_echo_sequence_get(inner),
				          hull_frame_icmp_echo_data_ptr(inner), hull_frame_icmp_echo_data_len(inner));
			return 1;
		}
		if (protocol == 58u && asker.six) {
			if (hull_frame_icmp6_echo_view(&msg, (uint32_t)at, (uint32_t)l4, &inner) == SITU_OK &&
			    hull_frame_icmp6_echo_validate(inner) == SITU_OK &&
			    hull_frame_icmp6_echo_checksum_check(inner, first, sizeof first) == SITU_OK &&
			    hull_frame_icmp6_echo_type_get(inner) == 128u)
				send_echo(s, from, mac, &asker, hull_frame_icmp6_echo_identifier_get(inner),
				          hull_frame_icmp6_echo_sequence_get(inner),
				          hull_frame_icmp6_echo_data_ptr(inner), hull_frame_icmp6_echo_data_len(inner));
			return 1;
		}
		/* A UDP sum of 0 is a datagram sent with none, which IPv4 allows
		 * and frame.situ's udp_checksum accepts; IPv6 does not (RFC 8200
		 * 8.1), so there it is refused. */
		if (protocol != 17u ||
		    hull_frame_udp_datagram_view(&msg, (uint32_t)at, (uint32_t)l4, &inner) != SITU_OK ||
		    hull_frame_udp_datagram_validate(inner) != SITU_OK ||
		    hull_frame_udp_datagram_destination_port_get(inner) != 53u ||
		    (asker.six && get16(hull_frame_udp_datagram_checksum_ptr(inner)) == 0u) ||
		    hull_frame_udp_datagram_checksum_check(inner, first, sizeof first) != SITU_OK)
			return 1;
		query        = hull_frame_udp_datagram_payload_ptr(inner);
		query_length = hull_frame_udp_datagram_payload_len(inner);
		/* Asked on only for a machine with a way out, and where there is a
		 * resolver to ask, from a socket opened for the first question;
		 * otherwise refused, as before there was one. */
		if (s->ports[from]->forwards && (count = resolvers(servers)) != 0u && upstream < 0) {
			open_upstream();
			if (upstream < 0)
				count = 0u;
			else
				(void)!write(wake[1], "", 1u);   /* for the poll() to take it */
		}
		dns = dns_answer(s, network, prefix, query, query_length, count != 0u, dns_out,
		                 sizeof dns_out, &question);
		if (dns == ASK_ON)
			ask_on(s, from, mac, &asker, frame + at, query, query_length,
			       &servers[asking_next++ % count], -1);
		else if (dns != 0u)
			send_answer(s, from, mac, &asker, frame + at, dns_out, dns);
		return 1;
	}
	return 0;
}

/* --- a way out through .1 -------------------------------------------------- */

/*
 * A port's way out, where its segment is a gateway (Appendix D): passt,
 * as the user, on a socket pair whose other end is the switch's, given
 * the machine's own address, its subnet and .1 as its gateway, and its
 * IPv6 address and hull's link-local one where the segment has an IPv6
 * network, so that it is the machine's NAT as a vm's passt is the vm's. One a machine, as
 * passt serves one guest: measured with two behind one, its replies went
 * to the MAC that last spoke, and two flows from one source port, one
 * from each machine, shared a host port and the second took the first's
 * answers. Its MAC is hull's, so that what it sends comes from .1 and
 * ::1. It
 * answers no ARP, DHCP or NDP, which are the switch's or nobody's, maps
 * no host loopback onto .1 and forwards nothing in. passt's parent exits
 * once it is set up, saying why where it cannot; the daemon it leaves
 * ends when the switch's end closes, with the port.
 */
static void dotted(uint32_t address, char out[16]);

static uplink *start_uplink(uint32_t address, unsigned prefix, const uint8_t *address6,
                           unsigned prefix6, char *why, size_t cap)
{
	char        self[16];
	char        bits[12];
	char        gateway[16];
	char        self6[INET6_ADDRSTRLEN];
	char        gateway6[INET6_ADDRSTRLEN];
	const char *argv[32];
	unsigned    a = 0u;
	uint32_t    mask = prefix == 0u ? 0u : 0xffffffffu << (32u - prefix);
	int         pair[2];
	int         said[2];
	int         status;
	pid_t       pid;
	uplink     *up;

	argv[a++] = "passt";
	argv[a++] = "--quiet";
	argv[a++] = "--fd";
	argv[a++] = "3";
	argv[a++] = "--one-off";
	if (prefix != 0u) {
		dotted(address, self);
		snprintf(bits, sizeof bits, "%u", prefix);
		dotted((address & mask) + 1u, gateway);
		argv[a++] = "--address";
		argv[a++] = self;
		argv[a++] = "--netmask";
		argv[a++] = bits;
		argv[a++] = "--gateway";
		argv[a++] = gateway;
	} else {
		argv[a++] = "--ipv6-only";
	}
	if (prefix6 != 0u) {
		/* Its gateway is where its router advertisements would come
		 * from, which it sends none of: hull's link-local address. */
		inet_ntop(AF_INET6, address6, self6, sizeof self6);
		inet_ntop(AF_INET6, hull_link_local, gateway6, sizeof gateway6);
		argv[a++] = "--address";
		argv[a++] = self6;
		argv[a++] = "--gateway";
		argv[a++] = gateway6;
	} else {
		argv[a++] = "--ipv4-only";
	}
	argv[a++] = "--no-map-gw";
	argv[a++] = "--mac-addr";
	argv[a++] = "02:68:75:6c:6c:01";
	argv[a++] = "--no-dhcp";
	argv[a++] = "--no-ndp";
	argv[a++] = "--no-dhcpv6";
	argv[a++] = "--no-ra";
	argv[a++] = "--tcp-ports";
	argv[a++] = "none";
	argv[a++] = "--udp-ports";
	argv[a++] = "none";
	argv[a] = NULL;
	if ((up = calloc(1u, sizeof *up)) == NULL) {
		say(why, cap, "out of memory");
		return NULL;
	}
	if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, pair) != 0) {
		say(why, cap, strerror(errno));
		free(up);
		return NULL;
	}
	if (pipe2(said, O_CLOEXEC) != 0) {
		say(why, cap, strerror(errno));
		close(pair[0]);
		close(pair[1]);
		free(up);
		return NULL;
	}
	pid = fork();
	if (pid == 0) {
		int null = open("/dev/null", O_RDWR);

		if (null < 0 || dup2(null, 0) < 0 || dup2(null, 1) < 0 || dup2(said[1], 2) < 0 ||
		    dup2(pair[1], 3) < 0)
			_exit(126);
		syscall(SYS_close_range, 4u, ~0u, 0u);
		signal(SIGPIPE, SIG_DFL);
		execvp(argv[0], (char *const *)argv);
		dprintf(2, "%s\n", strerror(errno));
		_exit(127);
	}
	close(pair[1]);
	close(said[1]);
	if (pid > 0)
		while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
			;
	if (pid < 0 || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
		char    line[200] = "";
		ssize_t n = pid < 0 ? -1 : read(said[0], line, sizeof line - 1u);
		char   *end;

		/* Its last line, which says why. */
		line[n > 0 ? n : 0] = '\0';
		while ((end = strrchr(line, '\n')) != NULL && end[1] == '\0')
			*end = '\0';
		snprintf(why, cap, "passt, which a segment's gateway needs: %s",
		         (end = strrchr(line, '\n')) != NULL ? end + 1 :
		         line[0] != '\0' ? line : "it could not start");
		close(said[0]);
		close(pair[0]);
		free(up);
		return NULL;
	}
	close(said[0]);
	fcntl(pair[0], F_SETFL, O_NONBLOCK);
	up->fd = pair[0];
	return up;
}

/* Whether `a` is in the IPv6 network of `prefix` bits at `network`. */
static int within6(const uint8_t *a, const uint8_t *network, unsigned prefix)
{
	for (unsigned bit = 0u; bit < prefix; bit++)
		if (((a[bit / 8u] ^ network[bit / 8u]) >> (7u - bit % 8u) & 1u) != 0u)
			return 0;
	return 1;
}

/* A frame `p` sent to .1's MAC that .1 and ::1 do not answer: to its
 * passt, where it has one, if it is for beyond the subnet of its family
 * -- one for a sibling is the sibling's MAC's, and hull's own are
 * answered or are not -- and not for a link-local or multicast address,
 * which no router forwards; dropped otherwise, as a router without that
 * route drops it. */
static void route_out(segment *s, unsigned from, const uint8_t *framed, size_t length)
{
	port       *p = s->ports[from];
	uplink     *up = p->up;
	uint32_t    mask = p->prefix == 0u ? 0u : 0xffffffffu << (32u - p->prefix);
	situ_msg_t  msg;
	situ_view_t ether;
	situ_view_t view;
	uint16_t    type;
	ssize_t     n;

	if (up == NULL || up->fd < 0 || up->out_length != 0u)
		return;
	situ_msg_init(&msg, (uint8_t *)(uintptr_t)(framed + 4), (uint32_t)(length - 4u));
	if (hull_frame_ethernet_header_view(&msg, 0u, &ether) != SITU_OK)
		return;
	type = hull_frame_ethernet_header_ethertype_get(ether);
	if (type == 0x0800u) {
		if (p->prefix == 0u ||
		    hull_frame_ipv4_header_view(&msg, 14u, (uint32_t)(length - 4u - 14u), &view) != SITU_OK ||
		    hull_frame_ipv4_header_validate(view) != SITU_OK ||
		    (hull_frame_ipv4_header_destination_get(view) & mask) == (p->address & mask))
			return;
	} else if (type == 0x86ddu) {
		const uint8_t *to;

		if (p->prefix6 == 0u || hull_frame_ipv6_header_view(&msg, 14u, &view) != SITU_OK ||
		    hull_frame_ipv6_header_validate(view) != SITU_OK)
			return;
		to = hull_frame_ipv6_header_destination_ptr(view);
		if (within6(to, p->address6, p->prefix6) || to[0] == 0xffu ||
		    (to[0] == 0xfeu && (to[1] & 0xc0u) == 0x80u))
			return;
	} else {
		return;
	}
	n = send(up->fd, framed, length, MSG_DONTWAIT | MSG_NOSIGNAL);
	if (n < 0)
		n = 0;
	if ((size_t)n < length) {
		memcpy(up->out, framed + n, length - (size_t)n);
		up->out_length = length - (size_t)n;
	}
}

/* What is left of `up`'s output, as much as passt takes now. */
static void drain_uplink(uplink *up)
{
	ssize_t n;

	if (up->out_length == 0u)
		return;
	n = send(up->fd, up->out, up->out_length, MSG_DONTWAIT | MSG_NOSIGNAL);
	if (n > 0) {
		memmove(up->out, up->out + n, up->out_length - (size_t)n);
		up->out_length -= (size_t)n;
	}
}

/* Read what port `i`'s passt sent, each IP frame it completes given to
 * the port through its link, as a sibling's would be. A passt gone ends
 * the way out and nothing else. */
static void take_uplink(segment *s, unsigned i)
{
	uplink *up = s->ports[i]->up;
	ssize_t n = recv(up->fd, up->in + up->in_length, sizeof up->in - up->in_length, MSG_DONTWAIT);

	if (n < 0 && (errno == EAGAIN || errno == EINTR))
		return;
	if (n <= 0) {
		close(up->fd);
		up->fd = -1;
		return;
	}
	up->in_length += (size_t)n;
	for (;;) {
		uint32_t frame;
		size_t   whole;

		if (up->in_length < 4u)
			break;
		frame = (uint32_t)up->in[0] << 24 | (uint32_t)up->in[1] << 16 |
		        (uint32_t)up->in[2] << 8 | (uint32_t)up->in[3];
		if (frame > MAX_FRAME) {
			close(up->fd);
			up->fd = -1;
			return;
		}
		whole = 4u + frame;
		if (up->in_length < whole)
			break;
		if (whole >= 4u + 14u &&
		    (get16(up->in + 4u + 12u) == 0x0800u || get16(up->in + 4u + 12u) == 0x86ddu))
			cross(s, i, HULL_SEGMENT_IN, up->in, whole);
		memmove(up->in, up->in + whole, up->in_length - whole);
		up->in_length -= whole;
	}
}

static void forward(segment *s, unsigned from, const uint8_t *framed, size_t length)
{
	int to;

	if (length < 4u + 14u)
		return;
	if (answer(s, from, framed, length))
		return;
	/* To .1's MAC and not .1's to answer: out through it, where the port
	 * has a way out, and to no other port either way, the MAC being no
	 * machine's. */
	if (memcmp(framed + 4, hull_mac, 6u) == 0) {
		route_out(s, from, framed, length);
		return;
	}
	to = lookup(s, framed + 4, from);
	if (to == -2)
		return;
	if (to >= 0 && (unsigned)to < s->port_count) {
		cross(s, (unsigned)to, HULL_SEGMENT_IN, framed, length);
		return;
	}
	for (unsigned i = 0u; i < s->port_count; i++)
		if (i != from)
			cross(s, i, HULL_SEGMENT_IN, framed, length);
}

/* The frames `s`'s port `i` holds that are due by `now`, handed on; the
 * first still held's time, or UINT64_MAX. */
static uint64_t release(segment *s, unsigned i, uint64_t now)
{
	uint64_t first = UINT64_MAX;

	for (int direction = 0; direction < 2; direction++) {
		way *w = &s->ports[i]->way[direction];

		while (w->queue != NULL && w->queue->due <= now) {
			held *h = w->queue;

			w->queue = h->next;
			w->queued--;
			if (direction == HULL_SEGMENT_OUT) {
				w->count.passed++;
				w->count.passed_bytes += h->length - 4u;
				forward(s, i, h->framed, h->length);
			} else {
				write_port(s, s->ports[i], h->framed, h->length);
			}
			free(h);
		}
		if (w->queue != NULL && w->queue->due < first)
			first = w->queue->due;
	}
	return first;
}

static void free_port(port *p)
{
	for (int direction = 0; direction < 2; direction++) {
		while (p->way[direction].queue != NULL) {
			held *h = p->way[direction].queue;

			p->way[direction].queue = h->next;
			free(h);
		}
	}
	close(p->fd);
	if (p->up != NULL && p->up->fd >= 0)
		close(p->up->fd);
	free(p->up);
	free(p->hosts);
	free(p);
}

/* `s`'s port `i` gone: the last moved into its place, and the addresses
 * learned on either told so. */
static void remove_port(segment *s, unsigned i)
{
	unsigned last = s->port_count - 1u;

	free_port(s->ports[i]);
	s->ports[i] = s->ports[last];
	s->ports[last] = NULL;
	s->port_count--;
	for (unsigned k = 0u; k < MAX_LEARNED; k++) {
		if (s->table[k].used && s->table[k].port == i)
			s->table[k].used = 0;
		else if (s->table[k].used && s->table[k].port == last)
			s->table[k].port = i;
	}
}

/* Read what `p` sent, passing each frame it completes through its out.
 * 0 where the port is gone. */
static int take(segment *s, unsigned i)
{
	port   *p = s->ports[i];
	ssize_t n = recv(p->fd, p->in + p->in_length, sizeof p->in - p->in_length, MSG_DONTWAIT);

	if (n == 0 || (n < 0 && errno != EAGAIN && errno != EINTR))
		return 0;
	if (n < 0)
		return 1;
	p->in_length += (size_t)n;
	for (;;) {
		uint32_t frame;
		size_t   whole;

		if (p->in_length < 4u)
			break;
		frame = (uint32_t)p->in[0] << 24 | (uint32_t)p->in[1] << 16 | (uint32_t)p->in[2] << 8 |
		        (uint32_t)p->in[3];
		if (frame > MAX_FRAME)
			return 0;
		whole = 4u + frame;
		if (p->in_length < whole)
			break;
		if (whole >= 4u + 14u)
			cross(s, i, HULL_SEGMENT_OUT, p->in, whole);
		memmove(p->in, p->in + whole, p->in_length - whole);
		p->in_length -= whole;
	}
	return 1;
}

static void *serve(void *arg)
{
	uint64_t first = UINT64_MAX;

	(void)arg;
	/* Static, the switch being this one thread: every port of every
	 * segment is a third of a megabyte, too much for a stack. */
	static struct pollfd fds[2u + 2u * MAX_SEGMENTS * MAX_PORTS];
	static segment      *of[2u * MAX_SEGMENTS * MAX_PORTS];
	static unsigned      at[2u * MAX_SEGMENTS * MAX_PORTS];
	static int           up_of[2u * MAX_SEGMENTS * MAX_PORTS];   /* the port's passt's */

	for (;;) {
		nfds_t        base;
		nfds_t        n;
		int           wait_ms = -1;
		uint64_t      now;

		fds[0] = (struct pollfd){ wake[0], POLLIN, 0 };
		pthread_mutex_lock(&lock);
		fds[1] = (struct pollfd){ upstream, POLLIN, 0 };
		base = upstream >= 0 ? 2u : 1u;
		n = base;
		for (unsigned k = 0u; k < MAX_SEGMENTS; k++) {
			segment *s = segments[k];

			for (unsigned i = 0u; s != NULL && i < s->port_count; i++) {
				const uplink *up = s->ports[i]->up;

				fds[n] = (struct pollfd){ s->ports[i]->fd,
				                          (short)(POLLIN | (s->ports[i]->out_length != 0u ?
				                                            POLLOUT : 0)), 0 };
				of[n - base] = s;
				at[n - base] = i;
				up_of[n - base] = 0;
				n++;
				if (up == NULL || up->fd < 0)
					continue;
				fds[n] = (struct pollfd){ up->fd,
				                          (short)(POLLIN | (up->out_length != 0u ? POLLOUT : 0)),
				                          0 };
				of[n - base] = s;
				at[n - base] = i;
				up_of[n - base] = 1;
				n++;
			}
		}
		pthread_mutex_unlock(&lock);
		if (first != UINT64_MAX) {
			now = now_ns();
			wait_ms = first <= now ? 0 : (int)((first - now + 999999u) / 1000000u);
		}
		if (poll(fds, n, wait_ms) < 0 && errno != EINTR)
			break;
		if (fds[0].revents != 0) {
			char drained[64];

			while (read(wake[0], drained, sizeof drained) > 0)
				;
		}
		pthread_mutex_lock(&lock);
		if (base == 2u && fds[1].revents != 0 && fds[1].fd == upstream)
			answered();
		/* Backwards, since a port removed takes the last one's place. */
		for (nfds_t k = n; k > base; k--) {
			segment *s = of[k - 1u - base];
			unsigned i = at[k - 1u - base];

			if (fds[k - 1u].revents == 0 || i >= s->port_count)
				continue;
			if (up_of[k - 1u - base]) {
				uplink *up = s->ports[i]->up;

				if (up == NULL || up->fd != fds[k - 1u].fd)
					continue;
				if ((fds[k - 1u].revents & POLLOUT) != 0)
					drain_uplink(up);
				if ((fds[k - 1u].revents & (POLLIN | POLLHUP | POLLERR)) != 0)
					take_uplink(s, i);
				continue;
			}
			if (s->ports[i]->fd != fds[k - 1u].fd)
				continue;
			if ((fds[k - 1u].revents & POLLOUT) != 0)
				drain(s->ports[i]);
			if ((fds[k - 1u].revents & (POLLIN | POLLHUP | POLLERR)) != 0 && !take(s, i))
				remove_port(s, i);
		}
		/* What is due now, and when the next is. */
		now   = now_ns();
		first = UINT64_MAX;
		for (unsigned k = 0u; k < MAX_SEGMENTS; k++) {
			segment *s = segments[k];
			uint64_t due;

			for (unsigned i = 0u; s != NULL && i < s->port_count; i++) {
				due = release(s, i, now);
				if (due < first)
					first = due;
			}
			if (s != NULL && (due = fire_sources(s, now)) < first)
				first = due;
		}
		{
			uint64_t due = conn_timers(now);

			if (due < first)
				first = due;
		}
		{
			int left = 0;

			for (unsigned k = 0u; k < MAX_SEGMENTS; k++) {
				if (segments[k] != NULL && segments[k]->port_count == 0u) {
					end_capture(segments[k]);
					free(segments[k]);
					segments[k] = NULL;
				}
				left |= segments[k] != NULL;
			}
			/* No segment left, no question to wait on: the core keeps no
			 * descriptor its machines are gone from. */
			if (!left && upstream >= 0) {
				close(upstream);
				upstream = -1;
				memset(asking, 0, sizeof asking);
			}
			for (unsigned i = 0u; !left && i < MAX_CONNS; i++)
				if (conns[i] != NULL)
					conn_drop((int)i);
		}
		pthread_mutex_unlock(&lock);
	}
	return NULL;
}

static segment *find(const char *key)
{
	for (unsigned k = 0u; k < MAX_SEGMENTS; k++)
		if (segments[k] != NULL && strcmp(segments[k]->key, key) == 0)
			return segments[k];
	return NULL;
}

/* `address` as a.b.c.d. */
static void dotted(uint32_t address, char out[16])
{
	snprintf(out, 16u, "%u.%u.%u.%u", address >> 24, address >> 16 & 0xffu, address >> 8 & 0xffu,
	         address & 0xffu);
}

/* Where `p` cannot join `s` as hull addresses it, why: another file's
 * machine already on it with another subnet, or with p's address. Each
 * file's draw sees only its own machines, so this is where two meet. A
 * port of p's own machine is passed over, being a vm's earlier attempt
 * whose end the switch has not yet seen close. */
static int addressed_apart(const segment *s, const port *p, char *why, size_t cap)
{
	uint32_t mask = p->prefix == 0u ? 0u : 0xffffffffu << (32u - p->prefix);

	for (unsigned i = 0u; p->prefix != 0u && i < s->port_count; i++) {
		const port *q = s->ports[i];
		char        theirs[16];
		char        ours[16];

		if (q->prefix == 0u || strcmp(q->machine, p->machine) == 0)
			continue;
		if (q->prefix != p->prefix || ((q->address ^ p->address) & mask) != 0u) {
			dotted(q->address & (0xffffffffu << (32u - q->prefix)), theirs);
			dotted(p->address & mask, ours);
			snprintf(why, cap,
			         "segment %s is %s/%u, as machine %s has it, and this machine gives it "
			         "%s/%u",
			         s->key, theirs, q->prefix, q->machine, ours, p->prefix);
			return 0;
		}
		if (q->address == p->address) {
			dotted(p->address, ours);
			snprintf(why, cap, "its address on segment %s, %s, is machine %s's", s->key, ours,
			         q->machine);
			return 0;
		}
	}
	/* And the same of its IPv6 network and address. */
	for (unsigned i = 0u; p->prefix6 != 0u && i < s->port_count; i++) {
		const port *q = s->ports[i];
		char        shown[INET6_ADDRSTRLEN];
		int         apart = 0;

		if (q->prefix6 == 0u || strcmp(q->machine, p->machine) == 0)
			continue;
		for (unsigned bit = 0u; bit < p->prefix6 && !apart; bit++)
			apart = ((q->address6[bit / 8u] ^ p->address6[bit / 8u]) & (0x80u >> (bit % 8u))) != 0;
		if (q->prefix6 != p->prefix6 || apart) {
			snprintf(why, cap, "segment %s has another IPv6 network, as machine %s has it, "
			         "than this machine gives it", s->key, q->machine);
			return 0;
		}
		if (memcmp(q->address6, p->address6, 16u) == 0) {
			inet_ntop(AF_INET6, p->address6, shown, sizeof shown);
			snprintf(why, cap, "its address on segment %s, %s, is machine %s's", s->key, shown,
			         q->machine);
			return 0;
		}
	}
	return 1;
}

int hull_segment_port(const char *key, const char *machine, char *why, size_t cap)
{
	return hull_segment_port_addressed(key, machine, 0u, 0u, 0, why, cap);
}

void hull_segment_resolvers(const char *const *servers, const unsigned *ports, unsigned count)
{
	pthread_mutex_lock(&lock);
	resolvers_given_count = 0u;
	for (unsigned i = 0u; i < count && i < 3u; i++)
		if (resolver_address(servers[i], ports[i], &resolvers_given[resolvers_given_count]))
			resolvers_given_count++;
	pthread_mutex_unlock(&lock);
}

int hull_segment_port_addressed(const char *key, const char *machine, uint32_t address,
                                unsigned prefix, int forwards, char *why, size_t cap)
{
	return hull_segment_port_named(key, machine, address, prefix, NULL, 0u, 0, 0, forwards,
	                               NULL, 0u, why, cap);
}

int hull_segment_port_named(const char *key, const char *machine, uint32_t address,
                            unsigned prefix, const uint8_t *address6, unsigned prefix6,
                            int advertise, int gateway, int forwards,
                            const hull_segment_host *hosts, unsigned count, char *why, size_t cap)
{
	int      pair[2];
	segment *s = NULL;
	port    *p;
	uplink  *up = NULL;

	if (strlen(key) >= sizeof s->key) {
		say(why, cap, "a segment's name is too long");
		return -1;
	}
	if (machine == NULL || strlen(machine) >= sizeof p->machine) {
		say(why, cap, "a port's machine is named by its id");
		return -1;
	}
	if (prefix > 32u || prefix6 > 128u || (prefix6 != 0u && address6 == NULL)) {
		say(why, cap, "a subnet's prefix is at most 32, an IPv6 one's 128, with its address");
		return -1;
	}
	/* Its passt before the lock, which the switch's frames wait on. */
	if (gateway && (prefix != 0u || prefix6 != 0u) &&
	    (up = start_uplink(address, prefix, address6, prefix6, why, cap)) == NULL)
		return -1;
	if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, pair) != 0) {
		say(why, cap, strerror(errno));
		if (up != NULL)
			close(up->fd);
		free(up);
		return -1;
	}
	p = calloc(1u, sizeof *p);
	if (p != NULL && count != 0u && (p->hosts = malloc(count * sizeof *hosts)) == NULL) {
		free(p);
		p = NULL;
	}
	if (p == NULL) {
		close(pair[0]);
		close(pair[1]);
		if (up != NULL)
			close(up->fd);
		free(up);
		say(why, cap, "out of memory");
		return -1;
	}
	p->up = up;
	p->fd = pair[0];
	snprintf(p->machine, sizeof p->machine, "%s", machine);
	p->address = address;
	p->prefix  = prefix;
	if (prefix6 != 0u)
		memcpy(p->address6, address6, 16u);
	p->prefix6 = prefix6;
	p->advertise = advertise && prefix6 != 0u;
	p->forwards = forwards;
	if (count != 0u)
		memcpy(p->hosts, hosts, count * sizeof *hosts);
	p->host_count = count;
	fcntl(p->fd, F_SETFL, O_NONBLOCK);
	pthread_mutex_lock(&lock);
	if (!running) {
		pthread_t thread;

		if (pipe2(wake, O_CLOEXEC | O_NONBLOCK) != 0 ||
		    pthread_create(&thread, NULL, serve, NULL) != 0) {
			pthread_mutex_unlock(&lock);
			free_port(p);
			close(pair[1]);
			say(why, cap, "the switch could not be started");
			return -1;
		}
		pthread_detach(thread);
		running = 1;
	}
	s = find(key);
	for (unsigned k = 0u; k < MAX_SEGMENTS && s == NULL; k++) {
		if (segments[k] == NULL && (segments[k] = calloc(1u, sizeof *segments[k])) != NULL) {
			s = segments[k];
			snprintf(s->key, sizeof s->key, "%s", key);
		}
	}
	if (s == NULL || s->port_count == MAX_PORTS) {
		pthread_mutex_unlock(&lock);
		free_port(p);
		close(pair[1]);
		say(why, cap, s == NULL ? "too many segments" : "too many machines on the segment");
		return -1;
	}
	if (!addressed_apart(s, p, why, cap)) {
		pthread_mutex_unlock(&lock);
		free_port(p);
		close(pair[1]);
		return -1;
	}
	apply_rule(s, p, HULL_SEGMENT_OUT);
	apply_rule(s, p, HULL_SEGMENT_IN);
	s->ports[s->port_count++] = p;
	pthread_mutex_unlock(&lock);
	(void)!write(wake[1], "", 1u);
	return pair[1];
}

/* The rule of `kind` already set for `machine` (or every machine) on
 * `direction` of `s`, else a free one, else NULL where the segment is
 * full; it is marked used and keyed, its payload the caller's to set. */
static rule *rule_slot(segment *s, const char *machine, int direction, int kind)
{
	rule *r = NULL;

	for (unsigned k = 0u; k < MAX_RULES && r == NULL; k++)
		if (s->rules[k].used && s->rules[k].direction == direction &&
		    s->rules[k].kind == kind &&
		    strcmp(s->rules[k].machine, machine != NULL ? machine : "") == 0)
			r = &s->rules[k];
	for (unsigned k = 0u; k < MAX_RULES && r == NULL; k++)
		if (!s->rules[k].used)
			r = &s->rules[k];
	if (r != NULL) {
		r->used      = 1;
		r->direction = direction;
		r->kind      = kind;
		snprintf(r->machine, sizeof r->machine, "%s", machine != NULL ? machine : "");
	}
	return r;
}

int hull_segment_impair(const char *key, const char *machine, int direction,
                        const hull_impairment *impairment, char *why, size_t cap)
{
	segment *s;
	rule    *r;

	if (direction != HULL_SEGMENT_OUT && direction != HULL_SEGMENT_IN) {
		say(why, cap, "a direction is out or in");
		return -1;
	}
	if (machine != NULL && strlen(machine) >= sizeof s->ports[0]->machine) {
		say(why, cap, "a port's machine is named by its id");
		return -1;
	}
	pthread_mutex_lock(&lock);
	s = find(key);
	if (s == NULL) {
		pthread_mutex_unlock(&lock);
		say(why, cap, "no machine is on that segment");
		return -1;
	}
	r = rule_slot(s, machine, direction, KIND_IMPAIR);
	if (r == NULL) {
		pthread_mutex_unlock(&lock);
		say(why, cap, "too many impairments on the segment");
		return -1;
	}
	r->impairment = *impairment;
	/* Each port it reaches takes the rule it now takes, which for a
	 * machine with a rule of its own stays that one: a machine named
	 * outranks every machine. The other direction is left as it was,
	 * draws and all. */
	for (unsigned i = 0u; i < s->port_count; i++) {
		port *p = s->ports[i];

		if (machine == NULL || strcmp(p->machine, machine) == 0)
			apply_rule(s, p, direction);
	}
	pthread_mutex_unlock(&lock);
	(void)!write(wake[1], "", 1u);
	return 0;
}

int hull_segment_tamper(const char *key, const char *machine, int direction,
                        const hull_tamper *tamper, char *why, size_t cap)
{
	segment *s;
	rule    *r;

	if (direction != HULL_SEGMENT_OUT && direction != HULL_SEGMENT_IN) {
		say(why, cap, "a direction is out or in");
		return -1;
	}
	if (machine != NULL && strlen(machine) >= sizeof s->ports[0]->machine) {
		say(why, cap, "a port's machine is named by its id");
		return -1;
	}
	pthread_mutex_lock(&lock);
	s = find(key);
	if (s == NULL) {
		pthread_mutex_unlock(&lock);
		say(why, cap, "no machine is on that segment");
		return -1;
	}
	r = rule_slot(s, machine, direction, KIND_TAMPER);
	if (r == NULL) {
		pthread_mutex_unlock(&lock);
		say(why, cap, "too many faults on the segment");
		return -1;
	}
	r->tamper = *tamper;
	for (unsigned i = 0u; i < s->port_count; i++) {
		port *p = s->ports[i];

		if (machine == NULL || strcmp(p->machine, machine) == 0)
			apply_rule(s, p, direction);
	}
	pthread_mutex_unlock(&lock);
	(void)!write(wake[1], "", 1u);
	return 0;
}

int hull_segment_inject(const char *key, const char *machine, const unsigned char *frame,
                        size_t length, int randomize, unsigned size, uint64_t count,
                        unsigned every_ms, uint64_t seed, char *why, size_t cap)
{
	segment *s;
	source  *src = NULL;
	int      present = 0;

	if (machine != NULL && strlen(machine) >= sizeof src->machine) {
		say(why, cap, "a port's machine is named by its id");
		return -1;
	}
	if (count != 0u && (randomize ? (size < 14u || size > MAX_FRAME) : (length < 14u || length > MAX_FRAME))) {
		say(why, cap, "a frame is 14 to 65600 bytes");
		return -1;
	}
	pthread_mutex_lock(&lock);
	s = find(key);
	if (s == NULL) {
		pthread_mutex_unlock(&lock);
		say(why, cap, "no machine is on that segment");
		return -1;
	}
	/* A named target must be on the segment. */
	for (unsigned i = 0u; i < s->port_count; i++)
		if (machine == NULL || strcmp(s->ports[i]->machine, machine) == 0)
			present = 1;
	if (!present) {
		pthread_mutex_unlock(&lock);
		say(why, cap, "that machine is not on the segment");
		return -1;
	}
	/* A count of 0 removes the target's injectors. */
	if (count == 0u) {
		for (unsigned k = 0u; k < MAX_SOURCES; k++)
			if (s->sources[k].used &&
			    strcmp(s->sources[k].machine, machine != NULL ? machine : "") == 0)
				s->sources[k].used = 0;
		pthread_mutex_unlock(&lock);
		(void)!write(wake[1], "", 1u);
		return 0;
	}
	for (unsigned k = 0u; k < MAX_SOURCES && src == NULL; k++)
		if (!s->sources[k].used)
			src = &s->sources[k];
	if (src == NULL) {
		pthread_mutex_unlock(&lock);
		say(why, cap, "too many injectors on the segment");
		return -1;
	}
	memset(src, 0, sizeof *src);
	snprintf(src->machine, sizeof src->machine, "%s", machine != NULL ? machine : "");
	src->randomize = randomize;
	src->size      = size;
	if (!randomize) {
		memcpy(src->frame, frame, length);
		src->length = length;
	}
	src->left     = count;
	src->every_ms = every_ms;
	src->random   = seed;
	src->next     = now_ns();
	src->used     = 1;
	pthread_mutex_unlock(&lock);
	(void)!write(wake[1], "", 1u);
	return 0;
}

int hull_segment_counts_of(const char *key, hull_segment_counts *out, unsigned cap)
{
	segment *s;
	unsigned n = 0u;

	pthread_mutex_lock(&lock);
	s = find(key);
	if (s == NULL) {
		pthread_mutex_unlock(&lock);
		return -1;
	}
	for (unsigned i = 0u; i < s->port_count && n < cap; i++, n++) {
		snprintf(out[n].machine, sizeof out[n].machine, "%s", s->ports[i]->machine);
		out[n].way[HULL_SEGMENT_OUT] = s->ports[i]->way[HULL_SEGMENT_OUT].count;
		out[n].way[HULL_SEGMENT_IN]  = s->ports[i]->way[HULL_SEGMENT_IN].count;
	}
	pthread_mutex_unlock(&lock);
	return (int)n;
}

int hull_segment_capture(const char *key, const char *machine, int direction, const char *path,
                         uint64_t frames, char *why, size_t cap)
{
	/* pcap 2.4 with nanosecond times, as the magic says, of Ethernet. */
	static const uint32_t header[6] = { 0xa1b23c4du, 2u | 4u << 16, 0u, 0u, 65535u, 1u };
	segment              *s;
	int                   fd = -1;

	if (machine != NULL && strlen(machine) >= sizeof s->capture.machine) {
		say(why, cap, "a port's machine is named by its id");
		return -1;
	}
	if (frames != 0u && (path == NULL || path[0] != '/')) {
		say(why, cap, "a capture's file is named by its whole path");
		return -1;
	}
	pthread_mutex_lock(&lock);
	s = find(key);
	if (s == NULL || s->port_count == 0u) {
		pthread_mutex_unlock(&lock);
		say(why, cap, "no machine is on that segment");
		return -1;
	}
	if (frames != 0u) {
		fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0644);
		if (fd < 0 || write(fd, header, sizeof header) != (ssize_t)sizeof header) {
			pthread_mutex_unlock(&lock);
			snprintf(why, cap, "%s: %s", path, strerror(errno));
			if (fd >= 0)
				close(fd);
			return -1;
		}
	}
	end_capture(s);
	if (frames != 0u) {
		s->capture.on        = 1;
		s->capture.fd        = fd;
		s->capture.direction = direction;
		s->capture.left      = frames;
		snprintf(s->capture.machine, sizeof s->capture.machine, "%s", machine != NULL ? machine : "");
	}
	pthread_mutex_unlock(&lock);
	return 0;
}
