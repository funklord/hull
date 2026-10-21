/*
 * nss_hull.c -- glibc's hosts database for section 8.1's names: `web.hull`
 * is the machine web's address, 127.107.x.y, for any program on the host
 * that looks names up, as `curl web.hull:8080` and `psql -h db.hull` do.
 *
 * The core keeps `hull/hosts` in its runtime directory, a line per
 * machine it holds, `ID ADDRESS`, replaced whole with a rename on every
 * change. This module reads the calling user's: $XDG_RUNTIME_DIR, through
 * secure_getenv(), so that a setuid program is not told where to read, or
 * else /run/user/UID. A user's machines are theirs, and another user's
 * names are not answered.
 *
 * Only a name ending in `.hull` is answered, with or without a final dot
 * and in any case; any other is not found here, and the next service in
 * nsswitch.conf is asked. The reverse, an address of a machine's, is its
 * `ID.hull`. IPv4 only, as the addresses are.
 *
 * Installing it is the one step that needs root: the library in the
 * system's library directory, and `hull` on the hosts line of
 * /etc/nsswitch.conf, before `dns`. `make install-nss` does the first.
 */

#define _GNU_SOURCE

#include <errno.h>
#include <netdb.h>
#include <nss.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

/* Only what is marked EXPORT leaves the library: the Makefile compiles it
 * with hidden visibility, and the directive below says so to fmake. */
/*! @cflags -fvisibility=hidden */
#define EXPORT __attribute__((visibility("default")))

#define MAX_LINES 4096  /* far more machines than a core holds */

/* The machine `id` (a DNS label, at most 63) from `name`, if it is one of
 * ours: lower-cased, without `.hull` or the final dot. */
static int machine_of(const char *name, char id[64])
{
	size_t n = name != NULL ? strlen(name) : 0u;

	if (n > 0u && name[n - 1u] == '.')
		n--;
	if (n <= 5u || strncasecmp(name + n - 5u, ".hull", 5u) != 0)
		return 0;
	n -= 5u;
	if (n > 63u)
		return 0;
	for (size_t i = 0u; i < n; i++) {
		char c = name[i];

		if (c >= 'A' && c <= 'Z')
			c = (char)(c - 'A' + 'a');
		if (!(c >= 'a' && c <= 'z') && !(c >= '0' && c <= '9') && c != '-')
			return 0;
		id[i] = c;
	}
	id[n] = '\0';
	return 1;
}

static FILE *open_hosts(void)
{
	char        path[4096];
	const char *runtime = secure_getenv("XDG_RUNTIME_DIR");
	int         n;

	if (runtime != NULL && runtime[0] == '/')
		n = snprintf(path, sizeof path, "%s/hull/hosts", runtime);
	else
		n = snprintf(path, sizeof path, "/run/user/%u/hull/hosts", (unsigned)getuid());
	if (n < 0 || (size_t)n >= sizeof path)
		return NULL;
	return fopen(path, "re");
}

/* The machine's line: by id, or with `id` empty, by address. Its id and
 * address into the arguments, or 0. */
static int look_up(char id[64], struct in_addr *address)
{
	FILE *hosts = open_hosts();
	char  line[128];
	int   found = 0;

	if (hosts == NULL)
		return 0;
	for (unsigned count = 0u; !found && count < MAX_LINES && fgets(line, sizeof line, hosts) != NULL;
	     count++) {
		char           name[64];
		char           text[16];
		struct in_addr at;

		if (sscanf(line, "%63s %15s", name, text) != 2 || inet_pton(AF_INET, text, &at) != 1)
			continue;
		if (id[0] != '\0' ? strcmp(name, id) == 0 : at.s_addr == address->s_addr) {
			strcpy(id, name);
			*address = at;
			found = 1;
		}
	}
	fclose(hosts);
	return found;
}

static enum nss_status not_found(int *errnop, int *herrnop)
{
	*errnop  = ENOENT;
	*herrnop = HOST_NOT_FOUND;
	return NSS_STATUS_NOTFOUND;
}

static enum nss_status too_small(int *errnop, int *herrnop)
{
	*errnop  = ERANGE;
	*herrnop = NETDB_INTERNAL;
	return NSS_STATUS_TRYAGAIN;
}

/* `id.hull` and its address as a hostent, all in `buffer`: the address
 * list and the empty alias list first, pointer-aligned, then the address,
 * then the name. */
