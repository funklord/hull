/*
 * ip6.c -- an IPv6 address given to an interface (ip6.h).
 */

#define _GNU_SOURCE

#include "ip6.h"

#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <linux/if_addr.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <net/if.h>
#include <sys/socket.h>

int hull_ip6_add(const char *name, const char *address, unsigned prefix)
{
	struct {
		struct nlmsghdr  header;
		struct ifaddrmsg message;
		char             attributes[64];
	} request;
	struct {
		struct nlmsghdr  header;
		struct nlmsgerr  error;
	} reply;
	struct sockaddr_nl kernel = { .nl_family = AF_NETLINK };
	struct in6_addr    parsed;
	struct rtattr     *attribute;
	unsigned           index = if_nametoindex(name);
	uint32_t           flags = IFA_F_NODAD;
	ssize_t            n;
	int                fd;

	if (index == 0u)
		return -1;
	if (inet_pton(AF_INET6, address, &parsed) != 1 || prefix > 128u) {
		errno = EINVAL;
		return -1;
	}
	memset(&request, 0, sizeof request);
	request.header.nlmsg_len   = NLMSG_LENGTH(sizeof request.message);
	request.header.nlmsg_type  = RTM_NEWADDR;
	request.header.nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK | NLM_F_CREATE | NLM_F_REPLACE;
	request.header.nlmsg_seq   = 1u;
	request.message.ifa_family    = AF_INET6;
	request.message.ifa_prefixlen = (unsigned char)prefix;
	request.message.ifa_flags     = IFA_F_NODAD;
	request.message.ifa_scope     = RT_SCOPE_UNIVERSE;
	request.message.ifa_index     = index;
	attribute = (struct rtattr *)((char *)&request + NLMSG_ALIGN(request.header.nlmsg_len));
	attribute->rta_type = IFA_LOCAL;
	attribute->rta_len  = RTA_LENGTH(sizeof parsed);
	memcpy(RTA_DATA(attribute), &parsed, sizeof parsed);
	request.header.nlmsg_len = NLMSG_ALIGN(request.header.nlmsg_len) + RTA_ALIGN(attribute->rta_len);
	attribute = (struct rtattr *)((char *)&request + request.header.nlmsg_len);
	attribute->rta_type = IFA_ADDRESS;
	attribute->rta_len  = RTA_LENGTH(sizeof parsed);
	memcpy(RTA_DATA(attribute), &parsed, sizeof parsed);
	request.header.nlmsg_len += RTA_ALIGN(attribute->rta_len);
	/* The flags again as IFA_FLAGS, which carries those past ifa_flags'
	 * eight bits and is what a newer kernel reads. */
	attribute = (struct rtattr *)((char *)&request + request.header.nlmsg_len);
	attribute->rta_type = IFA_FLAGS;
	attribute->rta_len  = RTA_LENGTH(sizeof flags);
	memcpy(RTA_DATA(attribute), &flags, sizeof flags);
	request.header.nlmsg_len += RTA_ALIGN(attribute->rta_len);

	if ((fd = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_ROUTE)) < 0)
		return -1;
	if (sendto(fd, &request, request.header.nlmsg_len, 0, (struct sockaddr *)&kernel,
	           sizeof kernel) != (ssize_t)request.header.nlmsg_len ||
	    (n = recv(fd, &reply, sizeof reply, 0)) < (ssize_t)NLMSG_LENGTH(sizeof reply.error)) {
		int saved = errno != 0 ? errno : EIO;

		close(fd);
		errno = saved;
		return -1;
	}
	close(fd);
	if (reply.header.nlmsg_type == NLMSG_ERROR && reply.error.error != 0) {
		errno = -reply.error.error;
		return -1;
	}
	return 0;
}
