/*
 * hostnet.c -- hull's own bridge on the host's network, and the tools that
 * make it, for a root core's (network bridge). See hostnet.h.
 */

#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <arpa/inet.h>
#include <net/if.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "hostnet.h"

/* The first of `paths` that is there to run. */
static const char *tool(const char *const *paths)
{
	for (; *paths != NULL; paths++)
		if (access(*paths, X_OK) == 0)
			return *paths;
	return NULL;
}

int hull_hostnet_run(const char *const *argv, char *why, size_t cap)
{
	int     said[2];
	int     status;
	pid_t   pid;
	ssize_t got;

	if (argv[0] == NULL) {
		snprintf(why, cap, "ip or nft, which (network bridge) needs, is not installed");
		return -1;
	}
	if (pipe2(said, O_CLOEXEC) != 0) {
		snprintf(why, cap, "%s: %s", argv[0], strerror(errno));
		return -1;
	}
	pid = fork();
	if (pid == 0) {
		int null = open("/dev/null", O_RDWR);

		if (null < 0 || dup2(null, 0) < 0 || dup2(said[1], 1) < 0 || dup2(said[1], 2) < 0)
			_exit(126);
		syscall(SYS_close_range, 3u, ~0u, 0u);
		execvp(argv[0], (char *const *)argv);
		_exit(127);
	}
	close(said[1]);
	if (pid < 0) {
		close(said[0]);
		snprintf(why, cap, "%s: %s", argv[0], strerror(errno));
		return -1;
	}
	got = read(said[0], why, cap - 1u);
	close(said[0]);
	while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
		;
	if (WIFEXITED(status) && WEXITSTATUS(status) == 0)
		return 0;
	if (got <= 0)
		snprintf(why, cap, "%s failed", argv[0]);
	else {
		why[got] = '\0';
		while (got > 0 && (why[got - 1] == '\n' || why[got - 1] == '\r'))
			why[--got] = '\0';
	}
	return -1;
}

const char *hull_hostnet_ip(void)
{
	static const char *const ips[] = { "/usr/sbin/ip", "/sbin/ip", "/usr/bin/ip", "/bin/ip", NULL };

	return tool(ips);
}

const char *hull_hostnet_nft(void)
{
	static const char *const nfts[] = { "/usr/sbin/nft", "/sbin/nft", "/usr/bin/nft", NULL };

	return tool(nfts);
}

int hull_hostnet_address(const char *address, char out[16])
{
	unsigned a, b, x, y;

	if (address == NULL || sscanf(address, "%u.%u.%u.%u", &a, &b, &x, &y) != 4 || a != 127u ||
	    b != 107u || x > 255u || y < 1u || y > 254u)
		return -1;
	snprintf(out, 16u, "172.30.%u.%u", x, y);
	return 0;
}

const char *hull_hostnet_bridge(char *why, size_t cap)
{
	static pthread_mutex_t made = PTHREAD_MUTEX_INITIALIZER;
	const char            *ip = hull_hostnet_ip();
	const char            *nft = hull_hostnet_nft();
	int                    failed = 0;

	pthread_mutex_lock(&made);
	if (if_nametoindex(HULL_BRIDGE) == 0) {
		const char *add[] = { ip, "link", "add", HULL_BRIDGE, "type", "bridge", NULL };
		const char *addr[] = { ip, "addr", "add", BRIDGE_GATEWAY "/15", "dev", HULL_BRIDGE, NULL };
		const char *up[] = { ip, "link", "set", HULL_BRIDGE, "up", NULL };

		failed = hull_hostnet_run(add, why, cap) != 0 || hull_hostnet_run(addr, why, cap) != 0 ||
		         hull_hostnet_run(up, why, cap) != 0;
	}
	if (!failed) {
		const char *listed[] = { nft, "list", "table", "ip", "hull", NULL };
		char        quiet[200];

		if (hull_hostnet_run(listed, quiet, sizeof quiet) != 0) {
			const char *table[] = { nft, "add", "table", "ip", "hull", NULL };
			const char *chain[] = { nft, "add", "chain", "ip", "hull", "post",
			                        "{ type nat hook postrouting priority 100 ; }", NULL };
			const char *rule[] = { nft, "add", "rule", "ip", "hull", "post", "ip", "saddr",
			                       BRIDGE_NETWORK, "oifname", "!=", HULL_BRIDGE, "masquerade",
			                       NULL };

			failed = hull_hostnet_run(table, why, cap) != 0 ||
			         hull_hostnet_run(chain, why, cap) != 0 ||
			         hull_hostnet_run(rule, why, cap) != 0;
		}
	}
	if (!failed) {
		int fd = open("/proc/sys/net/ipv4/ip_forward", O_WRONLY | O_CLOEXEC);

		if (fd < 0 || write(fd, "1", 1u) != 1) {
			snprintf(why, cap, "forwarding cannot be turned on: %s", strerror(errno));
			failed = 1;
		}
		if (fd >= 0)
			close(fd);
	}
	pthread_mutex_unlock(&made);
	return failed ? why : NULL;
}

