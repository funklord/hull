/*
 * nbd.c -- the NBD protocol's server side, as much of it as a read-only
 * disk needs (the NBD project's doc/proto.md): the fixed newstyle
 * handshake, NBD_OPT_EXPORT_NAME, NBD_OPT_INFO and NBD_OPT_GO answered
 * with the size and the read-only flag, every other option refused as
 * unsupported, and then reads, a flush, and a disconnect. One client, on a
 * stream given already connected -- qemu's end of a socketpair, which it
 * takes as `file.server.type=fd` -- so nothing listens anywhere.
 *
 * What a client sends is read as untrusted: a length past what a
 * request may carry ends the connection, and a read past the disk's end is
 * answered EINVAL.
 */
#define _GNU_SOURCE

#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "nbd.h"

#define NBD_MAGIC        0x4e42444d41474943ull  /* "NBDMAGIC" */
#define NBD_IHAVEOPT     0x49484156454f5054ull  /* "IHAVEOPT" */
#define NBD_REPLY_MAGIC  0x0003e889045565a9ull
#define NBD_REQUEST      0x25609513u
#define NBD_SIMPLE_REPLY 0x67446698u

#define FLAG_FIXED_NEWSTYLE 1u
#define FLAG_NO_ZEROES      2u
#define FLAG_HAS_FLAGS      1u
#define FLAG_READ_ONLY      2u
#define FLAG_SEND_FLUSH     4u

#define OPT_EXPORT_NAME 1u
#define OPT_ABORT       2u
#define OPT_INFO        6u
#define OPT_GO          7u

#define REP_ACK         1u
#define REP_INFO        3u
#define REP_ERR_UNSUP   0x80000001u
#define INFO_EXPORT     0u

#define CMD_READ  0u
#define CMD_DISC  2u
#define CMD_FLUSH 3u

#define MOST_OPTION  4096u        /* an option's data: a name and its requests */
#define MOST_READ    (32u << 20)  /* qemu asks 32 MiB at most */

typedef struct served {
	hull_media_map *map;
	uint64_t        bytes;
	int             fd;
} served;

static int full_read(int fd, void *to, size_t n)
{
	uint8_t *p = to;

	while (n > 0u) {
		ssize_t got = read(fd, p, n);

		if (got < 0 && errno == EINTR)
			continue;
		if (got <= 0)
			return -1;
		p += got;
		n -= (size_t)got;
	}
	return 0;
}

static int full_write(int fd, const void *from, size_t n)
{
	const uint8_t *p = from;

	while (n > 0u) {
		ssize_t wrote = write(fd, p, n);

		if (wrote < 0 && errno == EINTR)
			continue;
		if (wrote <= 0)
			return -1;
		p += wrote;
		n -= (size_t)wrote;
	}
	return 0;
}

static void be16(uint8_t *p, uint16_t v)
{
	p[0] = (uint8_t)(v >> 8);
	p[1] = (uint8_t)v;
}

static void be32(uint8_t *p, uint32_t v)
{
	for (unsigned i = 0u; i < 4u; i++)
		p[i] = (uint8_t)(v >> (24u - 8u * i));
}

static void be64(uint8_t *p, uint64_t v)
{
	for (unsigned i = 0u; i < 8u; i++)
		p[i] = (uint8_t)(v >> (56u - 8u * i));
}

