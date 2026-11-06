/*
 * user.c -- see user.h. /etc/passwd and /etc/group are read by hand: the
 * files are the machine's, not this process's NSS, and a static buffer
 * serves callers that may not take malloc's lock. One caller at a time per
 * process, which every caller is: a fork's child, or the agent's.
 */

#define _GNU_SOURCE

#include "user.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>

#define FILE_MAX 262144u

static char file[FILE_MAX];

/* `root`/`name` read whole into `file`: its length, or -1. A tree's own
 * /etc/passwd is not followed out of the tree where `root` is a host path. */
static ssize_t read_file(const char *root, const char *name)
{
	char    path[4200];
	size_t  have = 0u;
	ssize_t n;
	int     fd;

	if ((size_t)snprintf(path, sizeof path, "%s%s", root, name) >= sizeof path)
		return -1;
	fd = open(path, O_RDONLY | O_CLOEXEC | (root[0] != '\0' ? O_NOFOLLOW : 0));
	if (fd < 0)
		return -1;
	while (have < FILE_MAX - 1u && (n = read(fd, file + have, FILE_MAX - 1u - have)) != 0) {
		if (n < 0 && errno == EINTR)
			continue;
		if (n < 0) {
			close(fd);
			return -1;
		}
		have += (size_t)n;
	}
	close(fd);
	file[have] = '\0';
	return (ssize_t)have;
}

static int number(const char *text, size_t length, uint32_t *value)
{
	uint64_t n = 0u;

	if (length == 0u || length > 10u)
		return 0;
	for (size_t i = 0u; i < length; i++) {
		if (text[i] < '0' || text[i] > '9')
			return 0;
		n = n * 10u + (uint64_t)(text[i] - '0');
	}
	if (n >= 0xffffffffu)
		return 0;
	*value = (uint32_t)n;
	return 1;
}

/* The `count` fields of the line at `line`, split on ':' in place by their
 * starts and lengths; the line's end is its last. */
static unsigned fields(const char *line, const char *end, const char **at, size_t *length,
                       unsigned count)
{
	unsigned n = 0u;

	while (n < count) {
		const char *colon = memchr(line, ':', (size_t)(end - line));

		at[n] = line;
		length[n] = (size_t)((colon != NULL ? colon : end) - line);
		n++;
		if (colon == NULL)
			break;
		line = colon + 1;
	}
	return n;
}

static const char *next_line(const char *line, const char **end)
{
	const char *newline = strchr(line, '\n');

	*end = newline != NULL ? newline : line + strlen(line);
	return newline != NULL ? newline + 1 : NULL;
}

static int same(const char *text, size_t length, const char *word, size_t word_length)
{
	return length == word_length && memcmp(text, word, length) == 0;
}

/* The passwd entry of `name`, or of `uid` where `name` is NULL: 1 where
 * found, its uid, gid and home taken. */
static int read_passwd(const char *name, size_t name_length, hull_user *user)
{
	for (const char *line = file, *end; line != NULL && *line != '\0';) {
		const char *at[7];
		size_t      length[7];
		const char *next = next_line(line, &end);
		uint32_t    uid;
		uint32_t    gid;

		if (fields(line, end, at, length, 7u) >= 6u && number(at[2], length[2], &uid) &&
		    number(at[3], length[3], &gid) &&
		    (name != NULL ? same(at[0], length[0], name, name_length) : uid == user->uid)) {
			user->uid = uid;
			user->gid = gid;
			if (length[5] != 0u && length[5] < sizeof user->home) {
				memcpy(user->home, at[5], length[5]);
				user->home[length[5]] = '\0';
			}
			return 1;
		}
		line = next;
	}
	return 0;
}

/* The gid of the group `name`: 1 where found. */
static int group_of(const char *name, size_t name_length, uint32_t *gid)
{
	for (const char *line = file, *end; line != NULL && *line != '\0';) {
		const char *at[4];
		size_t      length[4];
		const char *next = next_line(line, &end);

		if (fields(line, end, at, length, 4u) >= 3u && same(at[0], length[0], name, name_length) &&
		    number(at[2], length[2], gid))
			return 1;
		line = next;
	}
	return 0;
}

/* The groups naming `name` as a member, its own gid aside. */
static void supplementary(const char *name, size_t name_length, hull_user *user)
{
	for (const char *line = file, *end; line != NULL && *line != '\0';) {
		const char *at[4];
		size_t      length[4];
		const char *next = next_line(line, &end);
		uint32_t    gid;

		if (fields(line, end, at, length, 4u) == 4u && number(at[2], length[2], &gid) &&
		    gid != user->gid && user->group_count < HULL_USER_GROUPS) {
			const char *member = at[3];
			const char *stop = at[3] + length[3];

			while (member < stop) {
				const char *comma = memchr(member, ',', (size_t)(stop - member));
				size_t      size = (size_t)((comma != NULL ? comma : stop) - member);

				if (same(member, size, name, name_length)) {
					user->groups[user->group_count++] = gid;
					break;
				}
				member += size + 1u;
			}
		}
		line = next;
	}
}

const char *hull_user_resolve(const char *root, const char *spec, hull_user *user)
{
	const char *colon = strchr(spec, ':');
	size_t      name_length = colon != NULL ? (size_t)(colon - spec) : strlen(spec);
	const char *group = colon != NULL ? colon + 1 : NULL;
	int         numeric;
	int         listed;

	memset(user, 0, sizeof *user);
	strcpy(user->home, "/");
	if (name_length == 0u)
		return "(user ...) names no one";
	numeric = number(spec, name_length, &user->uid);
	listed = read_file(root, "/etc/passwd") >= 0 &&
	         read_passwd(numeric ? NULL : spec, name_length, user);
	/* Root needs no file to be root: a tree of one program has none. */
	if (!numeric && !listed && !hull_user_is_root(spec))
		return "(user ...) names a user the machine's /etc/passwd does not";
	if (group != NULL && group[0] != '\0') {
		size_t length = strlen(group);

		if (!number(group, length, &user->gid) &&
		    (read_file(root, "/etc/group") < 0 || !group_of(group, length, &user->gid)))
			return "(user ...) names a group the machine's /etc/group does not";
		user->groups[user->group_count++] = user->gid;
		return NULL;
	}
	/* Docker's: its own group, and with a name and no group named its others. */
	user->groups[user->group_count++] = user->gid;
	if (listed && !numeric && read_file(root, "/etc/group") >= 0)
		supplementary(spec, name_length, user);
	return NULL;
}

int hull_user_become(const hull_user *user)
{
	if (syscall(SYS_setgroups, (long)user->group_count, user->groups) != 0 ||
	    syscall(SYS_setgid, (long)user->gid) != 0 || syscall(SYS_setuid, (long)user->uid) != 0)
		return -1;
	return 0;
}

int hull_user_is_root(const char *spec)
{
	size_t length = strcspn(spec, ":");

	return same(spec, length, "root", 4u) || same(spec, length, "0", 1u);
}
