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

/* A run of ids: `count` from `start`. */
typedef struct hull_id_range {
	unsigned long start;
	unsigned long count;
} hull_id_range;

/* One line of a map, as newuidmap and newgidmap take it: `count` ids
 * from `inside` in the machine's namespace are those from `outside`. */
typedef struct hull_id_line {
	unsigned long inside;
	unsigned long outside;
	unsigned long count;
} hull_id_line;

#define HULL_ID_EXTRAS 16u
#define HULL_ID_LINES  (2u + 2u * HULL_ID_EXTRAS)

/*
 * The map hull_map_many() asks for, into `out`: 0 the user's own id, 1 on
 * `main`, its subordinate range, and each of `extras` -- a further line
 * of the user's in /etc/subuid or /etc/subgid, such as `user:100:1` for a
 * group of the user's that its trees belong to -- as itself, the same id
 * inside as out, `main` split around it where it falls in `main`'s ids
 * inside. An extra that is 0, or that the map holds already, is left out.
 * How many lines; 0 where `cap` is too small.
 */
size_t hull_id_map(unsigned long own, hull_id_range main, const hull_id_range *extras,
                   size_t extra_count, hull_id_line *out, size_t cap);

/*
 * Whether a host uid (`group` 0) or gid (`group` 1) shows inside a
 * machine of the user `uid`, `gid` as an id of its own, rather than as
 * nobody's: the user's own, and where `many`, what hull_map_many()'s map
 * holds. An overlay cannot copy up what is nobody's, and refuses a write
 * there with EOVERFLOW.
 */
int hull_id_mapped(int group, unsigned long id, uid_t uid, gid_t gid, int many);

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
