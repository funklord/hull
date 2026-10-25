/*
 * idmap -- the user's subordinate ids mapped into a machine's namespace,
 * and a stored tree's owners given back in its tree. Shared by the chroot
 * and qemu engines: both map the user to root and the user's subordinate
 * range beside it, so a chown inside to an id the range holds works, and
 * both give a stored tree its manifest's owners before the workload. The
 * copies were one each until they were the same, so they are one.
 */
#ifndef HULL_ENGINE_IDMAP_H
#define HULL_ENGINE_IDMAP_H

#include <sys/types.h>

/*
 * Many uids for the machine `pid`, from outside it: where the user has a
 * subordinate range of each in /etc/subuid and /etc/subgid and the
 * setuid helpers that map one, 0 is the user and 1 on the range, so a
 * chown inside to an id the range holds works, as a postinst's to its
 * package's own user must. Whether it did.
 */
int hull_map_many(pid_t pid, uid_t uid, gid_t gid);

/*
 * (owners PATH)'s list, read whole from `fd`: each entry `UID`, `GID` and
 * a path of the tree, each ended by a NUL, since a path may hold a
 * newline. Each is given its owner at `prefix` + its path, not following
 * a link -- `prefix` the machine's root as the caller reaches it, "/"
 * after a pivot or "merged/" beside a host-side overlay. Those that
 * cannot be, as where one uid is mapped, are counted and said once on
 * `err`, the workload's stderr, since a build that took them silently
 * would lose owners it seemed to keep. `fd` is closed.
 */
void hull_give_owners(int fd, const char *prefix, int err);

#endif
