/*
 * plan.h -- a machine resolved for one engine: every default written in,
 * the projection chosen, and every field held to the engine's column
 * (project.md section 4.7).
 *
 * The resolved machine is readable text, the form `spec.resolved.conf`
 * takes (sections 3.1 and 4.1). Entries the user wrote are copied byte
 * for byte, so a report about one points at the user's own line; what
 * the planner adds is attributed to the entry it was added for. The text
 * is opened again with hull_spec_open(), so the planner's own output is
 * held to Appendix J, and resolved with hull_matrix_resolve(), so a
 * default asks of an engine exactly what the same value written would.
 */

#ifndef HULL_PLAN_H
#define HULL_PLAN_H

#include <stddef.h>
#include <stdint.h>

#include "matrix.h"
#include "registry.h"
#include "spec.h"

#ifdef __cplusplus
extern "C" {
#endif

#define HULL_PLAN_TEXT     65536u
#define HULL_PLAN_SEGMENTS 1024u

typedef enum hull_plan_status {
	HULL_PLAN_OK = 0,
	HULL_PLAN_REJECTED,      /* a field was reported rejected */
	HULL_PLAN_ERR_CLASS,     /* the engine does not implement the class */
	HULL_PLAN_ERR_SIZE,      /* the machine resolves to more than the plan holds */
	HULL_PLAN_ERR_INTERNAL,  /* the resolved text failed Appendix J: a planner bug */
} hull_plan_status;

/* A stretch of the resolved text, and where it came from: copied bytes map
 * one to one onto the user's file, added ones all onto `source`. */
typedef struct hull_plan_segment {
	uint32_t at;
	uint32_t length;
	uint32_t source;
	int      copied;
} hull_plan_segment;

typedef struct hull_plan {
	char               text[HULL_PLAN_TEXT];
	size_t             length;
	hull_spec          resolved;    /* `text`, opened */
	const char        *projection;  /* what auto chose, or what was written */
	hull_plan_segment  segments[HULL_PLAN_SEGMENTS];
	uint32_t           segment_count;
	const char        *reason;      /* why HULL_PLAN_ERR_INTERNAL */
} hull_plan;

/*
 * Resolve `machine` from `spec` for `choice` on a host whose arch is
 * `host_arch`. `name` is the machine's name in a `(machines ...)` file,
 * which is its hostname unless it says otherwise, or NULL. Every field is
 * reported, as hull_matrix_resolve() does, with its offset in the user's
 * file. `plan` is large; it is the caller's, and holds the resolved text
 * afterwards.
 */
hull_plan_status hull_plan_build(hull_plan *plan, const hull_spec *spec,
                                 const hull_sexpr_form *machine, const char *name,
                                 const hull_choice *choice, const char *host_arch,
                                 const char *const *accepted,
                                 hull_matrix_report report, void *context);

/* Where `offset` in the resolved text came from in the user's file. */
uint32_t hull_plan_source(const hull_plan *plan, uint32_t offset);

#ifdef __cplusplus
}
#endif

#endif /* HULL_PLAN_H */