char *hull_hostnet_resolvers(void)
{
	FILE  *conf = fopen("/etc/resolv.conf", "r");
	char   line[256];
	char  *text = NULL;
	size_t at = 0u;

	errno = 0;
	while (conf != NULL && fgets(line, sizeof line, conf) != NULL) {
		char   server[64];
		char  *grown;
		size_t need;

		if (sscanf(line, "nameserver %63s", server) != 1 || strncmp(server, "127.", 4u) == 0 ||
		    strcmp(server, "::1") == 0)
			continue;
		need = at + strlen(server) + 13u;
		if ((grown = realloc(text, need)) == NULL) {
			free(text);
			fclose(conf);
			errno = ENOMEM;
			return NULL;
		}
		text = grown;
		at += (size_t)snprintf(text + at, need - at, "nameserver %s\n", server);
	}
	if (conf != NULL)
		fclose(conf);
	if (text == NULL)
		errno = 0;
	return text;
}

/* --- the relay ------------------------------------------------------------ */

#define RELAY_PENDING 64u    /* questions in flight at once */
#define RELAY_WAIT_MS 5000   /* how long one waits for its answer */

typedef struct pending {
	int                     fd;      /* to the upstream, or -1 */
	struct sockaddr_storage asker;
	socklen_t               asker_length;
	uint64_t                deadline;
} pending;

static int                     relay_fd = -1;
static struct sockaddr_storage upstream;
static socklen_t               upstream_length;

static uint64_t relay_now(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return (uint64_t)t.tv_sec * 1000u + (uint64_t)t.tv_nsec / 1000000u;
}

/* The first nameserver of the host's resolv.conf, any, its loopback too:
 * the relay is on the host, where that is reachable. 0, or -1. */
static int first_resolver(void)
{
	FILE *conf = fopen("/etc/resolv.conf", "r");
	char  line[256];
	char  server[64];
	int   found = 0;

	while (conf != NULL && !found && fgets(line, sizeof line, conf) != NULL) {
		struct sockaddr_in  *v4 = (struct sockaddr_in *)&upstream;
		struct sockaddr_in6 *v6 = (struct sockaddr_in6 *)&upstream;

		if (sscanf(line, "nameserver %63s", server) != 1)
			continue;
		memset(&upstream, 0, sizeof upstream);
		if (inet_pton(AF_INET, server, &v4->sin_addr) == 1) {
			v4->sin_family  = AF_INET;
			v4->sin_port    = htons(53);
			upstream_length = sizeof *v4;
			found = 1;
		} else if (inet_pton(AF_INET6, server, &v6->sin6_addr) == 1) {
			v6->sin6_family = AF_INET6;
			v6->sin6_port   = htons(53);
			upstream_length = sizeof *v6;
			found = 1;
		}
	}
	if (conf != NULL)
		fclose(conf);
	return found ? 0 : -1;
}

