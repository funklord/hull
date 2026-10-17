/*
 * wire.c -- the frontend protocol, or the agent's, on a socket.
 */

#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "frontend_view.h"
#include "protocol_view.h"
#include "wire.h"

int hull_wire_open(hull_wire *wire, int fd, hull_wire_from from)
{
	memset(wire, 0, sizeof *wire);
	wire->fd     = fd;
	wire->from   = from;
	wire->buffer = malloc(HULL_AGENT_READER_SIZE);
	if (wire->buffer == NULL)
		return -1;
	if (hull_agent_reader_init(&wire->reader, wire->buffer, HULL_AGENT_READER_SIZE) !=
	    HULL_AGENT_OK) {
		free(wire->buffer);
		wire->buffer = NULL;
		return -1;
	}
	return 0;
}

void hull_wire_close(hull_wire *wire)
{
	if (wire->fd >= 0)
		close(wire->fd);
	free(wire->buffer);
	wire->buffer = NULL;
	wire->fd     = -1;
}

hull_wire_status hull_wire_fill(hull_wire *wire)
{
	uint8_t chunk[16384];
	ssize_t n = read(wire->fd, chunk, sizeof chunk);

	if (n < 0 && (errno == EINTR || errno == EAGAIN))
		return HULL_WIRE_WAIT;
	if (n < 0)
		return HULL_WIRE_BROKEN;
	if (n == 0)
		return HULL_WIRE_CLOSED;
	/* A push drops the message last handed out, which is dead by now. */
	if (hull_agent_reader_push(&wire->reader, chunk, (uint32_t)n) != HULL_AGENT_OK)
		return HULL_WIRE_BROKEN;
	wire->pending = 1;
	return HULL_WIRE_OK;
}

/* situ's verdict on a body, for the direction this wire reads. */
static int valid(const hull_wire *wire, const hull_sexpr_doc *body)
{
	situ_msg_t  msg;
	situ_view_t view;

	situ_msg_init(&msg, (uint8_t *)(uintptr_t)body->data, body->length);
	switch (wire->from) {
	case HULL_WIRE_FROM_CLIENT:
		return hull_frontend_client_message_view(&msg, 0u, body->length, &view) == SITU_OK &&
		       hull_frontend_client_message_validate(view) == SITU_OK;
	case HULL_WIRE_FROM_CORE:
		return hull_frontend_core_message_view(&msg, 0u, body->length, &view) == SITU_OK &&
		       hull_frontend_core_message_validate(view) == SITU_OK;
	case HULL_WIRE_FROM_HOST:
		return hull_protocol_host_message_view(&msg, 0u, body->length, &view) == SITU_OK &&
		       hull_protocol_host_message_validate(view) == SITU_OK;
	case HULL_WIRE_FROM_AGENT:
		return hull_protocol_agent_message_view(&msg, 0u, body->length, &view) == SITU_OK &&
		       hull_protocol_agent_message_validate(view) == SITU_OK;
	}
	return 0;
}

hull_wire_status hull_wire_next(hull_wire *wire, hull_sexpr_doc *body)
{
	hull_agent_status status = hull_agent_reader_next(&wire->reader, body, NULL);

	if (status == HULL_AGENT_WAIT)
		return HULL_WIRE_WAIT;
	if (status != HULL_AGENT_OK || !valid(wire, body))
		return HULL_WIRE_BROKEN;
	return HULL_WIRE_OK;
}

hull_wire_status hull_wire_read(hull_wire *wire, hull_sexpr_doc *body)
{
	for (;;) {
		hull_wire_status status = hull_wire_next(wire, body);

		if (status != HULL_WIRE_WAIT)
			return status;
		status = hull_wire_fill(wire);
		if (status != HULL_WIRE_OK)
			return status;
	}
}

/* --- building ------------------------------------------------------------ */

static void add(hull_msg *m, const void *bytes, size_t n)
{
	if (m->failed)
		return;
	if ((size_t)m->length + n > HULL_AGENT_MAX_BODY) {
		m->failed = 1;
		return;
	}
	if (m->length + n > m->cap) {
		uint32_t cap = m->cap == 0u ? 256u : m->cap;
		uint8_t *data;

		while (cap < m->length + n)
			cap *= 2u;
		data = realloc(m->data, cap);
		if (data == NULL) {
			m->failed = 1;
			return;
		}
		m->data = data;
		m->cap  = cap;
	}
	memcpy(m->data + m->length, bytes, n);
	m->length += (uint32_t)n;
}

