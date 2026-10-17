/*
 * wire.h -- a protocol of hull's on a socket: the frontend protocol
 * (project.md section 4.10), or the guest agent's (Appendix B).
 *
 * Framing is the guest agent's (agent/framing.h), and every message read
 * is held to core/frontend.situ by situ's generated validator for its
 * direction before anything reads it. A message is written with a small
 * builder that appends canonical atoms and raw bytes; the entries of each
 * message are written in their canonical order by the code that builds
 * it, and the reader on the other side refuses anything else.
 */

#ifndef HULL_WIRE_H
#define HULL_WIRE_H

#include <stddef.h>
#include <stdint.h>

#include "framing.h"
#include "sexpr.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Which side's messages a connection reads. */
typedef enum hull_wire_from {
	HULL_WIRE_FROM_CLIENT,  /* the core reads what a frontend sends */
	HULL_WIRE_FROM_CORE,    /* a frontend reads what the core sends */
	HULL_WIRE_FROM_HOST,    /* the guest agent reads what the host sends */
	HULL_WIRE_FROM_AGENT,   /* the host reads what the agent sends */
} hull_wire_from;

typedef struct hull_wire {
	int                fd;
	hull_wire_from     from;
	hull_agent_reader  reader;
	uint8_t           *buffer;
	int                pending;  /* a message has been pushed and not taken */
} hull_wire;

typedef enum hull_wire_status {
	HULL_WIRE_OK = 0,
	HULL_WIRE_WAIT,    /* nothing whole yet */
	HULL_WIRE_CLOSED,  /* the other end closed */
	HULL_WIRE_BROKEN,  /* a frame or message refused: drop the connection */
} hull_wire_status;

/* Returns 0, or -1 if memory ran out. */
int  hull_wire_open(hull_wire *wire, int fd, hull_wire_from from);
void hull_wire_close(hull_wire *wire);

/* Read once from the socket, as much as it has, without blocking for
 * more than what poll() said was there. */
hull_wire_status hull_wire_fill(hull_wire *wire);

/* The next whole message, validated and opened. The body dies at the
 * next call on this wire. */
hull_wire_status hull_wire_next(hull_wire *wire, hull_sexpr_doc *body);

/* Block until a whole message, then as hull_wire_next(). EINTR returns
 * HULL_WIRE_WAIT, so a caller can look at its signals. */
hull_wire_status hull_wire_read(hull_wire *wire, hull_sexpr_doc *body);

/* --- building a message -------------------------------------------------- */

typedef struct hull_msg {
	uint8_t *data;
	uint32_t length;
	uint32_t cap;
	int      failed;
} hull_msg;

void hull_msg_raw(hull_msg *m, const char *text);
/* Bytes already canonical, as they are: a plan, say. */
void hull_msg_bytes(hull_msg *m, const uint8_t *bytes, size_t length);
void hull_msg_atom(hull_msg *m, const uint8_t *bytes, size_t length);
void hull_msg_text(hull_msg *m, const char *text);
void hull_msg_number(hull_msg *m, long long n);
/* `(2:ch<n>)`. */
void hull_msg_channel(hull_msg *m, uint32_t ch);
void hull_msg_free(hull_msg *m);

/* Frame and write `m` whole, blocking. Returns 0, or -1. */
int hull_wire_send(int fd, const hull_msg *m);

/* --- reading a message --------------------------------------------------- */

/* The message's type, `(type ...)`: 1 if it is `type`. */
int hull_msg_is(const hull_sexpr_doc *body, const char *type);

/* The entry `(key value)` of the message: its value. */
int hull_msg_find(const hull_sexpr_doc *body, const char *key, hull_sexpr_form *value);

/* A value as a C string into `out`, or 0 if it does not fit or holds a NUL. */
int hull_msg_string(const hull_sexpr_doc *body, const hull_sexpr_form *value, char *out,
                    size_t cap);

/* A value as a number, or 0 if it is not minimal decimal. */
int hull_msg_long(const hull_sexpr_doc *body, const hull_sexpr_form *value, long long *out);

#ifdef __cplusplus
}
#endif

#endif /* HULL_WIRE_H */
