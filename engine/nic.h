/*
 * nic.h -- a machine's NICs on hull's segments (Appendix D) made inside
 * its network namespace: each a tap with its MAC, hull's address where hull
 * addresses the segment, and a way out through a segment's .1; and the
 * relay between the taps and their ports on the core's switch. chroot's
 * init makes them in the machine's namespace, and hull-agent in a
 * container's, as its root (docker).
 */
#ifndef HULL_ENGINE_NIC_H
#define HULL_ENGINE_NIC_H

#include <stdint.h>

#define HULL_NICS_MAX 8u

typedef struct hull_nic {
	char     name[16];       /* eth0, eth1, ... */
	uint8_t  mac[6];
	char     address[16];    /* hull's IPv4 one on it, or "" */
	unsigned prefix;
	char     address6[46];   /* its IPv6 one, or "" */
	unsigned prefix6;
	int      gateway;        /* the segment's .1 (and ::1) a way out */
} hull_nic;

/* Each of `nics` a tap, its descriptor into `tap`, left down unless hull
 * addresses it; the first gateway the default route where `routed` (and
 * for IPv6 `routed6`) says the machine has none of its own. NULL, or what
 * could not be done, errno set. Allocates nothing. */
const char *hull_nic_join(const hull_nic *nics, unsigned n, int routed, int routed6, int *tap);

/* Frames between each tap and its port, framed after their length as 32
 * bits in network order, as qemu's -netdev stream does. Returns when a
 * port closes. Allocates nothing. */
void hull_nic_relay(const int *tap, const int *port, unsigned n);

/* The same with each port read from `in` and written to `out`, a pipe's
 * two ends or a session's stdin and stdout. */
void hull_nic_relay_io(const int *tap, const int *in, const int *out, unsigned n);

#endif /* HULL_ENGINE_NIC_H */
