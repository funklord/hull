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

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "segment.h"

#define MAX_SEGMENTS 64u
#define MAX_PORTS    32u
#define MAX_LEARNED  256u
#define MAX_RULES    32u
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
	uint64_t           random;     /* the draws' state */
	int                bursting;   /* the Gilbert-Elliott model in its bad state */
	uint64_t           link_free;  /* when the rate's link is next free, ns */
	held              *queue;
	unsigned           queued;
	hull_segment_count count;
} way;

typedef struct port {
	int      fd;
	char     machine[64];
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

/* An impairment asked for a machine's ports, or every machine's where
 * `machine` is empty: given to each port that joins. */
typedef struct rule {
	char            machine[64];
	int             direction;
	hull_impairment impairment;
	int             used;
} rule;

typedef struct segment {
	char     key[512];
	port    *ports[MAX_PORTS];
	unsigned port_count;
	learned  table[MAX_LEARNED];
	unsigned next;    /* the entry a new address replaces, round the table */
	rule     rules[MAX_RULES];
} segment;

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static segment        *segments[MAX_SEGMENTS];
static int             wake[2] = { -1, -1 };
static int             running;

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
static uint64_t draw(way *w)
{
	uint64_t z = (w->random += 0x9e3779b97f4a7c15u);

	z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9u;
	z = (z ^ (z >> 27)) * 0x94d049bb133111ebu;
	return z ^ (z >> 31);
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

/* The rule `p`'s `direction` takes: its machine's own, else every
 * machine's, else none. */
static const rule *rule_for(const segment *s, const port *p, int direction)
{
	const rule *every = NULL;

	for (unsigned k = 0u; k < MAX_RULES; k++) {
		const rule *r = &s->rules[k];

		if (!r->used || r->direction != direction)
			continue;
		if (r->machine[0] == '\0')
			every = r;
		else if (strcmp(r->machine, p->machine) == 0)
			return r;
	}
	return every;
}

/* `p`'s `direction` given the rule it takes, if any. */
static void apply_rule(const segment *s, port *p, int direction)
{
	const rule *r = rule_for(s, p, direction);

	if (r != NULL)
		impair_way(&p->way[direction], p->machine, direction, &r->impairment);
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

/* The frame, its length before it, written to `p`'s socket, or dropped
 * where `p` has not taken the last. */
static void write_port(port *p, const uint8_t *framed, size_t length)
{
	way    *w = &p->way[HULL_SEGMENT_IN];
	ssize_t n;

	if (p->out_length != 0u) {
		w->count.full++;
		return;
	}
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

static void forward(segment *s, unsigned from, const uint8_t *framed, size_t length);

/* A frame through `w`, which is port `at` of `s`'s, direction
 * `direction`: dropped, held, or handed on now. */
static void cross(segment *s, unsigned at, int direction, const uint8_t *framed, size_t length)
{
	port                  *p = s->ports[at];
	way                   *w = &p->way[direction];
	const hull_impairment *m = &w->impairment;
	unsigned               copies = 1u;
	uint64_t               now;

	w->count.frames++;
	w->count.bytes += length - 4u;
	if (!w->impaired) {
		if (direction == HULL_SEGMENT_OUT) {
			w->count.passed++;
			w->count.passed_bytes += length - 4u;
			forward(s, at, framed, length);
		} else {
			write_port(p, framed, length);
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
	now = now_ns();
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
				write_port(p, copy, length);
			}
			continue;
		}
		if (w->queued >= (m->limit != 0u ? m->limit : QUEUE_FRAMES)) {
			w->count.overflow++;
			if (m->rate != 0u)
				w->link_free -= (uint64_t)(length - 4u) * 8u * 1000000000u / m->rate;
			continue;
		}
		{
			held  *h = malloc(sizeof *h + length);
			held **into = &w->queue;

			if (h == NULL) {
				w->count.overflow++;
				continue;
			}
			h->due    = due;
			h->length = length;
			memcpy(h->framed, copy, length);
			while (*into != NULL && (*into)->due <= due)
				into = &(*into)->next;
			h->next = *into;
			*into   = h;
			w->queued++;
		}
	}
}

static void forward(segment *s, unsigned from, const uint8_t *framed, size_t length)
{
	int to;

	if (length < 4u + 14u)
		return;
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
				write_port(s->ports[i], h->framed, h->length);
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
	for (;;) {
		struct pollfd fds[1u + MAX_SEGMENTS * MAX_PORTS];
		segment      *of[MAX_SEGMENTS * MAX_PORTS];
		unsigned      at[MAX_SEGMENTS * MAX_PORTS];
		nfds_t        n = 1u;
		int           wait_ms = -1;
		uint64_t      now;

		fds[0] = (struct pollfd){ wake[0], POLLIN, 0 };
		pthread_mutex_lock(&lock);
		for (unsigned k = 0u; k < MAX_SEGMENTS; k++) {
			segment *s = segments[k];

			for (unsigned i = 0u; s != NULL && i < s->port_count; i++) {
				fds[n] = (struct pollfd){ s->ports[i]->fd,
				                          (short)(POLLIN | (s->ports[i]->out_length != 0u ?
				                                            POLLOUT : 0)), 0 };
				of[n - 1u] = s;
				at[n - 1u] = i;
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
		/* Backwards, since a port removed takes the last one's place. */
		for (nfds_t k = n; k > 1u; k--) {
			segment *s = of[k - 2u];
			unsigned i = at[k - 2u];

			if (fds[k - 1u].revents == 0 || i >= s->port_count ||
			    s->ports[i]->fd != fds[k - 1u].fd)
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

			for (unsigned i = 0u; s != NULL && i < s->port_count; i++) {
				uint64_t due = release(s, i, now);

				if (due < first)
					first = due;
			}
		}
		for (unsigned k = 0u; k < MAX_SEGMENTS; k++) {
			if (segments[k] != NULL && segments[k]->port_count == 0u) {
				free(segments[k]);
				segments[k] = NULL;
			}
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

int hull_segment_port(const char *key, const char *machine, char *why, size_t cap)
{
	int      pair[2];
	segment *s = NULL;
	port    *p;

	if (strlen(key) >= sizeof s->key) {
		say(why, cap, "a segment's name is too long");
		return -1;
	}
	if (machine == NULL || strlen(machine) >= sizeof p->machine) {
		say(why, cap, "a port's machine is named by its id");
		return -1;
	}
	if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, pair) != 0) {
		say(why, cap, strerror(errno));
		return -1;
	}
	p = calloc(1u, sizeof *p);
	if (p == NULL) {
		close(pair[0]);
		close(pair[1]);
		say(why, cap, "out of memory");
		return -1;
	}
	p->fd = pair[0];
	snprintf(p->machine, sizeof p->machine, "%s", machine);
	fcntl(p->fd, F_SETFL, O_NONBLOCK);
	pthread_mutex_lock(&lock);
	if (!running) {
		pthread_t thread;

		if (pipe2(wake, O_CLOEXEC | O_NONBLOCK) != 0 ||
		    pthread_create(&thread, NULL, serve, NULL) != 0) {
			pthread_mutex_unlock(&lock);
			free(p);
			close(pair[0]);
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
		free(p);
		close(pair[0]);
		close(pair[1]);
		say(why, cap, s == NULL ? "too many segments" : "too many machines on the segment");
		return -1;
	}
	apply_rule(s, p, HULL_SEGMENT_OUT);
	apply_rule(s, p, HULL_SEGMENT_IN);
	s->ports[s->port_count++] = p;
	pthread_mutex_unlock(&lock);
	(void)!write(wake[1], "", 1u);
	return pair[1];
}

int hull_segment_impair(const char *key, const char *machine, int direction,
                        const hull_impairment *impairment, char *why, size_t cap)
{
	segment *s;
	rule    *r = NULL;

	if (direction != HULL_SEGMENT_OUT && direction != HULL_SEGMENT_IN) {
		say(why, cap, "a direction is out or in");
		return -1;
	}
	if (machine != NULL && strlen(machine) >= sizeof r->machine) {
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
	/* The rule it replaces, else a free one. */
	for (unsigned k = 0u; k < MAX_RULES && r == NULL; k++)
		if (s->rules[k].used && s->rules[k].direction == direction &&
		    strcmp(s->rules[k].machine, machine != NULL ? machine : "") == 0)
			r = &s->rules[k];
	for (unsigned k = 0u; k < MAX_RULES && r == NULL; k++)
		if (!s->rules[k].used)
			r = &s->rules[k];
	if (r == NULL) {
		pthread_mutex_unlock(&lock);
		say(why, cap, "too many impairments on the segment");
		return -1;
	}
	r->used       = 1;
	r->direction  = direction;
	r->impairment = *impairment;
	snprintf(r->machine, sizeof r->machine, "%s", machine != NULL ? machine : "");
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