static uint32_t get32(const uint8_t *p)
{
	return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static uint64_t get64(const uint8_t *p)
{
	return (uint64_t)get32(p) << 32 | get32(p + 4);
}

/* An option's reply: its header, then `length` bytes of `data`. */
static int reply(int fd, uint32_t option, uint32_t type, const uint8_t *data, uint32_t length)
{
	uint8_t head[20];

	be64(head, NBD_REPLY_MAGIC);
	be32(head + 8, option);
	be32(head + 12, type);
	be32(head + 16, length);
	return full_write(fd, head, sizeof head) == 0 &&
	       (length == 0u || full_write(fd, data, length) == 0) ? 0 : -1;
}

/* The handshake to the start of transmission: 0, or -1 to end. */
static int negotiate(served *s)
{
	static const uint16_t flags = FLAG_HAS_FLAGS | FLAG_READ_ONLY | FLAG_SEND_FLUSH;
	uint8_t               out[18];
	uint8_t               in[16];
	uint8_t               data[MOST_OPTION];
	uint32_t              client;

	be64(out, NBD_MAGIC);
	be64(out + 8, NBD_IHAVEOPT);
	be16(out + 16, FLAG_FIXED_NEWSTYLE | FLAG_NO_ZEROES);
	if (full_write(s->fd, out, 18u) != 0 || full_read(s->fd, in, 4u) != 0)
		return -1;
	client = get32(in);
	for (;;) {
		uint32_t option, length;

		if (full_read(s->fd, in, 16u) != 0 || get64(in) != NBD_IHAVEOPT)
			return -1;
		option = get32(in + 8);
		length = get32(in + 12);
		if (length > sizeof data || (length != 0u && full_read(s->fd, data, length) != 0))
			return -1;
		if (option == OPT_EXPORT_NAME) {
			uint8_t answer[10 + 124] = { 0 };

			be64(answer, s->bytes);
			be16(answer + 8, flags);
			return full_write(s->fd, answer, (client & FLAG_NO_ZEROES) ? 10u : sizeof answer);
		}
		if (option == OPT_INFO || option == OPT_GO) {
			uint8_t info[12];

			be16(info, INFO_EXPORT);
			be64(info + 2, s->bytes);
			be16(info + 10, flags);
			if (reply(s->fd, option, REP_INFO, info, sizeof info) != 0 ||
			    reply(s->fd, option, REP_ACK, NULL, 0u) != 0)
				return -1;
			if (option == OPT_GO)
				return 0;
			continue;
		}
		if (option == OPT_ABORT) {
			reply(s->fd, option, REP_ACK, NULL, 0u);
			return -1;
		}
		/* Structured replies, meta contexts, a list: not offered, and a
		 * client goes on without them. */
		if (reply(s->fd, option, REP_ERR_UNSUP, NULL, 0u) != 0)
			return -1;
	}
}

static int answer(int fd, uint32_t error, const uint8_t handle[8])
{
	uint8_t head[16];

	be32(head, NBD_SIMPLE_REPLY);
	be32(head + 4, error);
	memcpy(head + 8, handle, 8u);
	return full_write(fd, head, sizeof head);
}

static void *serve(void *arg)
{
	served  *s = arg;
	uint8_t *buffer = malloc(MOST_READ);

	if (buffer != NULL && negotiate(s) == 0) {
		uint8_t request[28];

		while (full_read(s->fd, request, sizeof request) == 0 && get32(request) == NBD_REQUEST) {
			uint16_t type = (uint16_t)(request[6] << 8 | request[7]);
			uint64_t offset = get64(request + 16);
			uint32_t length = get32(request + 24);

			if (type == CMD_DISC)
				break;
			if (type == CMD_FLUSH) {
				if (answer(s->fd, 0u, request + 8) != 0)
					break;
				continue;
			}
			if (type != CMD_READ) {
				/* A write to a read-only export; whatever else, refused. */
				if (answer(s->fd, type == 1u ? EPERM : EINVAL, request + 8) != 0)
					break;
				continue;
			}
			if (length > MOST_READ)
				break;
			if (offset > s->bytes || length > s->bytes - offset) {
				if (answer(s->fd, EINVAL, request + 8) != 0)
					break;
				continue;
			}
			if (hull_media_map_read(s->map, offset, buffer, length) != 0) {
				if (answer(s->fd, EIO, request + 8) != 0)
					break;
				continue;
			}
			if (answer(s->fd, 0u, request + 8) != 0 || full_write(s->fd, buffer, length) != 0)
				break;
		}
	}
	free(buffer);
	close(s->fd);
	hull_media_map_close(s->map);
	free(s);
	return NULL;
}

int hull_nbd_serve(hull_media_map *map, uint64_t bytes, int fd)
{
	served        *s = malloc(sizeof *s);
	pthread_t      thread;
	pthread_attr_t attr;
	int            started;

	if (s == NULL)
		return -1;
	*s = (served){ map, bytes, fd };
	if (pthread_attr_init(&attr) != 0) {
		free(s);
		return -1;
	}
	pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
	started = pthread_create(&thread, &attr, serve, s) == 0;
	pthread_attr_destroy(&attr);
	if (!started) {
		free(s);
		return -1;
	}
	return 0;
}
