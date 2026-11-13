/*
 * nic.c -- a machine's NICs on hull's segments, made in its own network
 * namespace, and their relay (nic.h). Moved from chroot's init, so that
 * hull-agent makes a container's the same way; it allocates nothing, a
 * chroot init's children being raw clones of a multithreaded core.
 */
#define _GNU_SOURCE

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/if_tun.h>
#include <net/if.h>
#include <net/route.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

#include "ip6.h"
#include "nic.h"

const char *hull_nic_join(const hull_nic *nics, unsigned n, int routed, int routed6, int *tap)
{
	for (unsigned i = 0u; i < n; i++) {
		const hull_nic *nic = &nics[i];
		struct ifreq    request = { 0 };
		int             fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);

		snprintf(request.ifr_name, sizeof request.ifr_name, "%s", nic->name);
		request.ifr_flags = IFF_TAP | IFF_NO_PI;
		if (fd < 0 || (tap[i] = open("/dev/net/tun", O_RDWR | O_CLOEXEC)) < 0 ||
		    ioctl(tap[i], TUNSETIFF, &request) != 0)
			return "a segment's NIC";
		request.ifr_hwaddr.sa_family = 1; /* ARPHRD_ETHER */
		memcpy(request.ifr_hwaddr.sa_data, nic->mac, 6u);
		if (ioctl(fd, SIOCSIFHWADDR, &request) != 0)
			return "a segment's NIC's address";
		/* Hull's address, where it addresses the segment, and the NIC up:
		 * the segment owns it, as it names it to the siblings. */
		if (nic->address[0] != '\0') {
			struct sockaddr_in *at = (struct sockaddr_in *)&request.ifr_addr;
			uint32_t            mask = 0xffffffffu << (32u - nic->prefix);

			memset(&request.ifr_addr, 0, sizeof request.ifr_addr);
			at->sin_family = AF_INET;
			if (inet_pton(AF_INET, nic->address, &at->sin_addr) != 1 ||
			    ioctl(fd, SIOCSIFADDR, &request) != 0)
				return "a segment's NIC's IPv4 address";
			at->sin_addr.s_addr = htonl(mask);
			if (ioctl(fd, SIOCSIFNETMASK, &request) != 0)
				return "a segment's NIC's netmask";
		}
		/* Up where hull addresses it, by either family or both. */
		if (nic->address[0] != '\0' || nic->address6[0] != '\0') {
			if (ioctl(fd, SIOCGIFFLAGS, &request) != 0)
				return "a segment's NIC's flags";
			request.ifr_flags |= IFF_UP;
			if (ioctl(fd, SIOCSIFFLAGS, &request) != 0)
				return "a segment's NIC up";
			if (nic->address6[0] != '\0' &&
			    hull_ip6_add(request.ifr_name, nic->address6, nic->prefix6) != 0)
				return "a segment's NIC's IPv6 address";
		}
		/* The first segment whose .1 is a gateway is the way out of a
		 * machine with none of its own (Appendix D), and the first whose
		 * ::1 is, by IPv6. */
		if (!routed6 && nic->gateway && nic->address6[0] != '\0') {
			if (hull_ip6_route(request.ifr_name, nic->address6, nic->prefix6) != 0)
				return "a segment's IPv6 route out through its ::1";
			routed6 = 1;
		}
		if (!routed && nic->gateway && nic->address[0] != '\0') {
			struct rtentry      route = { 0 };
			struct sockaddr_in *gateway = (struct sockaddr_in *)&route.rt_gateway;
			uint32_t            mask = 0xffffffffu << (32u - nic->prefix);
			struct in_addr      address;

			inet_pton(AF_INET, nic->address, &address);
			((struct sockaddr_in *)&route.rt_dst)->sin_family     = AF_INET;
			((struct sockaddr_in *)&route.rt_genmask)->sin_family = AF_INET;
			gateway->sin_family      = AF_INET;
			gateway->sin_addr.s_addr = htonl((ntohl(address.s_addr) & mask) + 1u);
			route.rt_flags = RTF_UP | RTF_GATEWAY;
			route.rt_dev   = request.ifr_name;
			if (ioctl(fd, SIOCADDRT, &route) != 0)
				return "a segment's route out through its .1";
			routed = 1;
		}
		close(fd);
	}
	return NULL;
}

/*
 * Frames between `tap`, a NIC's, and `port`, its segment's port on the
 * core's switch: in a process of the machine's own, which goes with its
 * namespace. Returns when a port closes.
 */
void hull_nic_relay(const int *tap, const int *port, unsigned n)
{
	static uint8_t from_port[HULL_NICS_MAX][4u + 65536u];
	size_t         held[HULL_NICS_MAX] = { 0u };
	uint8_t        frame[4u + 65536u];
	struct pollfd  fds[2u * HULL_NICS_MAX];

	for (unsigned i = 0u; i < n; i++) {
		fds[2u * i]      = (struct pollfd){ .fd = tap[i], .events = POLLIN };
		fds[2u * i + 1u] = (struct pollfd){ .fd = port[i], .events = POLLIN };
	}
	for (;;) {
		if (poll(fds, 2u * n, -1) < 0) {
			if (errno == EINTR)
				continue;
			return;
		}
		for (unsigned i = 0u; i < n; i++) {
			if (fds[2u * i].revents & POLLIN) {
				ssize_t got = read(tap[i], frame + 4, 65536u);

				if (got > 0) {
					size_t at = 0u;

					frame[0] = (uint8_t)((size_t)got >> 24);
					frame[1] = (uint8_t)((size_t)got >> 16);
					frame[2] = (uint8_t)((size_t)got >> 8);
					frame[3] = (uint8_t)got;
					while (at < (size_t)got + 4u) {
						ssize_t put = write(port[i], frame + at, (size_t)got + 4u - at);

						if (put <= 0)
							return;
						at += (size_t)put;
					}
				}
			}
			if (fds[2u * i + 1u].revents & (POLLIN | POLLHUP | POLLERR)) {
				ssize_t got = read(port[i], from_port[i] + held[i],
				                   sizeof from_port[i] - held[i]);

				if (got <= 0)
					return;
				held[i] += (size_t)got;
				for (;;) {
					size_t length;

					if (held[i] < 4u)
						break;
					length = (size_t)from_port[i][0] << 24 | (size_t)from_port[i][1] << 16 |
					         (size_t)from_port[i][2] << 8 | from_port[i][3];
					if (length > 65536u)
						return;
					if (held[i] < 4u + length)
						break;
					/* A frame the tap refuses, as one while the NIC is down, is
					 * dropped, as a wire drops it. */
					if (write(tap[i], from_port[i] + 4, length) < 0) {
					}
					memmove(from_port[i], from_port[i] + 4u + length, held[i] - 4u - length);
					held[i] -= 4u + length;
				}
			}
		}
	}
}
