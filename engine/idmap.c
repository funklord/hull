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
 * The user's subordinate ranges in `file`, /etc/subuid or /etc/subgid:
 * each line naming the user, by name or by id, as `name:start:count`.
 * The first is `main`, the rest `extras`, at most HULL_ID_EXTRAS. How
 * many there are in all; 0 where there is none.
 */
static size_t subordinate(const char *file, const char *name, unsigned long id,
                          hull_id_range *main, hull_id_range *extras, size_t *extra_count)
{
	FILE  *f = fopen(file, "re");
	char   line[512];
	char   number[32];
	size_t found = 0u;

	*extra_count = 0u;
	if (f == NULL)
		return 0u;
	snprintf(number, sizeof number, "%lu", id);
	while (fgets(line, sizeof line, f) != NULL) {
		char         *first = strtok(line, ":\n");
		char         *from  = strtok(NULL, ":\n");
		char         *many  = strtok(NULL, ":\n");
		hull_id_range range;

		if (first == NULL || from == NULL || many == NULL ||
		    (strcmp(first, name) != 0 && strcmp(first, number) != 0))
			continue;
		range.start = strtoul(from, NULL, 10);
		range.count = strtoul(many, NULL, 10);
		if (range.count == 0u)
			continue;
		if (found == 0u)
			*main = range;
		else if (*extra_count < HULL_ID_EXTRAS)
			extras[(*extra_count)++] = range;
		found++;
	}
	fclose(f);
	return found;
}

size_t hull_id_map(unsigned long own, hull_id_range main, const hull_id_range *extras,
                   size_t extra_count, hull_id_line *out, size_t cap)
{
	hull_id_range sorted[HULL_ID_EXTRAS];
	size_t        kept = 0u;
	size_t        n = 0u;
	unsigned long next = 1u;   /* the first id inside that main has yet to give */
	unsigned long taken = 1u;  /* past the last id inside that an extra has */

	/* The extras to map as themselves, by where they start: not 0, not
	 * the user's own id, and none that main gives out already, as an id
	 * outside is mapped once. */
	for (size_t i = 0u; i < extra_count && kept < HULL_ID_EXTRAS; i++) {
		hull_id_range e = extras[i];
		size_t        at = kept;

		if (e.start == 0u || (e.start <= own && own < e.start + e.count) ||
		    (e.start < main.start + main.count && main.start < e.start + e.count))
			continue;
		while (at > 0u && sorted[at - 1u].start > e.start) {
			sorted[at] = sorted[at - 1u];
			at--;
		}
		sorted[at] = e;
		kept++;
	}
	if (cap < 2u + 2u * kept)
		return 0u;
	out[n++] = (hull_id_line){ 0u, own, 1u };
	for (size_t i = 0u; i < kept; i++) {
		hull_id_range e = sorted[i];

		if (e.start < taken)
			continue;   /* one extra within another's */
		/* main's ids inside up to this extra, then the extra as itself,
		 * main going on after it. */
		if (e.start <= main.count) {
			if (e.start > next)
				out[n++] = (hull_id_line){ next, main.start + (next - 1u), e.start - next };
			if (e.start + e.count > next)
				next = e.start + e.count;
		} else if (next <= main.count) {
			/* Past main inside: the rest of main first, so the lines run
			 * in order of their ids inside. */
			out[n++] = (hull_id_line){ next, main.start + (next - 1u), main.count + 1u - next };
			next = main.count + 1u;
		}
		out[n++] = (hull_id_line){ e.start, e.start, e.count };
		taken = e.start + e.count;
	}
	if (next <= main.count)
		out[n++] = (hull_id_line){ next, main.start + (next - 1u), main.count + 1u - next };
	return n;
}

/* `helper` (newuidmap or newgidmap) run on `pid` with `lines` of a map.
 * Whether it mapped them. */
static int run_mapper(const char *helper, pid_t pid, const hull_id_line *lines, size_t count)
{
	char  text[1u + 3u * HULL_ID_LINES][24];
	char *argv[3u + 3u * HULL_ID_LINES];
	pid_t child;
	int   status = 0;
	size_t a = 0u;

	snprintf(text[0], sizeof text[0], "%ld", (long)pid);
	argv[a++] = (char *)helper;
	argv[a++] = text[0];
	for (size_t i = 0u; i < count; i++) {
		snprintf(text[1u + 3u * i], sizeof text[0], "%lu", lines[i].inside);
		snprintf(text[2u + 3u * i], sizeof text[0], "%lu", lines[i].outside);
		snprintf(text[3u + 3u * i], sizeof text[0], "%lu", lines[i].count);
		argv[a++] = text[1u + 3u * i];
		argv[a++] = text[2u + 3u * i];
		argv[a++] = text[3u + 3u * i];
	}
	argv[a] = NULL;
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

/* The map of `file`'s for the user: its lines into `lines`, how many. */
static size_t map_of(const char *file, const char *name, unsigned long uid, unsigned long own,
                     hull_id_line *lines)
{
	hull_id_range main;
	hull_id_range extras[HULL_ID_EXTRAS];
	size_t        extra_count;

	if (subordinate(file, name, uid, &main, extras, &extra_count) == 0u)
		return 0u;
	return hull_id_map(own, main, extras, extra_count, lines, HULL_ID_LINES);
}

int hull_map_many(pid_t pid, uid_t uid, gid_t gid)
{
	struct passwd *me = getpwuid(uid);
	hull_id_line   uids[HULL_ID_LINES];
	hull_id_line   gids[HULL_ID_LINES];
	size_t         uid_lines;
	size_t         gid_lines;

	if (me == NULL ||
	    (uid_lines = map_of("/etc/subuid", me->pw_name, uid, uid, uids)) == 0u ||
	    (gid_lines = map_of("/etc/subgid", me->pw_name, uid, gid, gids)) == 0u)
		return 0;
	return run_mapper("/usr/bin/newuidmap", pid, uids, uid_lines) &&
	       run_mapper("/usr/bin/newgidmap", pid, gids, gid_lines);
}

int hull_id_mapped(int group, unsigned long id, uid_t uid, gid_t gid, int many)
{
	struct passwd *me;
	hull_id_line   lines[HULL_ID_LINES];
	size_t         count;

	if (id == (group ? (unsigned long)gid : (unsigned long)uid))
		return 1;
	if (!many || (me = getpwuid(uid)) == NULL)
		return 0;
	count = map_of(group ? "/etc/subgid" : "/etc/subuid", me->pw_name, uid,
	               group ? gid : uid, lines);
	for (size_t i = 0u; i < count; i++)
		if (lines[i].outside <= id && id < lines[i].outside + lines[i].count)
			return 1;
	return 0;
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
