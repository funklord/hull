/*
 * sexpr.c -- the walk situ leaves to its caller, and decoding an atom.
 *
 * situ frames one form at a time: its kind, its extent, whether a string, a
 * comment or octets reached their closing byte, and whether a canonical
 * length is minimal with its bytes inside the buffer. It deliberately does
 * not validate a list's elements, and does not check that a list reached
 * its `)` -- a generated `_list_check` says so in a comment. So the walk
 * below visits every form, with an explicit stack rather than recursion,
 * and asks situ to frame each one as it goes.
 *
 * The walk is also a second witness. Where situ and the walk disagree about
 * where a list ends, the document is refused rather than believed: that is
 * how a framing fault in situ, or in the schemas, reaches a test instead of
 * a caller.
 */

#include <string.h>

#include "sexpr.h"
#include "readable.h"
#include "canonical.h"

/* situ counts nesting in structs, two per level of lists. */
#define SITU_NESTING_LIMIT (2u * HULL_SEXPR_MAX_DEPTH)

static int is_space(uint8_t byte)
{
	return byte == ' ' || byte == '\t' || byte == '\r' || byte == '\n';
}

static int hex_value(uint8_t byte)
{
	if (byte >= '0' && byte <= '9')
		return byte - '0';
	if (byte >= 'a' && byte <= 'f')
		return byte - 'a' + 10;
	if (byte >= 'A' && byte <= 'F')
		return byte - 'A' + 10;
	return -1;
}

/* situ binds a message to a writable buffer, for setters a reader never
 * calls. This is the one place hull's const meets that. */
static void bind(const hull_sexpr_doc *doc, situ_msg_t *msg)
{
	situ_msg_init(msg, (uint8_t *)(uintptr_t)doc->data, doc->length);
}

void hull_sexpr_locate(const hull_sexpr_doc *doc, uint32_t offset,
                       uint32_t *line, uint32_t *column)
{
	*line   = 1u;
	*column = 1u;
	for (uint32_t i = 0; i < offset && i < doc->length; i++) {
		if (doc->data[i] == '\n') {
			(*line)++;
			*column = 1u;
		} else {
			(*column)++;
		}
	}
}

static void locate(const hull_sexpr_doc *doc, hull_sexpr_error *error,
                   hull_sexpr_status status, uint32_t offset, const char *reason)
{
	if (error == NULL)
		return;
	hull_sexpr_locate(doc, offset, &error->line, &error->column);
	error->status = status;
	error->offset = offset;
	error->reason = reason;
}

/* Step over whitespace and comments. Readable syntax only: canonical form
 * has neither, and a stray space there is refused as the start of an atom. */
static const char *skip(const hull_sexpr_doc *doc, uint32_t *at)
{
	while (*at < doc->length) {
		situ_msg_t  msg;
		situ_view_t view;
		uint32_t    extent;

		if (is_space(doc->data[*at])) {
			(*at)++;
			continue;
		}
		if (doc->data[*at] != ';')
			return NULL;
		bind(doc, &msg);
		if (hull_sexpr_r_comment_view(&msg, *at, doc->length - *at, &view) != SITU_OK ||
		    hull_sexpr_r_comment_check(view, NULL) != SITU_OK)
			return "a comment must end with a newline";
		extent = hull_sexpr_r_comment_extent(view);
		if (extent == 0u)
			return "a comment of no length";
		*at += extent;
	}
	return NULL;
}

