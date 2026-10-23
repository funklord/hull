/*
 * stack.c -- a stack of directories mounted as one (stack.h).
 */

#define _GNU_SOURCE

#include "stack.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <unistd.h>

char *hull_stack_work_of(const char *upper)
{
	const char *slash = strrchr(upper, '/');
	char       *work = malloc(strlen(upper) + 16u);

	if (work == NULL)
		return NULL;
	if (slash == NULL)
		sprintf(work, ".%s.hull-work", upper);
	else
		sprintf(work, "%.*s/.%s.hull-work", (int)(slash - upper), upper, slash + 1);
	return work;
}

/* A clone of `path` mounted at `at`/`name`, made for it. 0, or -1. */
static int clone_to(const char *path, int at, const char *name, int readonly)
{
	struct mount_attr attr = { .attr_set = MOUNT_ATTR_RDONLY };
	int               tree = open_tree(AT_FDCWD, path, OPEN_TREE_CLONE | OPEN_TREE_CLOEXEC);
	int               result = -1;

	if (tree >= 0 && mkdirat(at, name, 0700) == 0 &&
	    (!readonly || mount_setattr(tree, "", AT_EMPTY_PATH, &attr, sizeof attr) == 0) &&
	    move_mount(tree, "", at, name, MOVE_MOUNT_F_EMPTY_PATH) == 0)
		result = 0;
	if (tree >= 0)
		close(tree);
	return result;
}

const char *hull_stack_stage(const hull_stack *stack, const char *at)
{
	char        options[64u + 16u * HULL_STACK_MAX + 8400u];
	char        upper_dir[4200] = "mem/upper";
	char        work_dir[4200] = "mem/work";
	char        here[4300];
	size_t      length;
	int         dir;
	const char *why = NULL;

	if (stack->count == 0u)
		return "a stack has no layers";
	if ((dir = open(at, O_PATH | O_DIRECTORY | O_CLOEXEC)) < 0)
		return "its staging";
	if (stack->readonly && stack->count == 1u) {
		why = clone_to(stack->layer[0], dir, "merged", 1) != 0 ? "its one layer" : NULL;
		close(dir);
		return why;
	}
	for (unsigned i = 0u; i < stack->count && why == NULL; i++) {
		char name[32];

		snprintf(name, sizeof name, "lower%u", i);
		if (clone_to(stack->layer[i], dir, name, 0) != 0)
			why = "a layer";
	}
	if (why == NULL && stack->upper != NULL) {
		char        parent[4200];
		const char *slash = strrchr(stack->upper, '/');
		const char *work = strrchr(stack->upper_work, '/');

		/* overlayfs wants the upper and its work directory on one mount,
		 * so the directory holding both is cloned, once. The work
		 * directory is hull's, made here: the user's own group and not
		 * setgid, though its parent may be setgid to another, since
		 * overlayfs refuses one of a group the namespace does not map,
		 * EPERM, which is what one inherited from a shared setgid
		 * directory is. */
		snprintf(parent, sizeof parent, "%.*s", slash != NULL && slash != stack->upper ?
		         (int)(slash - stack->upper) : 1, slash != NULL ? stack->upper : ".");
		snprintf(upper_dir, sizeof upper_dir, "kept/%s",
		         slash != NULL ? slash + 1 : stack->upper);
		snprintf(work_dir, sizeof work_dir, "kept/%s",
		         work != NULL ? work + 1 : stack->upper_work);
		if (strpbrk(upper_dir, ",:") != NULL || strpbrk(work_dir, ",:") != NULL ||
		    (mkdir(stack->upper, 0755) != 0 && errno != EEXIST) ||
		    (mkdir(stack->upper_work, 0700) != 0 && errno != EEXIST) ||
		    chown(stack->upper_work, (uid_t)-1, 0) != 0 || chmod(stack->upper_work, 0700) != 0 ||
		    clone_to(parent, dir, "kept", 0) != 0)
			why = "the upper, kept";
	} else if (why == NULL && !stack->readonly) {
		snprintf(here, sizeof here, "%s/mem", at);
		if (mkdirat(dir, "mem", 0700) != 0 ||
		    mount("tmpfs", here, "tmpfs", 0u, "mode=0700") != 0 ||
		    mkdirat(dir, "mem/upper", 0755) != 0 || mkdirat(dir, "mem/work", 0700) != 0)
			why = "the upper, in memory";
	}
	if (why == NULL && mkdirat(dir, "merged", 0700) != 0)
		why = "its staging";
	if (why != NULL) {
		close(dir);
		return why;
	}
	/* The topmost layer first, each named relative to `at`, in which the
	 * mount is made and from which the caller's directory is restored. */
	length = (size_t)snprintf(options, sizeof options, "lowerdir=");
	for (unsigned i = stack->count; i > 0u; i--)
		length += (size_t)snprintf(options + length, sizeof options - length, "lower%u%s",
		                           i - 1u, i > 1u ? ":" : "");
	if (!stack->readonly)
		snprintf(options + length, sizeof options - length,
		         ",upperdir=%s,workdir=%s,userxattr", upper_dir, work_dir);
	else
		snprintf(options + length, sizeof options - length, ",userxattr");
	{
		int back = open(".", O_PATH | O_DIRECTORY | O_CLOEXEC);

		if (back < 0 || fchdir(dir) != 0 ||
		    mount("overlay", "merged", "overlay", stack->readonly ? MS_RDONLY : 0u, options) != 0)
			why = "mounting it";
		if (back >= 0) {
			if (fchdir(back) != 0 && why == NULL)
				why = "its staging";
			close(back);
		}
	}
	close(dir);
	return why;
}

void hull_stack_unstage(const char *at)
{
	static const char *const names[] = { "merged", "kept", "mem" };
	char                     path[4300];

	for (unsigned i = 0u; i < sizeof names / sizeof names[0]; i++) {
		snprintf(path, sizeof path, "%s/%s", at, names[i]);
		rmdir(path);
	}
	for (unsigned i = 0u; i < HULL_STACK_MAX; i++) {
		snprintf(path, sizeof path, "%s/lower%u", at, i);
		if (rmdir(path) != 0 && errno == ENOENT)
			break;
	}
	rmdir(at);
}

void hull_stack_unwork(const char *upper_work)
{
	char path[4300];

	if (upper_work == NULL)
		return;
	snprintf(path, sizeof path, "%s/work", upper_work);
	chmod(path, 0700);
}

void hull_stack_release(hull_stack *stack)
{
	for (unsigned i = 0u; i < stack->count; i++)
		free(stack->layer[i]);
	free(stack->upper);
	free(stack->upper_work);
	memset(stack, 0, sizeof *stack);
}
