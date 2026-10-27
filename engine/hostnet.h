/*
 * hostnet.h -- what a root core does to the host's own network for
 * (network bridge): hull's own bridge, hull0, made on first use as docker
 * makes docker0, and the tools that make it. Shared by the chroot engine,
 * which puts a veth on a bridge, and the qemu engine, which puts a tap.
 */

#ifndef HULL_HOSTNET_H
#define HULL_HOSTNET_H

#include <stddef.h>

/* hull's own bridge, its gateway in a /15 whose lower half holds the
 * machines: each 172.30.x.y for its 127.107.x.y (section 8.1), which the
 * core keeps unique, so no two live machines share one. */
#define HULL_BRIDGE         "hull0"
#define BRIDGE_GATEWAY      "172.31.0.1"
#define BRIDGE_PREFIX       15u
#define BRIDGE_NETWORK      "172.30.0.0/15"

/* ip or nft by its absolute path, since /usr/sbin may not be on the
 * user's PATH; NULL where it is not installed. */
const char *hull_hostnet_ip(void);
const char *hull_hostnet_nft(void);

/* `argv` run, its stdout and stderr on a pipe: 0, or -1 with what it said
 * in `why`. */
int hull_hostnet_run(const char *const *argv, char *why, size_t cap);

/* hull0 made where it is not there yet: the bridge, its gateway address,
 * forwarding on, and nft's masquerade of BRIDGE_NETWORK going anywhere
 * but itself; left there after, as docker0 is. NULL, or why in `why`. */
const char *hull_hostnet_bridge(char *why, size_t cap);

/* The machine's 172.30.x.y on hull0, for its 127.107.x.y: 0, or -1 where
 * `address` is no 127.107 address. */
int hull_hostnet_address(const char *address, char out[16]);

/* The host's resolv.conf's nameservers that are not its loopback, as a
 * resolv.conf of their own: a machine on a bridge reaches the host's
 * resolvers only where they are not its loopback, which docker meets the
 * same way. NULL, errno 0, where there are none; ENOMEM where memory ran
 * out. */
char *hull_hostnet_resolvers(void);

#endif /* HULL_HOSTNET_H */
