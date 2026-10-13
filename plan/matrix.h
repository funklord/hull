/*
 * matrix.h -- one machine's fields against one engine's column of the
 * capability matrix (project.md sections 5.0 and 6).
 *
 * hull_matrix_resolve() names every field a checked machine uses, looks
 * each up in the engine's declaration and reports what it resolved to.
 * A field the engine does not declare is rejected, never ignored (5.0),
 * and a rejection is honoured only when the user accepted the token the
 * engine named for it (section 6, rule 1). The ids are the ones section
 * 4.4 lists; this is where they are derived.
 */

#ifndef HULL_MATRIX_H
#define HULL_MATRIX_H

#include <stdint.h>

#include "hull_engine.h"
#include "spec.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum hull_outcome {
	HULL_OUTCOME_NATIVE,
	HULL_OUTCOME_EMULATED,  /* honoured at the declared cost */
	HULL_OUTCOME_ACCEPTED,  /* rejected, and honoured because its token was accepted */
	HULL_OUTCOME_REJECTED,
	HULL_OUTCOME_IGNORED,   /* tuning for another class or engine: a warning */
} hull_outcome;

/* One field, as resolved. `id` is valid only during the report call that
 * hands it out. `note` is the engine's cost or reason, or the planner's
 * own where the engine has none to give; `token` is what `--accept` takes
 * to honour a rejection, or NULL. Both live as long as the declaration. */
typedef struct hull_resolution {
	const char    *id;
	hull_outcome   outcome;
	const char    *note;
	const char    *token;
	uint32_t       offset;  /* the entry in the spec that asks for it */
} hull_resolution;

typedef void (*hull_matrix_report)(void *context, const hull_resolution *resolution);

typedef enum hull_matrix_status {
	HULL_MATRIX_OK = 0,
	HULL_MATRIX_REJECTED,    /* at least one field was reported rejected */
	HULL_MATRIX_ERR_CLASS,   /* the engine does not implement `class_name` */
} hull_matrix_status;

/*
 * Resolve `machine`, a `(machine ...)` form or a named entry of
 * `(machines ...)` in a spec hull_spec_open() accepted, for the engine
 * `caps` running it as `class_name` on a host whose arch is `host_arch`,
 * or NULL where only `host` names it. `accepted` is the user's tokens,
 * NULL-terminated, or NULL for none. Every field is reported, in the
 * order the spec gives them, so `hull plan --explain` can print the lot;
 * the status says whether any was rejected.
 */
hull_matrix_status hull_matrix_resolve(const hull_spec *spec, const hull_sexpr_form *machine,
                                       const hull_capabilities *caps, const char *class_name,
                                       const char *host_arch, const char *const *accepted,
                                       hull_matrix_report report, void *context);

#ifdef __cplusplus
}
#endif

#endif /* HULL_MATRIX_H */
