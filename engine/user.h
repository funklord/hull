/*
 * user.h -- whom a machine's `(user ...)` names, by its own /etc/passwd and
 * /etc/group, as OCI's and docker's `USER` read them: `name`, `uid`, either
 * then `:group` or `:gid`.
 */

#ifndef HULL_USER_H
#define HULL_USER_H

#include <stdint.h>

#define HULL_USER_GROUPS 32u

typedef struct hull_user {
	uint32_t uid;
	uint32_t gid;
	uint32_t groups[HULL_USER_GROUPS];  /* the supplementary ones */
	unsigned group_count;
	char     home[256];                 /* its passwd's, or "/" */
} hull_user;

/* `spec` read against the tree at `root`, "" being this process's own:
 * NULL, or why it names nobody. Takes no lock and no memory, a raw clone of
 * a threaded core being among its callers. */
const char *hull_user_resolve(const char *root, const char *spec, hull_user *user);

/* This process become `user`, by the system calls: glibc's setuid() asks
 * every thread it knows of, and in a raw clone those are not here to answer.
 * 0, or -1 with errno. */
int hull_user_become(const hull_user *user);

/* Whether `spec` names root, by name or number, whatever the tree says. */
int hull_user_is_root(const char *spec);

#endif
