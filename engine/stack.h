/*
 * stack.h -- a stack of directories mounted as one, unionfs's way, for
 * an engine's root or a volume (project.md section 3.7).
 *
 * The layers are read-only, each above the one before it; the writes
 * go to an upper, a directory of the user's that keeps them, or a tmpfs
 * that does not. A stack is staged in a directory of its own, by
 * open_tree() clones of each layer, so that a path holding a comma or a
 * colon, which overlayfs's options cannot name, is never named to it:
 * `lower0`, `lower1`, ..., `kept` or `mem`, and `merged`, the stack.
 * Called in a mount namespace of its own, whose mounts these are, so
 * that on the host the staging is empty directories.
 */

#ifndef HULL_STACK_H
#define HULL_STACK_H

#ifdef __cplusplus
extern "C" {
#endif

#define HULL_STACK_MAX 64u

typedef struct hull_stack {
	char    *layer[HULL_STACK_MAX];  /* the bottom first */
	unsigned count;
	char    *upper;                  /* the writes kept here, or NULL */
	char    *upper_work;             /* overlayfs's work directory, beside it */
	int      readonly;               /* no upper at all, nothing written */
} hull_stack;

/* overlayfs's work directory for `upper`, which must be on the upper's
 * own filesystem: beside it, named for it and hidden. Allocated, or
 * NULL when memory ran out. */
char *hull_stack_work_of(const char *upper);

/* `stack` staged in `at`, an empty directory, its stack at `at`/merged:
 * NULL, or what could not be done. A read-only stack of one layer is
 * that layer, bound there read-only, since overlayfs refuses one lower
 * layer with no upper. */
const char *hull_stack_stage(const hull_stack *stack, const char *at);

/* The empty directories hull_stack_stage() left in `at` on the host,
 * removed by name, `at` with them. */
void hull_stack_unstage(const char *at);

/* The work directory overlayfs leaves in `upper_work`, mode 000, which
 * the user could not then remove, given back to the user, 0700: called
 * once the stack is unmounted. A work directory overlayfs makes again
 * on each mount, so nothing is lost. */
void hull_stack_unwork(const char *upper_work);

void hull_stack_release(hull_stack *stack);

#ifdef __cplusplus
}
#endif

#endif
