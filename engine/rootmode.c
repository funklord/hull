/*
 * rootmode.c -- running hull-root. See rootmode.h.
 */

#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "hull_engine.h"
#include "rootmode.h"

const char *hull_root_path(void)
{
	const char *given = getenv("HULL_ROOT_HELPER");

	return given != NULL ? given : HULL_ROOT_HELPER_PATH;
}

int hull_root_call(const char *const *args, const uint8_t *input, size_t length, char *why,
                   size_t cap)
{
	const char                *path = hull_root_path();
	char                      *argv[48];
	unsigned                   n = 0u;
	int                        said[2] = { -1, -1 };
	int                        fed[2] = { -1, -1 };
	pid_t                      pid;
	int                        status = 0;
	size_t                     got = 0u;
	ssize_t                    r;
	posix_spawn_file_actions_t actions;

	argv[n++] = (char *)"hull-root";
	for (unsigned i = 0u; args[i] != NULL && n < 47u; i++)
		argv[n++] = (char *)args[i];
	argv[n] = NULL;
	why[0] = '\0';
	if (access(path, X_OK) != 0) {
		snprintf(why, cap, "root mode is not installed on this host: no %s (make install-root, "
		         "as root)", path);
		return -1;
	}
	if (pipe2(said, O_CLOEXEC) != 0 || (input != NULL && pipe2(fed, O_CLOEXEC) != 0)) {
		snprintf(why, cap, "hull-root: %s", strerror(errno));
		if (said[0] >= 0) {
			close(said[0]);
			close(said[1]);
		}
		return -1;
	}
	posix_spawn_file_actions_init(&actions);
	posix_spawn_file_actions_adddup2(&actions, said[1], 2);
	posix_spawn_file_actions_addopen(&actions, 1, "/dev/null", O_WRONLY, 0);
	if (input != NULL)
		posix_spawn_file_actions_adddup2(&actions, fed[0], 0);
	else
		posix_spawn_file_actions_addopen(&actions, 0, "/dev/null", O_RDONLY, 0);
	/* The caller's environment, which a setuid hull-root reads nothing of;
	 * run as the user, as the suites run it, it reads its listing's and
	 * log's paths there. */
	if (posix_spawn(&pid, path, &actions, NULL, argv, environ) != 0) {
		posix_spawn_file_actions_destroy(&actions);
		close(said[0]);
		close(said[1]);
		if (input != NULL) {
			close(fed[0]);
			close(fed[1]);
		}
		snprintf(why, cap, "hull-root could not be run: %s", strerror(errno));
		return -1;
	}
	posix_spawn_file_actions_destroy(&actions);
	close(said[1]);
	/* The plan whole before anything is read back: hull-root reads all of
	 * it before it says a word. A helper gone early is a refusal. */
	if (input != NULL) {
		void (*was)(int) = signal(SIGPIPE, SIG_IGN);

		close(fed[0]);
		for (size_t at = 0u; at < length;) {
			ssize_t wrote = write(fed[1], input + at, length - at);

			if (wrote < 0 && errno == EINTR)
				continue;
			if (wrote <= 0)
				break;
			at += (size_t)wrote;
		}
		close(fed[1]);
		signal(SIGPIPE, was);
	}
	while (got + 1u < cap && (r = read(said[0], why + got, cap - 1u - got)) > 0)
		got += (size_t)r;
	why[got] = '\0';
	while (got > 0u && why[got - 1u] == '\n')
		why[--got] = '\0';
	close(said[0]);
	while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
		;
	if (WIFEXITED(status) && WEXITSTATUS(status) == 0)
		return 0;
	if (why[0] == '\0')
		snprintf(why, cap, "hull-root refused, saying nothing");
	return -1;
}
