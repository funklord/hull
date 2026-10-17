/*
 * protocol_view.h -- the generated agent protocol, included through here
 * and nowhere else.
 *
 * The same ordering fault as core/frontend_view.h (situ finding 18): the
 * check of `form` calls list's validate before it is declared. Declaring
 * the one function ahead of the generated header is enough to compile.
 * Delete this file, and include protocol.h directly, once situ orders the
 * pair itself.
 */

#ifndef HULL_PROTOCOL_VIEW_H
#define HULL_PROTOCOL_VIEW_H

#include "situ.h"

static inline situ_err_t hull_protocol_list_validate(situ_view_t view);

#include "protocol.h"

#endif /* HULL_PROTOCOL_VIEW_H */
