/*
 * runtime.h -- the runtime directory a user's core lives in when
 * XDG_RUNTIME_DIR names none, as for a service account a CI server runs
 * jobs as, which has no login session and so no /run/user/UID (section 7).
 */

#ifndef HULL_RUNTIME_H
#define HULL_RUNTIME_H

#include <stddef.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/* One place to look, in order: `dir`/UID where it is not to be made, as
 * /run/user/UID is a login's to make, else `dir`/hull-UID, made 0700 if
 * missing; with `in_memory`, only where `dir` is a tmpfs or a ramfs, so
 * that a machine's secrets, which must be, can be. */
typedef struct hull_runtime_base {
	const char *dir;
	int         make;
	int         in_memory;
} hull_runtime_base;

/* The bases hull looks in when XDG_RUNTIME_DIR is unset or empty:
 * /run/user/UID, then /dev/shm/hull-UID, then /tmp/hull-UID. */
extern const hull_runtime_base hull_runtime_bases[3];

/*
 * The first of `bases` the user `uid` may use as their runtime
 * directory, into `out`: a directory, not a symlink, that `uid` owns and
 * no one else may enter. One made here is made 0700; one of the user's
 * that others may enter is made 0700 if it was to be made here, and
 * passed over if not. Anything else, a file, a symlink, or one another
 * user owns, is passed over and said in `why`, which holds the last
 * reason. 0, or -1 when none would do.
 */
int hull_runtime_choose(uid_t uid, const hull_runtime_base *bases, size_t n, char *out, size_t cap,
                        char *why, size_t why_cap);

#ifdef __cplusplus
}
#endif

#endif /* HULL_RUNTIME_H */
