/*
 * dial.c -- a core on another host, reached over ssh. See dial.h.
 *
 * Hull writes no network protocol of its own (section 4.10): the frontend
 * runs ssh, whose keys and encryption do the work, and at the far end
 * `hull dial` joins ssh's stdin and stdout to the core's socket there,
 * starting a core as a local frontend would, so hull's own protocol
 * crosses unchanged. Docker's `docker system dial-stdio` is the same
 * shape. It beats the `ssh -L` this entry first named: the far socket's
 * path, which rests on the far side's XDG_RUNTIME_DIR, need not be known
 * here, no socket file is left here, and a core is started there.
 */

#define _GNU_SOURCE

#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "dial.h"

int hull_dial_parse(const char *url, hull_dial_target *out, char *why, size_t cap)
{
	const char *at = url;
	const char *slash;
	const char *colon;
	const char *host;
	size_t      length;

	memset(out, 0, sizeof *out);
	if (strncmp(at, "ssh://", 6u) != 0) {
		snprintf(why, cap, "-H takes ssh://[USER@]HOST[:PORT][/DIR], not %s", url);
		return -1;
	}
	at += 6;
	slash = strchr(at, '/');
	length = slash != NULL ? (size_t)(slash - at) : strlen(at);
	host = memchr(at, '@', length);
	host = host != NULL ? host + 1 : at;
	colon = memchr(host, ':', length - (size_t)(host - at));
	if (colon != NULL) {
		size_t digits = length - (size_t)(colon + 1 - at);

		if (digits == 0u || digits >= sizeof out->port ||
		    strspn(colon + 1, "0123456789") < digits) {
			snprintf(why, cap, "%s: the port is not a number", url);
			return -1;
		}
		memcpy(out->port, colon + 1, digits);
		length = (size_t)(colon - at);
	}
	/* ssh would take a leading dash for an option. */
	if (length == 0u || host == at + length || length >= sizeof out->destination || at[0] == '-') {
		snprintf(why, cap, "%s: no host", url);
		return -1;
	}
	memcpy(out->destination, at, length);
	if (slash != NULL && slash[1] != '\0') {
		if (strlen(slash) >= sizeof out->dir) {
			snprintf(why, cap, "%s: the directory is too long", url);
			return -1;
		}
		strcpy(out->dir, slash);
	}
	return 0;
}

static int dial_with(const hull_dial_target *remote, const char *command, int batch)
{
	int         pair[2];
	pid_t       child;

	if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, pair) != 0)
		return -1;
	child = fork();
	if (child < 0) {
		close(pair[0]);
		close(pair[1]);
		return -1;
	}
	if (child == 0) {
		char *argv[18];
		int   n = 0;

		if (dup2(pair[1], 0) < 0 || dup2(pair[1], 1) < 0)
			_exit(126);
		argv[n++] = "ssh";
		argv[n++] = "-T";
		if (batch) {
			argv[n++] = "-oBatchMode=yes";
			argv[n++] = "-oConnectTimeout=15";
			argv[n++] = "-oServerAliveInterval=15";
			argv[n++] = "-oServerAliveCountMax=3";
		}
		if (remote->port[0] != '\0') {
			argv[n++] = "-p";
			argv[n++] = (char *)remote->port;
		}
		argv[n++] = "--";
		argv[n++] = (char *)remote->destination;
		argv[n++] = (char *)command;
		argv[n] = NULL;
		execvp("ssh", argv);
		fprintf(stderr, "hull: ssh: %s\n", strerror(errno));
		_exit(127);
	}
	close(pair[1]);
	return pair[0];
}

int hull_dial_host(const hull_dial_target *remote)
{
	const char *far = getenv("HULL_REMOTE_HULL");
	char        command[1100];

	if (far == NULL || far[0] == '\0')
		far = "hull";
	snprintf(command, sizeof command, "%s dial", far);
	return dial_with(remote, command, 0);
}

int hull_dial_broker(const hull_dial_target *remote)
{
	const char *far = getenv("HULL_REMOTE_HULL");
	char        command[2400];
	size_t      at;

	if (far == NULL || far[0] == '\0')
		far = "hull";
	/* A shell reads this at the far end, so the directory is quoted; a
	 * forced command there takes no notice of it. */
	at = (size_t)snprintf(command, sizeof command, "%s broker-dial --dir '", far);
	for (const char *c = remote->dir; *c != '\0' && at + 6u < sizeof command; c++) {
		if (*c == '\'') {
			memcpy(command + at, "'\\''", 4u);
			at += 4u;
		} else {
			command[at++] = *c;
		}
	}
	command[at++] = '\'';
	command[at] = '\0';
	return dial_with(remote, command, 1);
}

/* Every byte of `n` to `fd`: 0, or -1. */
static int write_all(int fd, const char *data, size_t n)
{
	while (n > 0u) {
		ssize_t wrote = write(fd, data, n);

		if (wrote < 0 && errno == EINTR)
			continue;
		if (wrote <= 0)
			return -1;
		data += wrote;
		n -= (size_t)wrote;
	}
	return 0;
}

int hull_dial_pump(int fd)
{
	struct pollfd fds[2] = { { 0, POLLIN, 0 }, { fd, POLLIN, 0 } };
	static char   buffer[65536];

	signal(SIGPIPE, SIG_IGN);
	for (;;) {
		if (poll(fds, 2u, -1) < 0) {
			if (errno == EINTR)
				continue;
			return 1;
		}
		if (fds[0].fd >= 0 && fds[0].revents != 0) {
			ssize_t n = read(0, buffer, sizeof buffer);

			if (n < 0 && errno == EINTR)
				continue;
			/* The frontend gone: the core reads its end, as of a local one. */
			if (n <= 0) {
				shutdown(fd, SHUT_WR);
				fds[0].fd = -1;
			} else if (write_all(fd, buffer, (size_t)n) != 0) {
				return 1;
			}
		}
		if (fds[1].revents != 0) {
			ssize_t n = read(fd, buffer, sizeof buffer);

			if (n < 0 && errno == EINTR)
				continue;
			if (n <= 0)
				return 0;
			if (write_all(1, buffer, (size_t)n) != 0)
				return 1;
		}
	}
}
