/*
 * ip6.h -- an IPv6 address given to an interface, as hull gives a machine
 * its address on a segment with an IPv6 network (Appendix D): chroot's
 * init on its tap, the agent on a vm's NIC; and its route out through
 * the segment's ::1.
 */

#ifndef HULL_IP6_H
#define HULL_IP6_H

#ifdef __cplusplus
extern "C" {
#endif

/*
 * `address`, IPv6 text, with `prefix` bits, on the interface `name`, in
 * the calling process's network namespace, by netlink: marked to skip
 * duplicate address detection, since hull drew it apart from every other
 * on the segment and a tentative address cannot be bound for a second or
 * so, which a daemon starting at once meets. 0, or -1 with errno.
 */
int hull_ip6_add(const char *name, const char *address, unsigned prefix);

/* A default IPv6 route on interface `name` through its network's ::1,
 * hull's, where the segment is a gateway (Appendix D): the network that
 * of `address`, IPv6 text, and `prefix` bits. By netlink: 0, or -1 with
 * errno set. */
int hull_ip6_route(const char *name, const char *address, unsigned prefix);

#ifdef __cplusplus
}
#endif

#endif /* HULL_IP6_H */
