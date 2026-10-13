/*
 * convert.c -- a readable form, written in canonical form (A.1).
 *
 * "The readable form parses to the canonical one with no help": a symbol,
 * a string and octets all become an atom of their bytes, and a list stays
 * a list. The one thing A.1 asks beyond that is order, and the shape
 * decides it. A list whose elements after the first are all lists opening
 * with an atom, no two the same, is a record, and its entries are sorted
 * bytewise by key. Any other list keeps its order: arguments, and plural
 * lists like `(ports (port ...) (port ...))`, whose items share a key.
 *
 * Everything goes through the writer, so every piece is read back as it
 * is made. Each form is built in `scratch` and moved into place: a list's
 * elements are written first, the list after them, and the list then
 * moved down over its elements, since the writer refuses to read and
 * write the same bytes.
 */

#include <string.h>

#include "sexpr.h"

typedef struct convert {
	const hull_sexpr_doc *doc;
	uint8_t              *scratch;
	uint32_t              cap;
	uint32_t              top;  /* the end of what is built */
} convert;

/* Write one form at `top`, and move `top` past it. */
static hull_sexpr_status emit(convert *c, const hull_sexpr_form *form);

/* A form built at `[from, top)` moved down to `to`, `top` following. */
static void settle(convert *c, uint32_t to, uint32_t from)
{
	uint32_t length = c->top - from;

	memmove(c->scratch + to, c->scratch + from, length);
	c->top = to + length;
}

static hull_sexpr_status emit_atom(convert *c, const hull_sexpr_form *form)
{
	uint32_t          start = c->top;
	size_t            length;
	hull_sexpr_status status;

	/* The value's bytes first, then the atom after them. */
	status = hull_sexpr_value(c->doc, form, c->scratch + start, c->cap - start, &length);
	if (status != HULL_SEXPR_OK)
		return status;
	c->top = start + (uint32_t)length;
	status = hull_sexpr_put_atom(c->scratch, c->cap, &c->top, c->scratch + start,
	                             (uint32_t)length);
	if (status != HULL_SEXPR_OK)
		return status;
	settle(c, start, start + (uint32_t)length);
	return HULL_SEXPR_OK;
}

/* Whether `list`'s elements after the first are all lists opening with
 * an atom, and there is at least one: a record, if the keys differ. */
static int record_shaped(const convert *c, const hull_sexpr_form *list)
{
	hull_sexpr_form item;
	hull_sexpr_form key;
	int             more;
	int             any = 0;

	if (!hull_sexpr_child(c->doc, list, &item) || item.kind == HULL_SEXPR_LIST)
		return 0;
	for (more = hull_sexpr_next(c->doc, &item, &item); more;
	     more = hull_sexpr_next(c->doc, &item, &item)) {
		if (item.kind != HULL_SEXPR_LIST || !hull_sexpr_child(c->doc, &item, &key) ||
		    key.kind == HULL_SEXPR_LIST)
			return 0;
		any = 1;
	}
	return any;
}

static hull_sexpr_status emit_list(convert *c, const hull_sexpr_form *list)
{
	uint32_t          start = c->top;
	uint32_t          entries;
	uint32_t          end;
	hull_sexpr_form   item;
	hull_sexpr_status status;
	int               more;

	more = hull_sexpr_child(c->doc, list, &item);
	if (more) {
		status = emit(c, &item);
		if (status != HULL_SEXPR_OK)
			return status;
	}
	entries = c->top;
	for (more = more && hull_sexpr_next(c->doc, &item, &item); more;
	     more = hull_sexpr_next(c->doc, &item, &item)) {
		status = emit(c, &item);
		if (status != HULL_SEXPR_OK)
			return status;
	}
	end = c->top;
	if (record_shaped(c, list)) {
		status = hull_sexpr_put_sorted(c->scratch, c->cap, &c->top, c->scratch + entries,
		                               end - entries);
		if (status == HULL_SEXPR_OK)
			settle(c, entries, end);
		else if (status == HULL_SEXPR_ERR_DUPLICATE)
			c->top = end;  /* a plural list: its order is its own */
		else
			return status;
	}
	end = c->top;
	status = hull_sexpr_put_list(c->scratch, c->cap, &c->top, c->scratch + start, end - start);
	if (status != HULL_SEXPR_OK)
		return status;
	settle(c, start, end);
	return HULL_SEXPR_OK;
}

static hull_sexpr_status emit(convert *c, const hull_sexpr_form *form)
{
	return form->kind == HULL_SEXPR_LIST ? emit_list(c, form) : emit_atom(c, form);
}

hull_sexpr_status hull_sexpr_to_canonical(const hull_sexpr_doc *doc, const hull_sexpr_form *form,
                                          uint8_t *out, uint32_t cap, uint32_t *at,
                                          uint8_t *scratch, uint32_t scratch_cap)
{
	convert           c;
	hull_sexpr_status status;

	if (out == NULL || at == NULL || *at > cap || scratch == NULL)
		return HULL_SEXPR_ERR_SPACE;
	c.doc     = doc;
	c.scratch = scratch;
	c.cap     = scratch_cap;
	c.top     = 0u;
	status = emit(&c, form);
	if (status != HULL_SEXPR_OK)
		return status;
	if (c.top > cap - *at)
		return HULL_SEXPR_ERR_SPACE;
	memcpy(out + *at, scratch, c.top);
	*at += c.top;
	return HULL_SEXPR_OK;
}