static enum nss_status fill(const char *id, struct in_addr address, struct hostent *host,
                            char *buffer, size_t buflen, int *errnop, int *herrnop)
{
	size_t pointers = 3u * sizeof (char *);
	size_t name_len = strlen(id) + sizeof ".hull";
	char **list;
	char  *at;

	if (buflen < pointers + sizeof address + name_len)
		return too_small(errnop, herrnop);
	list = (char **)(void *)buffer;
	at   = buffer + pointers;
	memcpy(at, &address, sizeof address);
	list[0] = at;
	list[1] = NULL;
	list[2] = NULL;
	at += sizeof address;
	snprintf(at, name_len, "%s.hull", id);
	host->h_name      = at;
	host->h_aliases   = &list[2];
	host->h_addrtype  = AF_INET;
	host->h_length    = (int)sizeof address;
	host->h_addr_list = &list[0];
	return NSS_STATUS_SUCCESS;
}

EXPORT enum nss_status _nss_hull_gethostbyname4_r(const char *name, struct gaih_addrtuple **pat,
                                                  char *buffer, size_t buflen, int *errnop,
                                                  int *herrnop, int32_t *ttlp)
{
	struct gaih_addrtuple *tuple;
	struct in_addr         address;
	char                   id[64];
	size_t                 name_len;

	if (!machine_of(name, id) || !look_up(id, &address))
		return not_found(errnop, herrnop);
	name_len = strlen(id) + sizeof ".hull";
	if (buflen < sizeof *tuple + name_len)
		return too_small(errnop, herrnop);
	tuple = (struct gaih_addrtuple *)(void *)buffer;
	memset(tuple, 0, sizeof *tuple);
	tuple->name   = buffer + sizeof *tuple;
	snprintf(tuple->name, name_len, "%s.hull", id);
	tuple->family = AF_INET;
	memcpy(tuple->addr, &address, sizeof address);
	*pat = tuple;
	if (ttlp != NULL)
		*ttlp = 0;
	return NSS_STATUS_SUCCESS;
}

EXPORT enum nss_status _nss_hull_gethostbyname3_r(const char *name, int af, struct hostent *host,
                                                  char *buffer, size_t buflen, int *errnop,
                                                  int *herrnop, int32_t *ttlp, char **canonp)
{
	struct in_addr  address;
	char            id[64];
	enum nss_status status;

	if (af != AF_INET || !machine_of(name, id) || !look_up(id, &address))
		return not_found(errnop, herrnop);
	status = fill(id, address, host, buffer, buflen, errnop, herrnop);
	if (status == NSS_STATUS_SUCCESS) {
		if (ttlp != NULL)
			*ttlp = 0;
		if (canonp != NULL)
			*canonp = host->h_name;
	}
	return status;
}

EXPORT enum nss_status _nss_hull_gethostbyname2_r(const char *name, int af, struct hostent *host,
                                                  char *buffer, size_t buflen, int *errnop,
                                                  int *herrnop)
{
	return _nss_hull_gethostbyname3_r(name, af, host, buffer, buflen, errnop, herrnop, NULL, NULL);
}

EXPORT enum nss_status _nss_hull_gethostbyname_r(const char *name, struct hostent *host,
                                                 char *buffer, size_t buflen, int *errnop,
                                                 int *herrnop)
{
	return _nss_hull_gethostbyname3_r(name, AF_INET, host, buffer, buflen, errnop, herrnop, NULL,
	                                  NULL);
}

EXPORT enum nss_status _nss_hull_gethostbyaddr2_r(const void *addr, socklen_t len, int af,
                                                  struct hostent *host, char *buffer,
                                                  size_t buflen, int *errnop, int *herrnop,
                                                  int32_t *ttlp)
{
	struct in_addr address;
	char           id[64] = "";

	if (af != AF_INET || len != sizeof address)
		return not_found(errnop, herrnop);
	memcpy(&address, addr, sizeof address);
	/* 127.107.0.0/16, section 8.1's: nothing else is a machine's. */
	if ((ntohl(address.s_addr) >> 16) != ((127u << 8) | 107u) || !look_up(id, &address))
		return not_found(errnop, herrnop);
	if (ttlp != NULL)
		*ttlp = 0;
	return fill(id, address, host, buffer, buflen, errnop, herrnop);
}

EXPORT enum nss_status _nss_hull_gethostbyaddr_r(const void *addr, socklen_t len, int af,
                                                 struct hostent *host, char *buffer,
                                                 size_t buflen, int *errnop, int *herrnop)
{
	return _nss_hull_gethostbyaddr2_r(addr, len, af, host, buffer, buflen, errnop, herrnop, NULL);
}
