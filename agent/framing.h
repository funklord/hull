/*
 * framing.h -- the guest agent protocol's framing (project.md B.2).
 *
 * A message on the vsock connection is a big-endian u32 length and then
 * that many bytes of one canonical s-expression. situ generates both
 * halves from agent/message.situ: its frame layer turns a byte stream
 * into whole messages, and its build layer writes one. This file adds
 * what the schema cannot say: that a body is one canonical s-expression,
 * which sexpr/ decides, and that an oversized length is refused as soon
 * as it arrives rather than when the buffer fills.
 *
 * Nothing allocates. The reader's buffer is the caller's and must hold
 * the largest message; HULL_AGENT_READER_SIZE is that size.
 *
 * A reader that has answered HULL_AGENT_OVERSIZE or HULL_AGENT_MALFORMED
 * has lost its place in the stream for good -- the next length would be
 * read from the middle of something -- so the connection is dropped. B.2
 * gives the agent no way to resynchronise, on purpose.
 */

#ifndef HULL_AGENT_FRAMING_H
#define HULL_AGENT_FRAMING_H

#include <stdint.h>

#include "message_frame.h"
#include "sexpr.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Bytes of length, and the most a body may hold. The schema states the
 * maximum; situ derives the whole message's from it. */
#define HULL_AGENT_LENGTH_SIZE   4u
#define HULL_AGENT_MAX_BODY      (HULL_AGENT_MESSAGE_SIZE_MAX - HULL_AGENT_LENGTH_SIZE)
#define HULL_AGENT_READER_SIZE   HULL_AGENT_MESSAGE_SIZE_MAX

typedef enum hull_agent_status {
	HULL_AGENT_OK = 0,
	HULL_AGENT_WAIT,       /* no whole message has arrived yet */
	HULL_AGENT_FULL,       /* push: no room for these bytes until a
	                          message is taken */
	HULL_AGENT_OVERSIZE,   /* a length above HULL_AGENT_MAX_BODY */
	HULL_AGENT_MALFORMED,  /* a body that is not one canonical
	                          s-expression */
	HULL_AGENT_SMALL,      /* a buffer too small for what was asked */
} hull_agent_status;

typedef struct hull_agent_reader {
	hull_agent_message_reader_t  frames;
} hull_agent_reader;

/* HULL_AGENT_SMALL where `cap` cannot hold the largest message. */
hull_agent_status hull_agent_reader_init(hull_agent_reader *reader,
                                         uint8_t *buf, uint32_t cap);

/* Append bytes as they arrive. Drops the message `next` last returned,
 * so a body from that call is dead once this is called. */
hull_agent_status hull_agent_reader_push(hull_agent_reader *reader,
                                         const uint8_t *data, uint32_t len);

/* The next whole message, its body opened as canonical s-expression.
 * `error` says why a body was malformed and may be NULL. The body points
 * into the reader's buffer and dies at the next push, next or advance. */
hull_agent_status hull_agent_reader_next(hull_agent_reader *reader,
                                         hull_sexpr_doc *body,
                                         hull_sexpr_error *error);

/* Drop the message `next` returned. */
void hull_agent_reader_advance(hull_agent_reader *reader);

/* Write one message: the length, then `body`, which must be one canonical
 * s-expression. `*wrote` is the message's size on HULL_AGENT_OK. */
hull_agent_status hull_agent_frame(uint8_t *out, uint32_t cap,
                                   const uint8_t *body, uint32_t body_len,
                                   uint32_t *wrote);

#ifdef __cplusplus
}
#endif

#endif /* HULL_AGENT_FRAMING_H */
