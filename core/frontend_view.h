/*
 * frontend_view.h -- the generated frontend protocol, included through
 * here and nowhere else.
 *
 * situ's C for a schema that imports canonical.situ and uses `form`
 * defines form's check before list's validate, which it calls, and both
 * are `static inline` (situ suggestion/hull.md, finding 18). Declaring
 * the one function ahead of the generated header is enough to compile,
 * and changes nothing the generator wrote. Delete this file, and include
 * frontend.h directly, once situ orders the pair itself.
 */

#ifndef HULL_FRONTEND_VIEW_H
#define HULL_FRONTEND_VIEW_H

#include "situ.h"

static inline situ_err_t hull_frontend_list_validate(situ_view_t view);

#include "frontend.h"

#endif /* HULL_FRONTEND_VIEW_H */