/* Questions in on relay_fd, each passed up on a socket of its own, so its
 * answer comes back on that one and goes to whoever asked; one unanswered
 * in RELAY_WAIT_MS is dropped, as a lost datagram is, and the asker's
 * resolver asks again. */
static void *relay(void *arg)
{
	pending table[RELAY_PENDING];
	uint8_t packet[4096];

	(void)arg;
	for (unsigned i = 0u; i < RELAY_PENDING; i++)
		table[i].fd = -1;
	for (;;) {
		struct pollfd fds[1u + RELAY_PENDING];
		unsigned      which[1u + RELAY_PENDING];
		nfds_t        n = 1u;
		uint64_t      now = relay_now();

		fds[0].fd     = relay_fd;
		fds[0].events = POLLIN;
		for (unsigned i = 0u; i < RELAY_PENDING; i++) {
			if (table[i].fd < 0)
				continue;
			if (now >= table[i].deadline) {
				close(table[i].fd);
				table[i].fd = -1;
				continue;
			}
			fds[n].fd     = table[i].fd;
			fds[n].events = POLLIN;
			which[n++]    = i;
		}
		if (poll(fds, n, 1000) <= 0)
			continue;
		for (nfds_t k = 1u; k < n; k++) {
			pending *p = &table[which[k]];
			ssize_t  got;

			if ((fds[k].revents & (POLLIN | POLLERR | POLLHUP)) == 0)
				continue;
			got = recv(p->fd, packet, sizeof packet, 0);
			if (got > 0)
				sendto(relay_fd, packet, (size_t)got, 0, (struct sockaddr *)&p->asker,
				       p->asker_length);
			close(p->fd);
			p->fd = -1;
		}
		if ((fds[0].revents & POLLIN) != 0) {
			pending *p = NULL;
			ssize_t  got;
			struct sockaddr_storage asker;
			socklen_t               length = sizeof asker;

			got = recvfrom(relay_fd, packet, sizeof packet, 0, (struct sockaddr *)&asker, &length);
			if (got < 12)
				continue;
			for (unsigned i = 0u; i < RELAY_PENDING && p == NULL; i++)
				if (table[i].fd < 0)
					p = &table[i];
			if (p == NULL)
				continue;    /* full: dropped, and asked again */
			p->fd = socket(upstream.ss_family, SOCK_DGRAM | SOCK_CLOEXEC, 0);
			if (p->fd < 0)
				continue;
			if (connect(p->fd, (struct sockaddr *)&upstream, upstream_length) != 0 ||
			    send(p->fd, packet, (size_t)got, 0) != got) {
				close(p->fd);
				p->fd = -1;
				continue;
			}
			p->asker        = asker;
			p->asker_length = length;
			p->deadline     = relay_now() + RELAY_WAIT_MS;
		}
	}
	return NULL;
}

const char *hull_hostnet_relay(char *why, size_t cap)
{
	static pthread_mutex_t started = PTHREAD_MUTEX_INITIALIZER;
	struct sockaddr_in     at = { .sin_family = AF_INET, .sin_port = htons(53) };
	const char            *reason = NULL;
	pthread_t              thread;
	int                    one = 1;

	pthread_mutex_lock(&started);
	if (relay_fd < 0) {
		inet_pton(AF_INET, BRIDGE_GATEWAY, &at.sin_addr);
		if (first_resolver() != 0) {
			snprintf(why, cap, "the host's resolv.conf names no nameserver for hull0's relay");
			reason = why;
		} else if ((relay_fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0)) < 0 ||
		           setsockopt(relay_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one) != 0 ||
		           bind(relay_fd, (struct sockaddr *)&at, sizeof at) != 0 ||
		           pthread_create(&thread, NULL, relay, NULL) != 0) {
			snprintf(why, cap, "hull0's DNS relay: %s", strerror(errno));
			if (relay_fd >= 0)
				close(relay_fd);
			relay_fd = -1;
			reason = why;
		} else {
			pthread_detach(thread);
		}
	}
	pthread_mutex_unlock(&started);
	return reason;
}
