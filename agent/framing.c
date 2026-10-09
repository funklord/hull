/*
 * framing.c -- B.2's framing over situ's frame and build layers.
 *
 * situ's reader answers "is a whole message here yet" from the length,
 * and saturates a hostile one rather than wrapping. It does not consult
 * the schema's [max] while waiting: a length above the maximum is refused
 * only once the buffer it would need exceeds the caller's, which with a
 * larger buffer means waiting for bytes that should never be read. So the
 * length is checked here the moment it is whole, against the same number
 * situ derived, and every message situ hands out is validated before its
 * body is opened.
 */

#include "framing.h"
#include "message_build.h"

hull_agent_status hull_agent_reader_init(hull_agent_reader *reader,
                                         uint8_t *buf, uint32_t cap)
{
	if (cap < HULL_AGENT_READER_SIZE)
		return HULL_AGENT_SMALL;
	hull_agent_message_reader_init(&reader->frames, buf, cap);
	return HULL_AGENT_OK;
}

hull_agent_status hull_agent_reader_push(hull_agent_reader *reader,
                                         const uint8_t *data, uint32_t len)
{
	return hull_agent_message_reader_push(&reader->frames, data, len) == SITU_OK
	       ? HULL_AGENT_OK : HULL_AGENT_FULL;
}

void hull_agent_reader_advance(hull_agent_reader *reader)
{
	hull_agent_message_reader_advance(&reader->frames);
}

hull_agent_status hull_agent_reader_next(hull_agent_reader *reader,
                                         hull_sexpr_doc *body,
                                         hull_sexpr_error *error)
{
	hull_agent_message_reader_t *frames = &reader->frames;
	situ_msg_t                   msg;
	situ_view_t                  view;
	uint32_t                     need = 0u;
	situ_err_t                   err;

	/* The previous message goes first, as situ's own `next` does, so the
	 * length below is the next message's and not the one already read. */
	hull_agent_message_reader_advance(frames);

	/* Refuse an oversized length as soon as it is whole. situ answers
	 * `need` once the length has arrived, saturated if it would wrap. */
	err = hull_agent_message_required(frames->buf, frames->have, &need);
	if (frames->have >= HULL_AGENT_LENGTH_SIZE &&
	    need - HULL_AGENT_LENGTH_SIZE > HULL_AGENT_MAX_BODY)
		return HULL_AGENT_OVERSIZE;
	if (err == SITU_ERR_TRUNCATED)
		return HULL_AGENT_WAIT;

	err = hull_agent_message_reader_next(frames, &msg, &view);
	if (err == SITU_ERR_TRUNCATED)
		return HULL_AGENT_WAIT;
	if (err != SITU_OK)
		return HULL_AGENT_OVERSIZE;
	if (hull_agent_message_validate(view) != SITU_OK)
		return HULL_AGENT_OVERSIZE;

	if (hull_sexpr_open(body, hull_agent_message_body_ptr(view),
	                    hull_agent_message_body_len(view),
	                    HULL_SEXPR_CANONICAL, error) != HULL_SEXPR_OK)
		return HULL_AGENT_MALFORMED;
	return HULL_AGENT_OK;
}

hull_agent_status hull_agent_frame(uint8_t *out, uint32_t cap,
                                   const uint8_t *body, uint32_t body_len,
                                   uint32_t *wrote)
{
	hull_sexpr_doc doc;
	situ_err_t     err;

	if (body_len > HULL_AGENT_MAX_BODY)
		return HULL_AGENT_OVERSIZE;
	/* A peer refuses a body that is not one canonical s-expression and
	 * drops the connection, so it is refused here instead, where the
	 * fault is still the writer's to see. */
	if (hull_sexpr_open(&doc, body, body_len, HULL_SEXPR_CANONICAL, NULL) != HULL_SEXPR_OK)
		return HULL_AGENT_MALFORMED;
	err = hull_agent_message_build(out, cap, body, body_len, wrote);
	if (err == SITU_ERR_BOUNDS)
		return HULL_AGENT_SMALL;
	return err == SITU_OK ? HULL_AGENT_OK : HULL_AGENT_OVERSIZE;
}