void hull_msg_raw(hull_msg *m, const char *text)
{
	add(m, text, strlen(text));
}

void hull_msg_bytes(hull_msg *m, const uint8_t *bytes, size_t length)
{
	add(m, bytes, length);
}

void hull_msg_atom(hull_msg *m, const uint8_t *bytes, size_t length)
{
	char prefix[24];

	snprintf(prefix, sizeof prefix, "%zu:", length);
	hull_msg_raw(m, prefix);
	add(m, bytes, length);
}

void hull_msg_text(hull_msg *m, const char *text)
{
	hull_msg_atom(m, (const uint8_t *)text, strlen(text));
}

void hull_msg_number(hull_msg *m, long long n)
{
	char digits[24];

	snprintf(digits, sizeof digits, "%lld", n);
	hull_msg_text(m, digits);
}

void hull_msg_channel(hull_msg *m, uint32_t ch)
{
	hull_msg_raw(m, "(2:ch");
	hull_msg_number(m, ch);
	hull_msg_raw(m, ")");
}

void hull_msg_free(hull_msg *m)
{
	free(m->data);
	memset(m, 0, sizeof *m);
}

int hull_wire_send(int fd, const hull_msg *m)
{
	uint8_t  *frame;
	uint32_t  wrote = 0u;
	uint32_t  at    = 0u;

	if (m->failed || m->data == NULL)
		return -1;
	frame = malloc((size_t)m->length + HULL_AGENT_LENGTH_SIZE);
	if (frame == NULL)
		return -1;
	if (hull_agent_frame(frame, m->length + HULL_AGENT_LENGTH_SIZE, m->data, m->length,
	                     &wrote) != HULL_AGENT_OK) {
		free(frame);
		return -1;
	}
	while (at < wrote) {
		ssize_t n = write(fd, frame + at, wrote - at);

		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0) {
			free(frame);
			return -1;
		}
		at += (uint32_t)n;
	}
	free(frame);
	return 0;
}

/* --- reading ------------------------------------------------------------- */

int hull_msg_is(const hull_sexpr_doc *body, const char *type)
{
	hull_sexpr_form message;
	hull_sexpr_form head;

	return hull_sexpr_first(body, &message) && hull_sexpr_child(body, &message, &head) &&
	       hull_sexpr_is(body, &head, type);
}

int hull_msg_find(const hull_sexpr_doc *body, const char *key, hull_sexpr_form *value)
{
	hull_sexpr_form message;
	hull_sexpr_form item;
	int             more;

	if (!hull_sexpr_first(body, &message) || !hull_sexpr_child(body, &message, &item))
		return 0;
	for (more = hull_sexpr_next(body, &item, &item); more;
	     more = hull_sexpr_next(body, &item, &item)) {
		hull_sexpr_form name;

		if (item.kind == HULL_SEXPR_LIST && hull_sexpr_child(body, &item, &name) &&
		    hull_sexpr_is(body, &name, key)) {
			/* A list value -- `plan` -- is the entry itself. */
			if (!hull_sexpr_next(body, &name, value) || value->kind == HULL_SEXPR_LIST)
				*value = item;
			return 1;
		}
	}
	return 0;
}

int hull_msg_string(const hull_sexpr_doc *body, const hull_sexpr_form *value, char *out,
                    size_t cap)
{
	if (value->kind != HULL_SEXPR_ATOM || value->content_length >= cap ||
	    memchr(body->data + value->content, '\0', value->content_length) != NULL)
		return 0;
	memcpy(out, body->data + value->content, value->content_length);
	out[value->content_length] = '\0';
	return 1;
}

int hull_msg_long(const hull_sexpr_doc *body, const hull_sexpr_form *value, long long *out)
{
	char  text[24];
	char *end;
	int   negative;

	if (!hull_msg_string(body, value, text, sizeof text) || text[0] == '\0')
		return 0;
	negative = text[0] == '-';
	/* Minimal decimal: no leading zero, no "-0", no plus. */
	if ((text[negative] == '0' && text[negative + 1] != '\0') ||
	    (negative && text[1] == '0') || text[negative] < '0' || text[negative] > '9')
		return 0;
	errno = 0;
	*out = strtoll(text, &end, 10);
	return errno == 0 && *end == '\0';
}
