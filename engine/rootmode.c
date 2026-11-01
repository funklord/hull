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
#include <sys/socket.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <unistd.h>

#include "hull_engine.h"
#include "rootmode.h"

const char *hull_root_path(void)
{
	const char *given = getenv("HULL_ROOT_HELPER");

	return given != NULL ? given : HULL_ROOT_HELPER_PATH;
}

int hull_root_call(const char *const *args, const uint8_t *input, size_t length, int *fds,
                   unsigned fd_count, char *why, size_t cap)
{
	const char                *path = hull_root_path();
	char                      *argv[48];
	unsigned                   n = 0u;
	int                        said[2] = { -1, -1 };
	int                        fed[2] = { -1, -1 };
	int                        given[2] = { -1, -1 };
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
	for (unsigned i = 0u; fds != NULL && i < fd_count; i++)
		fds[i] = -1;
	if (fds != NULL && socketpair(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0, given) != 0) {
		snprintf(why, cap, "hull-root: %s", strerror(errno));
		close(said[0]);
		close(said[1]);
		if (input != NULL) {
			close(fed[0]);
			close(fed[1]);
		}
		return -1;
	}
	posix_spawn_file_actions_init(&actions);
	if (fds != NULL)
		posix_spawn_file_actions_adddup2(&actions, given[1], 3);
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
		if (fds != NULL) {
			close(given[0]);
			close(given[1]);
		}
		snprintf(why, cap, "hull-root could not be run: %s", strerror(errno));
		return -1;
	}
	posix_spawn_file_actions_destroy(&actions);
	close(said[1]);
	if (fds != NULL)
		close(given[1]);
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
	/* What it handed back, queued on the socket until it is read. */
	if (fds != NULL) {
		char            byte;
		struct iovec    v = { &byte, 1u };
		char            control[CMSG_SPACE(4u * sizeof(int))];
		struct msghdr   message = { .msg_iov = &v, .msg_iovlen = 1u,
		                            .msg_control = control, .msg_controllen = sizeof control };
		struct cmsghdr *c;

		if (recvmsg(given[0], &message, MSG_DONTWAIT | MSG_CMSG_CLOEXEC) > 0)
			for (c = CMSG_FIRSTHDR(&message); c != NULL; c = CMSG_NXTHDR(&message, c))
				if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS) {
					unsigned n = (unsigned)((c->cmsg_len - CMSG_LEN(0)) / sizeof(int));

					for (unsigned i = 0u; i < n; i++) {
						int fd;

						memcpy(&fd, CMSG_DATA(c) + i * sizeof(int), sizeof fd);
						if (i < fd_count)
							fds[i] = fd;
						else
							close(fd);
					}
				}
		close(given[0]);
	}
	if (WIFEXITED(status) && WEXITSTATUS(status) == 0)
		return 0;
	for (unsigned i = 0u; fds != NULL && i < fd_count; i++)
		if (fds[i] >= 0) {
			close(fds[i]);
			fds[i] = -1;
		}
	if (why[0] == '\0')
		snprintf(why, cap, "hull-root refused, saying nothing");
	return -1;
}
