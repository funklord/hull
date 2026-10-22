/*
 * runtime_test.c -- cli/runtime.c's choice of a runtime directory, on
 * directories of the test's own under $BUILD_DIR/test/runtime, never the
 * account's own /run/user: what is taken, what is made and how, and what
 * is passed over. Another user's directory cannot be made without root,
 * so that refusal is not tried here.
 */

#define _GNU_SOURCE

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "runtime.h"

static unsigned cases;
static unsigned failures;
static char     root[4096];

static void fail(const char *what, const char *detail)
{
	failures++;
	printf("FAIL %s: %s\n", what, detail);
}

static void at(char *out, size_t cap, const char *name)
{
	snprintf(out, cap, "%s/%s", root, name);
}

static void fresh(const char *name)
{
	char path[4200];
	char command[8600];

	at(path, sizeof path, name);
	snprintf(command, sizeof command, "rm -rf '%s' && mkdir -p '%s'", path, path);
	if (system(command) != 0)
		fail("setting up", path);
}

/* The choice among `bases`, wanted to be `want` (a name under the root, or
 * NULL for none), and the reason, where none, to hold `reason`. */
static void choose(const char *what, const hull_runtime_base *bases, size_t n, const char *want,
                   const char *reason)
{
	char out[4096] = "";
	char why[512] = "";
	char expected[4200];
	int  got = hull_runtime_choose(getuid(), bases, n, out, sizeof out, why, sizeof why);

	cases++;
	if (want == NULL) {
		if (got == 0)
			fail(what, out);
		else if (reason != NULL && strstr(why, reason) == NULL)
			fail(what, why);
		return;
	}
	at(expected, sizeof expected, want);
	if (got != 0 || strcmp(out, expected) != 0)
		fail(what, got != 0 ? why : out);
}

int main(void)
{
	const char *build = getenv("BUILD_DIR");
	char        run[4200];
	char        shm[4200];
	char        tmp[4200];
	char        mine[4300];
	char        text[64];
	struct stat st;
	uid_t       uid = getuid();

	if (build == NULL || realpath(build, root) == NULL) {
		printf("runtime_test: BUILD_DIR names no directory\n");
		return 2;
	}
	strncat(root, "/test/runtime", sizeof root - strlen(root) - 1u);
	fresh("");
	fresh("run");
	fresh("shm");
	fresh("tmp");
	at(run, sizeof run, "run");
	at(shm, sizeof shm, "shm");
	at(tmp, sizeof tmp, "tmp");
	{
		/* shm's test copy is not in memory, so it is asked of no base but
		 * the one case that wants it refused. */
		hull_runtime_base bases[3] = { { run, 0, 0 }, { shm, 1, 0 }, { tmp, 1, 0 } };
		hull_runtime_base memory[1] = { { tmp, 1, 1 } };

		/* /run/user/UID as a login makes it: taken, nothing made. */
		snprintf(mine, sizeof mine, "%s/%u", run, (unsigned)uid);
		mkdir(mine, 0700);
		snprintf(text, sizeof text, "run/%u", (unsigned)uid);
		choose("a login's own", bases, 3u, text, NULL);
		cases++;
		snprintf(mine, sizeof mine, "%s/hull-%u", shm, (unsigned)uid);
		if (lstat(mine, &st) == 0)
			fail("a login's own", "a directory was made though one was there");

		/* One others may enter is not this user's to tighten: passed
		 * over, and the next made, 0700. */
		snprintf(mine, sizeof mine, "%s/%u", run, (unsigned)uid);
		chmod(mine, 0755);
		snprintf(text, sizeof text, "shm/hull-%u", (unsigned)uid);
		choose("one others may enter, passed over", bases, 3u, text, NULL);
		cases++;
		snprintf(mine, sizeof mine, "%s/hull-%u", shm, (unsigned)uid);
		if (stat(mine, &st) != 0 || (st.st_mode & 0777) != 0700)
			fail("a made one", "not 0700");
		rmdir(mine);
		snprintf(mine, sizeof mine, "%s/%u", run, (unsigned)uid);
		rmdir(mine);

		/* A symlink where one would be made, to where another could
		 * read, is passed over; so is a file. */
		snprintf(mine, sizeof mine, "%s/hull-%u", shm, (unsigned)uid);
		symlink(tmp, mine);
		snprintf(text, sizeof text, "tmp/hull-%u", (unsigned)uid);
		choose("a symlink, passed over", bases, 3u, text, NULL);
		unlink(mine);
		snprintf(mine, sizeof mine, "%s/hull-%u", tmp, (unsigned)uid);
		rmdir(mine);
		snprintf(mine, sizeof mine, "%s/hull-%u", shm, (unsigned)uid);
		{
			FILE *f = fopen(mine, "w");

			if (f != NULL)
				fclose(f);
		}
		snprintf(text, sizeof text, "tmp/hull-%u", (unsigned)uid);
		choose("a file, passed over", bases, 3u, text, NULL);
		unlink(mine);

		/* One of the user's own that others may enter, where one is made:
		 * tightened, and taken. */
		snprintf(mine, sizeof mine, "%s/hull-%u", tmp, (unsigned)uid);
		chmod(mine, 0755);
		snprintf(text, sizeof text, "tmp/hull-%u", (unsigned)uid);
		choose("a loose one of the user's, tightened", bases + 2, 1u, text, NULL);
		cases++;
		if (stat(mine, &st) != 0 || (st.st_mode & 0777) != 0700)
			fail("a loose one of the user's, tightened", "not 0700");

		/* Where the base must be in memory and is not: passed over. */
		choose("a base not in memory, passed over", memory, 1u, NULL, "not in memory");

		/* None that would do: -1, and the reason. */
		{
			hull_runtime_base none[1] = { { run, 0, 0 } };

			choose("none", none, 1u, NULL, "No such file or directory");
		}
	}
	{
		char command[4300];

		snprintf(command, sizeof command, "rm -rf '%s'", root);
		cases++;
		if (system(command) != 0)
			fail("removing the test's directory", root);
	}
	printf("runtime: %u cases, %u failed\n", cases, failures);
	return failures != 0;
}
