/*
 * sexpr.h -- hull's s-expression reader (project.md section 4.1).
 *
 * Two syntaxes. Readable is what people write: lists, symbols, strings,
 * `;` comments and `#hex#` octets, with whitespace between forms.
 * Canonical is what is hashed and signed (A.1): length-prefixed atoms and
 * lists, nothing else.
 *
 * situ frames each form from sexpr/readable.situ and sexpr/canonical.situ.
 * What situ leaves to the caller -- walking a list's elements, deciding
 * that a list was closed, and the rules about which bytes may follow which
 * -- is the walk in sexpr.c. hull_sexpr_open() runs that walk over the
 * whole document before anything else may read it, so every form the
 * navigation functions hand out has already been checked.
 *
 * The reader never copies and never allocates: a form is a set of offsets
 * into the caller's buffer, which must outlive the document. Comments are
 * stepped over and never handed out.
 */

#ifndef HULL_SEXPR_H
#define HULL_SEXPR_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Levels of nested lists. situ's schemas bound the same depth, counted in
 * structs: `[depth = 64]` with two structs per level. */
#define HULL_SEXPR_MAX_DEPTH 32u

typedef enum hull_sexpr_syntax {
	HULL_SEXPR_READABLE,
	HULL_SEXPR_CANONICAL,
} hull_sexpr_syntax;

typedef enum hull_sexpr_kind {
	HULL_SEXPR_LIST,
	HULL_SEXPR_SYMBOL,  /* readable only */
	HULL_SEXPR_STRING,  /* readable only; content still escaped */
	HULL_SEXPR_OCTETS,  /* readable only; content still hexadecimal */
	HULL_SEXPR_ATOM,    /* canonical only; content is the bytes */
} hull_sexpr_kind;

typedef enum hull_sexpr_status {
	HULL_SEXPR_OK = 0,
	HULL_SEXPR_ERR_SYNTAX,  /* the bytes are not this syntax */
	HULL_SEXPR_ERR_DEPTH,   /* lists nested deeper than the maximum */
	HULL_SEXPR_ERR_SIZE,    /* a document larger than 4 GiB */
	HULL_SEXPR_ERR_SPACE,   /* a decode or output buffer too small, or
	                           input lying where the output goes */
	HULL_SEXPR_ERR_KIND,    /* asked of a form of the wrong kind */
	HULL_SEXPR_ERR_DUPLICATE,  /* a record key given twice */
} hull_sexpr_status;

/* Where reading stopped, and why. `reason` is a static string. */
typedef struct hull_sexpr_error {
	hull_sexpr_status  status;
	uint32_t           offset;
	uint32_t           line;    /* 1-based */
	uint32_t           column;  /* 1-based, in bytes */
	const char        *reason;
} hull_sexpr_error;

typedef struct hull_sexpr_doc {
	const uint8_t      *data;
	uint32_t            length;
	hull_sexpr_syntax   syntax;
} hull_sexpr_doc;

/* A form: offsets into the document, valid for as long as it is. */
typedef struct hull_sexpr_form {
	hull_sexpr_kind  kind;
	uint32_t         offset;          /* the form's first byte */
	uint32_t         extent;          /* its length, closing byte included */
	uint32_t         content;         /* name, string body, digits, atom bytes;
	                                     for a list, the byte after `(` */
	uint32_t         content_length;  /* 0 for a list */
	uint32_t         end;             /* where the enclosing run stops: the
	                                     parent's `)`, or the document's end */
} hull_sexpr_form;

/* Check a whole document and bind `doc` to it. Nothing else may be called
 * on a document this did not return HULL_SEXPR_OK for. `error` may be NULL. */
hull_sexpr_status hull_sexpr_open(hull_sexpr_doc *doc, const uint8_t *data,
                                  size_t length, hull_sexpr_syntax syntax,
                                  hull_sexpr_error *error);

/* The 1-based line and byte column of `offset` in `doc`, as an error
 * reports them. */
void hull_sexpr_locate(const hull_sexpr_doc *doc, uint32_t offset,
                       uint32_t *line, uint32_t *column);

/* Navigation. Each returns 1 and fills `out`, or returns 0 when there is
 * nothing there: no first form, no next sibling, an empty list. */
int hull_sexpr_first(const hull_sexpr_doc *doc, hull_sexpr_form *out);
int hull_sexpr_next(const hull_sexpr_doc *doc, const hull_sexpr_form *form,
                    hull_sexpr_form *out);
int hull_sexpr_child(const hull_sexpr_doc *doc, const hull_sexpr_form *list,
                     hull_sexpr_form *out);

/* 1 if `form` is the symbol `name`, in readable syntax, or an atom of those
 * bytes, in canonical. */
int hull_sexpr_is(const hull_sexpr_doc *doc, const hull_sexpr_form *form,
                  const char *name);

/* An atom's value: a symbol's name, a string with its escapes decoded,
 * octets with their hexadecimal decoded, a canonical atom's bytes. Writes
 * at most `capacity` bytes and stores the value's length in `*length`
 * either way, so a caller can size a second attempt. */
hull_sexpr_status hull_sexpr_value(const hull_sexpr_doc *doc,
                                   const hull_sexpr_form *form, uint8_t *out,
                                   size_t capacity, size_t *length);

/*
 * The canonical writer (A.1). Each call appends to `out` at `*at`, never
 * past `cap`, and advances `*at` only on HULL_SEXPR_OK; on failure the
 * bytes from `*at` on are unspecified. Forms are written by situ's build
 * layer from sexpr/canonical.situ and built from the inside out: the
 * elements of a list are written first, into a buffer of their own, and
 * handed to hull_sexpr_put_list whole. Every form written is read back
 * with hull_sexpr_open before `*at` moves, so the writer refuses exactly
 * what the reader would, depth included.
 */

/* One atom holding `length` bytes. */
hull_sexpr_status hull_sexpr_put_atom(uint8_t *out, uint32_t cap, uint32_t *at,
                                      const uint8_t *bytes, uint32_t length);

/* One list whose elements are `items`: canonical forms, back to back, as
 * these functions wrote them. `items` must not overlap `out`. */
hull_sexpr_status hull_sexpr_put_list(uint8_t *out, uint32_t cap, uint32_t *at,
                                      const uint8_t *items, uint32_t length);

/* A record's entries in canonical order: `entries` holds lists, each
 * opening with its key's atom, in any order, and they are appended sorted
 * bytewise by key. A key given twice is HULL_SEXPR_ERR_DUPLICATE. For the
 * handful of keys a record has: each entry is placed by a pass over all of
 * them. `entries` must not overlap `out`. */
hull_sexpr_status hull_sexpr_put_sorted(uint8_t *out, uint32_t cap, uint32_t *at,
                                        const uint8_t *entries, uint32_t length);

#ifdef __cplusplus
}
#endif

#endif /* HULL_SEXPR_H */
