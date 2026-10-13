/*
 * access.h -- reading a plan, for engines (hull_engine.h: "accessor
 * functions are provided by the core").
 *
 * A plan is `(plan (class ...) (machine ...))` in canonical form, already
 * checked by the planner (project.md section 4.7), so these look things
 * up and copy them out and check nothing about their shape.
 */

#ifndef HULL_ACCESS_H
#define HULL_ACCESS_H

#include "sexpr.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The entry `(key ...)` among `list`'s elements after its first. */
int hull_access_find(const hull_sexpr_doc *doc, const hull_sexpr_form *list, const char *key,
                     hull_sexpr_form *entry);

/* An atom's bytes as a C string the caller frees, or NULL if it holds a
 * NUL byte or memory ran out. */
char *hull_access_text(const hull_sexpr_doc *doc, const hull_sexpr_form *form);

/* The value of `(key value)` in `record`, as hull_access_text() gives it,
 * or NULL where there is none. */
char *hull_access_value(const hull_sexpr_doc *doc, const hull_sexpr_form *record,
                        const char *key);

/* Whether `(key true)` is in `record`. */
int hull_access_true(const hull_sexpr_doc *doc, const hull_sexpr_form *record, const char *key);

/* Open `plan` and find its machine. Returns 0 if it is not a plan. */
int hull_access_machine(hull_sexpr_doc *doc, const uint8_t *plan, size_t length,
                        hull_sexpr_form *machine);

#ifdef __cplusplus
}
#endif

#endif /* HULL_ACCESS_H */
