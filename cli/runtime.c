/*
 * runtime.c -- a runtime directory where XDG_RUNTIME_DIR names none. See
 * runtime.h.
 *
 * A directory under /dev/shm or /tmp is one anyone may make first, so what
 * is found there is taken only if it is a directory, not a symlink, owned
 * by the user, and made 0700 here; another user's, made to catch this
 * user's sockets, is passed over rather than used.
 */

#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/vfs.h>
#include <unistd.h>

#include "runtime.h"

#define TMPFS_MAGIC 0x01021994
#define RAMFS_MAGIC 0x858458f6

const hull_runtime_base hull_runtime_bases[3] = {
	{ "/run/user", 0, 0 },
	{ "/dev/shm",  1, 1 },
	{ "/tmp",      1, 0 },
};

static void say(char *why, size_t cap, const char *path, const char *reason)
{
	if (why != NULL && cap > 0u)
		snprintf(why, cap, "%s: %s", path, reason);
}

int hull_runtime_choose(uid_t uid, const hull_runtime_base *bases, size_t n, char *out, size_t cap,
                        char *why, size_t why_cap)
{
	say(why, why_cap, "a runtime directory", "no place to look");
	for (size_t i = 0u; i < n; i++) {
		char          path[4096];
		struct stat   st;
		struct statfs fs;
		int           fd;

		if ((size_t)snprintf(path, sizeof path, "%s/%s%u", bases[i].dir,
		                     bases[i].make ? "hull-" : "", (unsigned)uid) >= sizeof path)
			continue;
		if (bases[i].in_memory &&
		    (statfs(bases[i].dir, &fs) != 0 ||
		     (fs.f_type != (__fsword_t)TMPFS_MAGIC && fs.f_type != (__fsword_t)RAMFS_MAGIC))) {
			say(why, why_cap, bases[i].dir, "not in memory, or not there");
			continue;
		}
		if (bases[i].make && mkdir(path, 0700) != 0 && errno != EEXIST) {
			say(why, why_cap, path, strerror(errno));
			continue;
		}
		/* Opened as it is, following nothing, and judged by what was
		 * opened, so that it cannot be changed between the look and the
		 * use. */
		fd = open(path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
		if (fd < 0 || fstat(fd, &st) != 0) {
			say(why, why_cap, path, errno == ELOOP || errno == ENOTDIR ?
			    "not a directory, or a symlink" : strerror(errno));
			if (fd >= 0)
				close(fd);
			continue;
		}
		if (st.st_uid != uid) {
			say(why, why_cap, path, "another user's");
			close(fd);
			continue;
		}
		if ((st.st_mode & 077) != 0) {
			if (!bases[i].make || fchmod(fd, 0700) != 0) {
				say(why, why_cap, path, "others may enter it");
				close(fd);
				continue;
			}
		}
		close(fd);
		if ((size_t)snprintf(out, cap, "%s", path) >= cap)
			continue;
		return 0;
	}
	return -1;
}
