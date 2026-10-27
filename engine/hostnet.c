/*
 * hostnet.c -- hull's own bridge on the host's network, and the tools that
 * make it, for a root core's (network bridge). See hostnet.h.
 */

#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <net/if.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/wait.h>
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
		execv(argv[0], (char *const *)argv);
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