/* Frame the form starting at `at`, which is neither a lead nor a `)`. */
static hull_sexpr_status frame_readable(const hull_sexpr_doc *doc, uint32_t at,
                                        hull_sexpr_form *out, const char **reason)
{
	situ_msg_t  msg;
	situ_view_t view;
	situ_view_t arm;
	uint8_t     kind = doc->data[at];

	bind(doc, &msg);
	if (hull_sexpr_r_form_view(&msg, at, doc->length - at, &view) != SITU_OK) {
		*reason = "a form that does not fit in the input";
		return HULL_SEXPR_ERR_SYNTAX;
	}
	if (hull_sexpr_r_form_nesting(view) > SITU_NESTING_LIMIT) {
		*reason = "lists nested deeper than the maximum";
		return HULL_SEXPR_ERR_DEPTH;
	}
	if (hull_sexpr_r_form_check(view, NULL) != SITU_OK) {
		*reason = kind == '"' ? "a string that is not closed"
		        : kind == '#' ? "octets that are not closed with `#`"
		        : kind == '(' ? "a malformed list"
		        :               "a malformed symbol";
		return HULL_SEXPR_ERR_SYNTAX;
	}
	out->offset = at;
	out->extent = hull_sexpr_r_form_extent(view);
	if (out->extent == 0u || out->extent > doc->length - at) {
		*reason = "a form whose extent situ could not settle";
		return HULL_SEXPR_ERR_SYNTAX;
	}
	switch (kind) {
	case '(':
		out->kind           = HULL_SEXPR_LIST;
		out->content        = at + 1u;
		out->content_length = 0u;
		return HULL_SEXPR_OK;
	case '"':
		if (hull_sexpr_r_form_body_as_text_view(view, &arm) != SITU_OK)
			break;
		out->kind           = HULL_SEXPR_STRING;
		out->content        = (uint32_t)(hull_sexpr_r_text_chars_ptr(arm) - doc->data);
		out->content_length = hull_sexpr_r_text_chars_len(arm);
		return HULL_SEXPR_OK;
	case '#':
		if (hull_sexpr_r_form_body_as_octets_view(view, &arm) != SITU_OK)
			break;
		out->kind           = HULL_SEXPR_OCTETS;
		out->content        = (uint32_t)(hull_sexpr_r_octets_digits_ptr(arm) - doc->data);
		out->content_length = hull_sexpr_r_octets_digits_len(arm);
		return HULL_SEXPR_OK;
	default:
		if (hull_sexpr_r_form_body_as_symbol_view(view, &arm) != SITU_OK)
			break;
		out->kind           = HULL_SEXPR_SYMBOL;
		out->content        = (uint32_t)(hull_sexpr_r_symbol_name_ptr(arm) - doc->data);
		out->content_length = hull_sexpr_r_symbol_name_len(arm);
		if (out->content_length == 0u) {
			*reason = "a symbol of no length";
			return HULL_SEXPR_ERR_SYNTAX;
		}
		return HULL_SEXPR_OK;
	}
	*reason = "a form situ framed and would not open";
	return HULL_SEXPR_ERR_SYNTAX;
}

static hull_sexpr_status frame_canonical(const hull_sexpr_doc *doc, uint32_t at,
                                         hull_sexpr_form *out, const char **reason)
{
	situ_msg_t  msg;
	situ_view_t view;
	situ_view_t arm;

	bind(doc, &msg);
	if (hull_sexpr_c_form_view(&msg, at, doc->length - at, &view) != SITU_OK) {
		*reason = "a form that does not fit in the input";
		return HULL_SEXPR_ERR_SYNTAX;
	}
	if (hull_sexpr_c_form_nesting(view) > SITU_NESTING_LIMIT) {
		*reason = "lists nested deeper than the maximum";
		return HULL_SEXPR_ERR_DEPTH;
	}
	if (hull_sexpr_c_form_check(view, NULL) != SITU_OK) {
		*reason = doc->data[at] == '(' ? "a malformed list"
		        : "an atom whose length is not minimal decimal, or whose bytes run past the input";
		return HULL_SEXPR_ERR_SYNTAX;
	}
	out->offset = at;
	out->extent = hull_sexpr_c_form_extent(view);
	if (out->extent == 0u || out->extent > doc->length - at) {
		*reason = "a form whose extent situ could not settle";
		return HULL_SEXPR_ERR_SYNTAX;
	}
	if (doc->data[at] == '(') {
		out->kind           = HULL_SEXPR_LIST;
		out->content        = at + 1u;
		out->content_length = 0u;
		return HULL_SEXPR_OK;
	}
	if (hull_sexpr_c_form_body_as_atom_view(view, &arm) != SITU_OK) {
		*reason = "a form situ framed and would not open";
		return HULL_SEXPR_ERR_SYNTAX;
	}
	out->kind           = HULL_SEXPR_ATOM;
	out->content        = (uint32_t)(hull_sexpr_c_atom_bytes_ptr(arm) - doc->data);
	out->content_length = hull_sexpr_c_atom_bytes_len(arm);
	return HULL_SEXPR_OK;
}

