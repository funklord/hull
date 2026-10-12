/*
 * spec.h -- hull.conf, checked against Appendix J (project.md section 4).
 *
 * hull_spec_open() reads a file with the readable reader and holds every
 * form in it to the key inventory: unknown keys are errors (J.8), as are a
 * key given twice, a required key missing, a value of the wrong shape and
 * a value outside its key's set. What is left for the planner is what
 * depends on a backend -- whether an engine supports a field, and the
 * defaults a manifest supplies -- so a spec that opens is one the planner
 * can read without checking it again.
 *
 * Like the reader, nothing is copied or allocated: a spec is the reader's
 * document plus the top-level forms found in it, and values are read with
 * the reader's own calls.
 */

#ifndef HULL_SPEC_H
#define HULL_SPEC_H

#include <stddef.h>
#include <stdint.h>

#include "sexpr.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum hull_spec_status {
	HULL_SPEC_OK = 0,
	HULL_SPEC_ERR_SYNTAX,     /* the reader refused the file */
	HULL_SPEC_ERR_UNKNOWN,    /* a key, entry or name the schema lacks */
	HULL_SPEC_ERR_DUPLICATE,  /* a key or name given twice */
	HULL_SPEC_ERR_MISSING,    /* a required key absent */
	HULL_SPEC_ERR_SHAPE,      /* the wrong kind or number of values */
	HULL_SPEC_ERR_VALUE,      /* a value outside its key's set or form */
} hull_spec_status;

/* Where checking stopped, and why. `path` names the key as written from
 * the top of the file, map names included: "machine.ports.port.host",
 * "machines.web.env.RUST_LOG". A path too long for it is cut short. */
typedef struct hull_spec_error {
	hull_spec_status  status;
	uint32_t          offset;
	uint32_t          line;    /* 1-based */
	uint32_t          column;  /* 1-based, in bytes */
	const char       *reason;  /* a static string */
	char              path[128];
} hull_spec_error;

/* A checked file. Each form is a whole top-level list, its key included,
 * and is valid only where its flag is set. A file holds `machine` or
 * `machines`, not both, and may hold `build` beside either or alone. */
typedef struct hull_spec {
	hull_sexpr_doc   doc;
	hull_sexpr_form  machine;
	hull_sexpr_form  machines;
	hull_sexpr_form  build;
	int              has_machine;
	int              has_machines;
	int              has_build;
} hull_spec;

/* Read and check a whole file, binding `spec` to `data`, which must
 * outlive it. Nothing else may be called on a spec this did not return
 * HULL_SPEC_OK for. `error` may be NULL. */
hull_spec_status hull_spec_open(hull_spec *spec, const uint8_t *data, size_t length,
                                hull_spec_error *error);

/* The entry `(key ...)` in `record`, a list whose first element names it.
 * Returns 1 and fills `entry`, or 0 when the record has no such key. */
int hull_spec_find(const hull_spec *spec, const hull_sexpr_form *record,
                   const char *key, hull_sexpr_form *entry);

#ifdef __cplusplus
}
#endif

#endif /* HULL_SPEC_H */
