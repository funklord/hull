/*
 * spec_fuzz.c -- libFuzzer over spec/spec.c and the s-expression reader
 * under it, which read every hull.conf: each input checked as a whole
 * file, from seeds that are project.md's own examples. Wherever it reads
 * as forms, spec or not, readable or canonical, every form is also
 * framed twice -- by the tokens, as a checked document is read, and by
 * situ, as it was before -- and the two must agree. A crash, a sanitizer's report or a disagreement
 * stops the run. `make check-fuzz` runs it.
 */

#include <stdint.h>
#include <stdlib.h>
#include <stddef.h>
#include "spec.h"

/* Every form under `a` in the checked document and `b` in the same one
 * read as unchecked, side by side. */
static void agree(const hull_sexpr_doc *checked, const hull_sexpr_doc *judged, int found_a,
                  hull_sexpr_form a, int found_b, hull_sexpr_form b)
{
	while (found_a || found_b) {
		hull_sexpr_form child_a, child_b;

		if (found_a != found_b || a.offset != b.offset || a.extent != b.extent ||
		    a.kind != b.kind || a.content != b.content ||
		    a.content_length != b.content_length)
			abort();
		if (a.kind == HULL_SEXPR_LIST)
			agree(checked, judged, hull_sexpr_child(checked, &a, &child_a), child_a,
			      hull_sexpr_child(judged, &b, &child_b), child_b);
		found_a = hull_sexpr_next(checked, &a, &a);
		found_b = hull_sexpr_next(judged, &b, &b);
	}
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	hull_spec       spec;
	hull_spec_error error;
	hull_sexpr_form entry, a, b;
	hull_sexpr_doc  judged;

	hull_sexpr_doc  doc;

	if (hull_spec_open(&spec, data, size, &error) == HULL_SPEC_OK && spec.has_machine)
		(void)hull_spec_find(&spec, &spec.machine, "source", &entry);
	/* Whether or not it is a spec, wherever it reads as forms, readable
	 * or canonical, a manifest's and the wire's. */
	for (int syntax = 0; syntax < 2; syntax++) {
		if (hull_sexpr_open(&doc, data, size, syntax == 0 ? HULL_SEXPR_READABLE :
		                    HULL_SEXPR_CANONICAL, NULL) != HULL_SEXPR_OK)
			continue;
		judged = doc;
		judged.checked = 0;
		agree(&doc, &judged, hull_sexpr_first(&doc, &a), a, hull_sexpr_first(&judged, &b), b);
	}
	return 0;
}