static hull_sexpr_status frame(const hull_sexpr_doc *doc, uint32_t at, uint32_t end,
                               hull_sexpr_form *out, const char **reason)
{
	hull_sexpr_status status;

	status = doc->syntax == HULL_SEXPR_READABLE
	       ? frame_readable(doc, at, out, reason)
	       : frame_canonical(doc, at, out, reason);
	out->end = end;
	return status;
}

/* What situ does not judge inside a string or octets: which escapes exist,
 * and whether the digits are hexadecimal and pair up. Returns the offset
 * of the first bad byte, or the form's end if there is none. */
static uint32_t check_content(const hull_sexpr_doc *doc, const hull_sexpr_form *form,
                              const char **reason)
{
	const uint8_t *text = doc->data + form->content;
	uint32_t       n    = form->content_length;

	if (form->kind == HULL_SEXPR_STRING) {
		for (uint32_t i = 0; i < n; i++) {
			if (text[i] != '\\')
				continue;
			if (i + 1u == n || strchr("\\\"nt", text[i + 1u]) == NULL ||
			    text[i + 1u] == '\0') {
				*reason = "an escape other than \\\\, \\\", \\n or \\t";
				return form->content + i;
			}
			i++;
		}
	} else if (form->kind == HULL_SEXPR_OCTETS) {
		for (uint32_t i = 0; i < n; i++) {
			if (hex_value(text[i]) < 0) {
				*reason = "octets holding a byte that is not a hexadecimal digit";
				return form->content + i;
			}
		}
		if (n % 2u != 0u) {
			*reason = "octets with an odd number of hexadecimal digits";
			return form->content;
		}
	}
	return form->offset + form->extent;
}

hull_sexpr_status hull_sexpr_open(hull_sexpr_doc *doc, const uint8_t *data,
                                  size_t length, hull_sexpr_syntax syntax,
                                  hull_sexpr_error *error)
{
	uint32_t          open_at[HULL_SEXPR_MAX_DEPTH];
	uint32_t          depth = 0u;
	uint32_t          at    = 0u;
	uint32_t          top   = 0u;
	const char       *reason;
	hull_sexpr_status status;

	doc->data   = data;
	doc->length = 0u;
	doc->syntax = syntax;
	if (length > UINT32_MAX) {
		locate(doc, error, HULL_SEXPR_ERR_SIZE, 0u, "a document larger than 4 GiB");
		return HULL_SEXPR_ERR_SIZE;
	}
	doc->length = (uint32_t)length;

	for (;;) {
		hull_sexpr_form form;
		uint32_t        bad;

		if (syntax == HULL_SEXPR_READABLE && (reason = skip(doc, &at)) != NULL) {
			locate(doc, error, HULL_SEXPR_ERR_SYNTAX, at, reason);
			return HULL_SEXPR_ERR_SYNTAX;
		}
		if (at == doc->length) {
			if (depth > 0u) {
				locate(doc, error, HULL_SEXPR_ERR_SYNTAX, open_at[depth - 1u],
				       "a list that is not closed");
				return HULL_SEXPR_ERR_SYNTAX;
			}
			if (syntax == HULL_SEXPR_CANONICAL && top == 0u) {
				locate(doc, error, HULL_SEXPR_ERR_SYNTAX, at,
				       "canonical form holds exactly one form, and this holds none");
				return HULL_SEXPR_ERR_SYNTAX;
			}
			return HULL_SEXPR_OK;
		}
		if (doc->data[at] == ')') {
			uint32_t list_at;

			if (depth == 0u) {
				locate(doc, error, HULL_SEXPR_ERR_SYNTAX, at,
				       "a `)` with no list open");
				return HULL_SEXPR_ERR_SYNTAX;
			}
			list_at = open_at[--depth];
			/* The second witness: situ's extent for this list against
			 * the `)` the walk found. */
			if (frame(doc, list_at, doc->length, &form, &reason) != HULL_SEXPR_OK ||
			    form.extent != at + 1u - list_at) {
				locate(doc, error, HULL_SEXPR_ERR_SYNTAX, list_at,
				       "situ and the reader disagree about where this list ends");
				return HULL_SEXPR_ERR_SYNTAX;
			}
			at++;
			continue;
		}
		if (depth == 0u && syntax == HULL_SEXPR_CANONICAL && top > 0u) {
			locate(doc, error, HULL_SEXPR_ERR_SYNTAX, at,
			       "canonical form holds exactly one form, and more follows it");
			return HULL_SEXPR_ERR_SYNTAX;
		}
		if (depth == 0u)
			top++;

		status = frame(doc, at, doc->length, &form, &reason);
		if (status != HULL_SEXPR_OK) {
			locate(doc, error, status, at, reason);
			return status;
		}
		if (form.kind == HULL_SEXPR_LIST) {
			if (depth == HULL_SEXPR_MAX_DEPTH) {
				locate(doc, error, HULL_SEXPR_ERR_DEPTH, at,
				       "lists nested deeper than the maximum");
				return HULL_SEXPR_ERR_DEPTH;
			}
			open_at[depth++] = at;
			at++;
			continue;
		}
		bad = check_content(doc, &form, &reason);
		if (bad != form.offset + form.extent) {
			locate(doc, error, HULL_SEXPR_ERR_SYNTAX, bad, reason);
			return HULL_SEXPR_ERR_SYNTAX;
		}
		at += form.extent;
	}
}

