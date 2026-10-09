/*
 * writer.c -- canonical s-expressions, written by situ's build layer.
 *
 * situ builds an atom from its bytes, computing the length rather than
 * taking it, and a list from elements already encoded, walking them with
 * each form's own `_required` so that they are a whole number of forms.
 * What it does not do is validate the elements: a generated list
 * validates the list and leaves its elements to the caller, as it does
 * when reading. So everything written here is read back with
 * hull_sexpr_open(), whose walk visits every element, and only then is
 * `*at` moved. The same walk is what refuses a list nested deeper than the
 * reader accepts.
 *
 * Ordering a record's entries is hull's (A.1): a layout says nothing about
 * the order of the elements of a run. hull_sexpr_put_sorted() places them.
 */

#include <string.h>

#include "sexpr.h"
#include "canonical_build.h"

static hull_sexpr_status from_situ(situ_err_t err)
{
	if (err == SITU_OK)
		return HULL_SEXPR_OK;
	return err == SITU_ERR_BOUNDS ? HULL_SEXPR_ERR_SPACE : HULL_SEXPR_ERR_SYNTAX;
}

/* 1 if [a, a + a_len) and [b, b + b_len) share a byte. Compared as
 * integers, since the two may be different objects. */
static int overlaps(const uint8_t *a, uint32_t a_len, const uint8_t *b, uint32_t b_len)
{
	uintptr_t a0 = (uintptr_t)a;
	uintptr_t b0 = (uintptr_t)b;

	if (a_len == 0u || b_len == 0u)
		return 0;
	return a0 < b0 + b_len && b0 < a0 + a_len;
}

/* Read back what was written at `*at`, and move `*at` past it. */
static hull_sexpr_status read_back(const uint8_t *out, uint32_t *at, uint32_t wrote)
{
	hull_sexpr_doc    doc;
	hull_sexpr_status status;

	status = hull_sexpr_open(&doc, out + *at, wrote, HULL_SEXPR_CANONICAL, NULL);
	if (status != HULL_SEXPR_OK)
		return status;
	*at += wrote;
	return HULL_SEXPR_OK;
}

hull_sexpr_status hull_sexpr_put_atom(uint8_t *out, uint32_t cap, uint32_t *at,
                                      const uint8_t *bytes, uint32_t length)
{
	uint32_t   wrote = 0u;
	situ_err_t err;

	if (out == NULL || at == NULL || *at > cap)
		return HULL_SEXPR_ERR_SPACE;
	if (overlaps(bytes, length, out + *at, cap - *at))
		return HULL_SEXPR_ERR_SPACE;
	err = hull_sexpr_c_atom_build(out + *at, cap - *at, bytes, length, &wrote);
	if (err != SITU_OK)
		return from_situ(err);
	return read_back(out, at, wrote);
}

hull_sexpr_status hull_sexpr_put_list(uint8_t *out, uint32_t cap, uint32_t *at,
                                      const uint8_t *items, uint32_t length)
{
	uint32_t   wrote = 0u;
	situ_err_t err;

	if (out == NULL || at == NULL || *at > cap)
		return HULL_SEXPR_ERR_SPACE;
	if (overlaps(items, length, out + *at, cap - *at))
		return HULL_SEXPR_ERR_SPACE;
	err = hull_sexpr_c_list_build(out + *at, cap - *at, '(', items, length, &wrote);
	if (err != SITU_OK)
		return from_situ(err);
	return read_back(out, at, wrote);
}

/* One entry of a record: where it is, and its key. */
typedef struct entry {
	uint32_t        offset;
	uint32_t        extent;
	const uint8_t  *key;
	uint32_t        key_length;
} entry;

/* The entry starting at `at`, which must be a list opening with an atom. */
static hull_sexpr_status entry_at(const uint8_t *entries, uint32_t length,
                                  uint32_t at, entry *out)
{
	hull_sexpr_doc    doc;
	hull_sexpr_form   list;
	hull_sexpr_form   key;
	hull_sexpr_status status;
	uint32_t          need = 0u;

	if (hull_sexpr_c_form_required(entries + at, length - at, &need) != SITU_OK ||
	    need == 0u || need > length - at)
		return HULL_SEXPR_ERR_SYNTAX;
	status = hull_sexpr_open(&doc, entries + at, need, HULL_SEXPR_CANONICAL, NULL);
	if (status != HULL_SEXPR_OK)
		return status;
	if (!hull_sexpr_first(&doc, &list) || list.kind != HULL_SEXPR_LIST ||
	    !hull_sexpr_child(&doc, &list, &key) || key.kind != HULL_SEXPR_ATOM)
		return HULL_SEXPR_ERR_KIND;
	out->offset     = at;
	out->extent     = need;
	out->key        = entries + at + key.content;
	out->key_length = key.content_length;
	return HULL_SEXPR_OK;
}

/* Bytewise, a key that is a prefix of another sorting first. */
static int compare(const entry *a, const entry *b)
{
	uint32_t n = a->key_length < b->key_length ? a->key_length : b->key_length;
	int      c = n == 0u ? 0 : memcmp(a->key, b->key, n);

	if (c != 0)
		return c;
	if (a->key_length == b->key_length)
		return 0;
	return a->key_length < b->key_length ? -1 : 1;
}

hull_sexpr_status hull_sexpr_put_sorted(uint8_t *out, uint32_t cap, uint32_t *at,
                                        const uint8_t *entries, uint32_t length)
{
	hull_sexpr_status status;
	entry             last  = { 0u, 0u, NULL, 0u };
	uint32_t          count = 0u;
	uint32_t          wrote = 0u;

	if (out == NULL || at == NULL || *at > cap)
		return HULL_SEXPR_ERR_SPACE;
	if (length != 0u && entries == NULL)
		return HULL_SEXPR_ERR_SYNTAX;
	if (overlaps(entries, length, out + *at, cap - *at))
		return HULL_SEXPR_ERR_SPACE;

	/* Every entry is checked before anything is placed. */
	for (uint32_t scan = 0u; scan < length; count++) {
		entry e;

		status = entry_at(entries, length, scan, &e);
		if (status != HULL_SEXPR_OK)
			return status;
		scan += e.extent;
	}

	/* Then each is placed in turn: the smallest key above the last one
	 * placed. Two entries holding that key are a duplicate. */
	for (uint32_t placed = 0u; placed < count; placed++) {
		entry    next  = { 0u, 0u, NULL, 0u };
		unsigned found = 0u;

		for (uint32_t scan = 0u; scan < length; ) {
			entry e;

			/* Checked above, and checked again: an entry whose extent
			 * went unread would leave `scan` where it is for ever. */
			status = entry_at(entries, length, scan, &e);
			if (status != HULL_SEXPR_OK)
				return status;
			scan += e.extent;
			if (placed != 0u && compare(&e, &last) <= 0)
				continue;
			if (found == 0u || compare(&e, &next) < 0) {
				next  = e;
				found = 1u;
			} else if (compare(&e, &next) == 0) {
				found++;
			}
		}
		if (found > 1u)
			return HULL_SEXPR_ERR_DUPLICATE;
		if (next.extent > cap - *at - wrote)
			return HULL_SEXPR_ERR_SPACE;
		memcpy(out + *at + wrote, entries + next.offset, next.extent);
		wrote += next.extent;
		last = next;
	}
	*at += wrote;
	return HULL_SEXPR_OK;
}
