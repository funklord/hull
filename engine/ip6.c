/*
 * ip6.c -- an IPv6 address given to an interface, and a default route
 * through a gateway on it (ip6.h).
 */

#define _GNU_SOURCE

#include "ip6.h"

#include <errno.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <linux/if_addr.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <net/if.h>
#include <sys/socket.h>

/* `request`, `length` bytes, put to the kernel and its acknowledgement
 * read: 0, or -1 with errno its refusal. */
static int ask(const void *request, uint32_t length)
{
	struct {
		struct nlmsghdr  header;
		struct nlmsgerr  error;
	} reply;
	struct sockaddr_nl kernel = { .nl_family = AF_NETLINK };
	ssize_t            n;
	int                fd;

	if ((fd = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_ROUTE)) < 0)
		return -1;
	if (sendto(fd, request, length, 0, (struct sockaddr *)&kernel, sizeof kernel) !=
	        (ssize_t)length ||
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

int hull_ip6_add(const char *name, const char *address, unsigned prefix)
{
	struct {
		struct nlmsghdr  header;
		struct ifaddrmsg message;
		char             attributes[64];
	} request;
	struct in6_addr    parsed;
	struct rtattr     *attribute;
	unsigned           index = if_nametoindex(name);
	uint32_t           flags = IFA_F_NODAD;

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
	return ask(&request, request.header.nlmsg_len);
}

int hull_ip6_route(const char *name, const char *address, unsigned prefix)
{
	struct {
		struct nlmsghdr header;
		struct rtmsg    message;
		char            attributes[64];
	} request;
	struct in6_addr parsed;
	struct rtattr  *attribute;
	uint32_t        index = if_nametoindex(name);

	if (index == 0u)
		return -1;
	if (inet_pton(AF_INET6, address, &parsed) != 1 || prefix == 0u || prefix > 127u) {
		errno = EINVAL;
		return -1;
	}
	/* The network's ::1: its host bits cleared, then the last set. */
	for (unsigned bit = prefix; bit < 128u; bit++)
		parsed.s6_addr[bit / 8u] &= (uint8_t)~(0x80u >> (bit % 8u));
	parsed.s6_addr[15] |= 1u;
	memset(&request, 0, sizeof request);
	request.header.nlmsg_len   = NLMSG_LENGTH(sizeof request.message);
	request.header.nlmsg_type  = RTM_NEWROUTE;
	request.header.nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK | NLM_F_CREATE | NLM_F_EXCL;
	request.header.nlmsg_seq   = 1u;
	request.message.rtm_family   = AF_INET6;
	request.message.rtm_table    = RT_TABLE_MAIN;
	request.message.rtm_protocol = RTPROT_BOOT;
	request.message.rtm_scope    = RT_SCOPE_UNIVERSE;
	request.message.rtm_type     = RTN_UNICAST;
	attribute = (struct rtattr *)((char *)&request + NLMSG_ALIGN(request.header.nlmsg_len));
	attribute->rta_type = RTA_GATEWAY;
	attribute->rta_len  = RTA_LENGTH(sizeof parsed);
	memcpy(RTA_DATA(attribute), &parsed, sizeof parsed);
	request.header.nlmsg_len = NLMSG_ALIGN(request.header.nlmsg_len) + RTA_ALIGN(attribute->rta_len);
	attribute = (struct rtattr *)((char *)&request + request.header.nlmsg_len);
	attribute->rta_type = RTA_OIF;
	attribute->rta_len  = RTA_LENGTH(sizeof index);
	memcpy(RTA_DATA(attribute), &index, sizeof index);
	request.header.nlmsg_len += RTA_ALIGN(attribute->rta_len);
	return ask(&request, request.header.nlmsg_len);
}
