/*
 * idmap -- see idmap.h. The user's subordinate range mapped into a
 * machine's namespace from outside, and a stored tree's owners given
 * back in its tree once that range holds them.
 */
#define _GNU_SOURCE

#include <fcntl.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>

#include "idmap.h"

/*
 * The user's subordinate range in `file`, /etc/subuid or /etc/subgid: the
 * line naming the user, by name or by id, as `name:start:count`. 1 and
 * the range, or 0 where there is none.
 */
static int subordinate(const char *file, const char *name, unsigned long id, unsigned long *start,
                       unsigned long *count)
{
	FILE *f = fopen(file, "re");
	char  line[512];
	char  number[32];
	int   found = 0;

	if (f == NULL)
		return 0;
	snprintf(number, sizeof number, "%lu", id);
	while (!found && fgets(line, sizeof line, f) != NULL) {
		char *first = strtok(line, ":\n");
		char *from  = strtok(NULL, ":\n");
		char *many  = strtok(NULL, ":\n");

		if (first != NULL && from != NULL && many != NULL &&
		    (strcmp(first, name) == 0 || strcmp(first, number) == 0)) {
			*start = strtoul(from, NULL, 10);
			*count = strtoul(many, NULL, 10);
			found  = *count > 0u;
		}
	}
	fclose(f);
	return found;
}

/* `helper` (newuidmap or newgidmap) run on `pid`: 0 the user's own id, 1
 * on the range. Whether it mapped them. */
static int run_mapper(const char *helper, pid_t pid, unsigned long id, unsigned long start,
                      unsigned long count)
{
	char  text[5][32];
	char *argv[9];
	pid_t child;
	int   status = 0;

	snprintf(text[0], sizeof text[0], "%ld", (long)pid);
	snprintf(text[1], sizeof text[1], "%lu", id);
	snprintf(text[2], sizeof text[2], "%lu", start);
	snprintf(text[3], sizeof text[3], "%lu", count);
	argv[0] = (char *)helper;
	argv[1] = text[0];
	argv[2] = "0";
	argv[3] = text[1];
	argv[4] = "1";
	argv[5] = "1";
	argv[6] = text[2];
	argv[7] = text[3];
	argv[8] = NULL;
	child = fork();
	if (child == 0) {
		int null = open("/dev/null", O_RDWR | O_CLOEXEC);

		if (null >= 0) {
			dup2(null, 0);
			dup2(null, 1);
			dup2(null, 2);
		}
		execv(helper, argv);
		_exit(127);
	}
	return child > 0 && waitpid(child, &status, 0) == child && WIFEXITED(status) &&
	       WEXITSTATUS(status) == 0;
}

int hull_map_many(pid_t pid, uid_t uid, gid_t gid)
{
	struct passwd *me = getpwuid(uid);
	unsigned long  uid_start;
	unsigned long  uid_count;
	unsigned long  gid_start;
	unsigned long  gid_count;

	if (me == NULL ||
	    !subordinate("/etc/subuid", me->pw_name, (unsigned long)uid, &uid_start, &uid_count) ||
	    !subordinate("/etc/subgid", me->pw_name, (unsigned long)uid, &gid_start, &gid_count))
		return 0;
	return run_mapper("/usr/bin/newuidmap", pid, (unsigned long)uid, uid_start, uid_count) &&
	       run_mapper("/usr/bin/newgidmap", pid, (unsigned long)gid, gid_start, gid_count);
}

void hull_give_owners(int fd, const char *prefix, int err)
{
	static char data[8u << 20];
	size_t      length = 0u;
	ssize_t     n;
	unsigned    refused = 0u;
	unsigned    given = 0u;

	while (length < sizeof data && (n = read(fd, data + length, sizeof data - length)) > 0)
		length += (size_t)n;
	close(fd);
	for (size_t at = 0u; at < length; ) {
		const char *fields[3];
		unsigned    k = 0u;
		char        path[4200];

		for (; k < 3u && at < length; k++) {
			fields[k] = data + at;
			while (at < length && data[at] != '\0')
				at++;
			if (at == length)
				break;
			at++;
		}
		if (k < 3u)
			break;
		snprintf(path, sizeof path, "%s%s", prefix, fields[2]);
		if (fchownat(AT_FDCWD, path, (uid_t)strtoul(fields[0], NULL, 10),
		             (gid_t)strtoul(fields[1], NULL, 10), AT_SYMLINK_NOFOLLOW) == 0)
			given++;
		else
			refused++;
	}
	if (refused != 0u) {
		char said[200];
		int  m = snprintf(said, sizeof said, "hull: owners: %u of %u not given: %s\n", refused,
		                  refused + given, "the machine maps one uid, or a path is not there");

		if (m > 0 && write(err, said, (size_t)m) < 0) {
			/* the workload's stderr is gone; nothing to tell */
		}
	}
}
