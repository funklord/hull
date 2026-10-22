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
#include <unistd.h>

#include "segment.h"

#define MAX_SEGMENTS 64u
#define MAX_PORTS    32u
#define MAX_LEARNED  256u
#define MAX_FRAME    (65536u + 64u)

typedef struct port {
	int      fd;
	uint8_t  in[4u + MAX_FRAME];
	size_t   in_length;
	uint8_t  out[4u + MAX_FRAME];
	size_t   out_length;
} port;

typedef struct learned {
	uint8_t  mac[6];
	unsigned port;
	int      used;
} learned;

typedef struct segment {
	char     key[512];
	port    *ports[MAX_PORTS];
	unsigned port_count;
	learned  table[MAX_LEARNED];
	unsigned next;    /* the entry a new address replaces, round the table */
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

/* The frame, its length before it, to `p`, or dropped where `p` has not
 * taken the last. */
static void deliver(port *p, const uint8_t *framed, size_t length)
{
	ssize_t n;

	if (p->out_length != 0u)
		return;
	n = send(p->fd, framed, length, MSG_DONTWAIT | MSG_NOSIGNAL);
	if (n < 0)
		n = 0;
	if ((size_t)n < length) {
		memcpy(p->out, framed + n, length - (size_t)n);
		p->out_length = length - (size_t)n;
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
		deliver(s->ports[to], framed, length);
		return;
	}
	for (unsigned i = 0u; i < s->port_count; i++)
		if (i != from)
			deliver(s->ports[i], framed, length);
}

/* `s`'s port `i` gone: the last moved into its place, and the addresses
 * learned on either told so. */
static void remove_port(segment *s, unsigned i)
{
	unsigned last = s->port_count - 1u;

	close(s->ports[i]->fd);
	free(s->ports[i]);
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

/* Read what `p` sent, forwarding each frame it completes. 0 where the
 * port is gone. */
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
		forward(s, i, p->in, whole);
		memmove(p->in, p->in + whole, p->in_length - whole);
		p->in_length -= whole;
	}
	return 1;
}

static void *serve(void *arg)
{
	(void)arg;
	for (;;) {
		struct pollfd fds[1u + MAX_SEGMENTS * MAX_PORTS];
		segment      *of[MAX_SEGMENTS * MAX_PORTS];
		unsigned      at[MAX_SEGMENTS * MAX_PORTS];
		nfds_t        n = 1u;

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
		if (poll(fds, n, -1) < 0 && errno != EINTR)
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

int hull_segment_port(const char *key, char *why, size_t cap)
{
	int      pair[2];
	segment *s = NULL;
	port    *p;

	if (strlen(key) >= sizeof s->key) {
		say(why, cap, "a segment's name is too long");
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
	for (unsigned k = 0u; k < MAX_SEGMENTS && s == NULL; k++)
		if (segments[k] != NULL && strcmp(segments[k]->key, key) == 0)
			s = segments[k];
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
	s->ports[s->port_count++] = p;
	pthread_mutex_unlock(&lock);
	(void)!write(wake[1], "", 1u);
	return pair[1];
}
