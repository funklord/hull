/*
 * rootmode.h -- running root mode's helper, hull-root (project.md section
 * 7), for the core and the engines alike.
 */

#ifndef HULL_ROOTMODE_H
#define HULL_ROOTMODE_H

#include <stddef.h>
#include <stdint.h>

#include "hostdev.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Where hull-root's device op makes a machine's nodes: a tmpfs of its
 * own, its directory UID/NAME the user's once made (section 7). */
#define HULL_ROOT_DEVICES "/run/hull-root"

/* The helper hull runs: HULL_ROOT_HELPER where the environment names one,
 * as the suites do, else where `make install-root` puts it. */
const char *hull_root_path(void);

/*
 * hull-root with `args`, NULL-ended, after its own name, `input` on its
 * stdin where it is not NULL, as an op that acts on a plan is handed the
 * plan to check: 0 where it granted, or -1 with what it said, or why it
 * could not be asked, in `why`. Where `fds` is not NULL, a socket on its
 * fd 3 takes what it hands back, up to `fd_count` descriptors into `fds`,
 * -1 where none came: what only root may make, a socket bound on a
 * privileged port, given to a core run as the user.
 */
int hull_root_call(const char *const *args, const uint8_t *input, size_t length, int *fds,
                   unsigned fd_count, char *why, size_t cap);

/*
 * Root mode's devices, (devices (root ...)) (section 7): hull-root, from
 * `plan`, whose root hash is `hash`, makes each device it names again as
 * a node of the user's, of the same kind and number, in a directory of
 * its own under HULL_ROOT_DEVICES/UID, named here; the node of each of
 * `devices` flagged root, found already, into its host.granted, and the
 * directory into `dir`. NULL, or why in `why`, nothing then left made.
 */
const char *hull_root_grant(const char *hash, const uint8_t *plan, size_t length,
                            hull_device *devices, unsigned count, char *dir, size_t dir_cap,
                            char *why, size_t cap);

/* What hull_root_grant made, the user's: its nodes and `dir`, removed,
 * and `dir` emptied. */
void hull_root_ungrant(hull_device *devices, unsigned count, char *dir);

#ifdef __cplusplus
}
#endif

#endif /* HULL_ROOTMODE_H */
