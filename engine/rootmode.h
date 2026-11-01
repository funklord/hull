/*
 * rootmode.h -- running root mode's helper, hull-root (project.md section
 * 7), for the core and the engines alike.
 */

#ifndef HULL_ROOTMODE_H
#define HULL_ROOTMODE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The helper hull runs: HULL_ROOT_HELPER where the environment names one,
 * as the suites do, else where `make install-root` puts it. */
const char *hull_root_path(void);

/*
 * hull-root with `args`, NULL-ended, after its own name, `input` on its
 * stdin where it is not NULL, as an op that acts on a plan is handed the
 * plan to check: 0 where it granted, or -1 with what it said, or why it
 * could not be asked, in `why`.
 */
int hull_root_call(const char *const *args, const uint8_t *input, size_t length, char *why,
                   size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* HULL_ROOTMODE_H */