/* The form at or after `at`, before `end`; 0 at the end of the run. */
static int form_at(const hull_sexpr_doc *doc, uint32_t at, uint32_t end,
                   hull_sexpr_form *out)
{
	const char *reason;

	if (doc->syntax == HULL_SEXPR_READABLE && skip(doc, &at) != NULL)
		return 0;
	if (at >= end || doc->data[at] == ')')
		return 0;
	return frame(doc, at, end, out, &reason) == HULL_SEXPR_OK;
}

int hull_sexpr_first(const hull_sexpr_doc *doc, hull_sexpr_form *out)
{
	return form_at(doc, 0u, doc->length, out);
}

int hull_sexpr_next(const hull_sexpr_doc *doc, const hull_sexpr_form *form,
                    hull_sexpr_form *out)
{
	return form_at(doc, form->offset + form->extent, form->end, out);
}

int hull_sexpr_child(const hull_sexpr_doc *doc, const hull_sexpr_form *list,
                     hull_sexpr_form *out)
{
	if (list->kind != HULL_SEXPR_LIST)
		return 0;
	/* A list's own `)` is the last byte of its extent. */
	return form_at(doc, list->offset + 1u, list->offset + list->extent - 1u, out);
}

int hull_sexpr_is(const hull_sexpr_doc *doc, const hull_sexpr_form *form,
                  const char *name)
{
	size_t n = strlen(name);

	if (form->kind != HULL_SEXPR_SYMBOL && form->kind != HULL_SEXPR_ATOM)
		return 0;
	return form->content_length == n &&
	       memcmp(doc->data + form->content, name, n) == 0;
}

hull_sexpr_status hull_sexpr_value(const hull_sexpr_doc *doc,
                                   const hull_sexpr_form *form, uint8_t *out,
                                   size_t capacity, size_t *length)
{
	const uint8_t *text = doc->data + form->content;
	uint32_t       n    = form->content_length;
	size_t         used = 0u;

	switch (form->kind) {
	case HULL_SEXPR_LIST:
		*length = 0u;
		return HULL_SEXPR_ERR_KIND;
	case HULL_SEXPR_SYMBOL:
	case HULL_SEXPR_ATOM:
		used = n;
		if (used <= capacity)
			memcpy(out, text, used);
		break;
	case HULL_SEXPR_STRING:
		/* hull_sexpr_open() has refused every escape but these four. */
		for (uint32_t i = 0; i < n; i++) {
			uint8_t byte = text[i];

			if (byte == '\\') {
				i++;
				byte = text[i] == 'n' ? '\n' : text[i] == 't' ? '\t' : text[i];
			}
			if (used < capacity)
				out[used] = byte;
			used++;
		}
		break;
	case HULL_SEXPR_OCTETS:
		for (uint32_t i = 0; i + 1u < n; i += 2u) {
			if (used < capacity)
				out[used] = (uint8_t)(hex_value(text[i]) << 4 | hex_value(text[i + 1u]));
			used++;
		}
		break;
	}
	*length = used;
	return used <= capacity ? HULL_SEXPR_OK : HULL_SEXPR_ERR_SPACE;
}
