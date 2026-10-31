/*
 * probe.c -- the one program in the reference engine's test rootfs.
 *
 * Linked static, since the rootfs holds nothing else. Its arguments are
 * steps, taken in order, each printing what the machine looks like from
 * inside:
 *
 *   say TEXT    TEXT on stdout        err TEXT    TEXT on stderr
 *   bare TEXT   TEXT on stdout, with no newline after it
 *   ticks N     tick=1 to tick=N, a tenth of a second apart, from a child
 *   env NAME    NAME's value          cwd         the working directory
 *   host        the hostname          uid         the uid
 *   euid        the effective uid, which a setuid file's owner gives
 *   write PATH  whether PATH can be created
 *   remove PATH whether PATH can be removed
 *   mkdir PATH  whether PATH can be made, as a directory
 *   rmdir PATH  whether the directory PATH can be removed
 *   list PATH   the names in the directory PATH, sorted
 *   owner PATH  PATH's owner, group and mode, not following a link
 *   kind PATH   PATH's kind, and a character device's major:minor
 *   read PATH   PATH's first line
 *   load PATH   whether the module PATH, under /lib/modules/RELEASE, loads
 * Run as PID 1 with no arguments it is an init that reaps for ever; run as
 * `poweroff` it powers the machine off. Steps:
 *
 *   target PATH where the symlink PATH points
 *   fsflags PATH  the nosuid, nodev and noexec of PATH's mount, and its size in MiB
 *   chown PATH ID  whether PATH can be given to ID, as its owner and group
 *   nest        whether a user and network namespace can be made inside,
 *               as `unshare -rn` makes one, in a child that then exits
 *   link IF     whether the interface IF can be brought up
 *   shout IF T  T in a broadcast frame on IF, of EtherType 0x88b5, the
 *               IEEE's for local experiments, once a second for 20 s
 *               or until a frame of that type is heard back
 *   hear IF     the text of the first such frame IF is given in 20 s,
 *               and its source; then one back, so a shout ends
 *   addr IF     IF's IPv4 address and prefix, and whether it is up
 *   hosts N     N's address in /etc/hosts, read as nss_files reads it:
 *               the probe is static, so it has no resolver of glibc's
 *   answer P    the first UDP datagram to port P in 60 s, and where from;
 *               the machine's hostname sent back to it
 *   ask N P     N looked up as hosts does, a datagram to its port P each
 *               200 ms until one comes back, or 60 s pass, as a sibling vm
 *               may still be booting; and the reply
 *   dns N       N's A record from resolv.conf's first nameserver, asked
 *               as a stub resolver asks; or the rcode
 *   ptr A       the name a PTR lookup of address A gives there
 *   stdin       the first line of its stdin
 *   count       all of its stdin: how many bytes, and their FNV-1a hash
 *   spew N      N bytes on stdout, from a xorshift that does not repeat
 *               in any window a ring or a buffer would have
 *   lines N T   N lines on stdout, `T NNNNNN`, each written by itself, so
 *               each crosses to the engine as a frame of its own
 *   line F T    whether file F holds a line that is exactly T: yes or no
 *   sleep N     sleep N seconds
 *   nnp         whether no_new_privs is set
 *   session     whether it leads a session of its own
 *   pid         its pid
 *   procs       how many processes /proc lists, or -1 without one
 *   orphan      leave a grandchild to the machine's init, which ends a
 *               second later, and wait two for the init to reap it
 *   exit N      exit with N           die         end by SIGTERM
 *   wait        wait for a signal, for 20 seconds at most, so that a
 *               test whose stop fails leaves no probe behind
 *   trap        catch SIGHUP, SIGINT or SIGTERM, for 20 seconds at most,
 *               and say which; the probe goes on
 *   tty         whether stdin is a terminal, its size, and whether it is
 *               this process's controlling terminal
 *   winch       wait for the terminal to be resized, as trap waits, and
 *               say its size
 *   serve P N   listen on port N, P tcp or udp, for 20 seconds at most;
 *               greet the one peer over tcp and say the line it sends,
 *               or echo the one datagram over udp and say it
 *   echo N      echo each datagram on udp port N to its sender, until
 *               one says `end`, for 20 seconds at most, naming each
 *               sender's port as it first comes, and say how many came
 *   dial A N    connect to address A, port N, over tcp within 5 seconds,
 *               and say, after A:N, the first line it sends or why it
 *               could not
 *   pour N B    listen on tcp port N, for 20 seconds at most, and send
 *               the one peer B bytes of spew's, then close; a peer that
 *               goes is said, SIGPIPE being ignored from then on
 *   drink N     listen on tcp port N, for 20 seconds at most, count what
 *               the one peer sends until it shuts its writing, as count
 *               does, and send it the count
 *   resolve A N ask the DNS server at address A for NAME's IPv4 address,
 *               within 5 seconds, and say its answer's rcode and count
 *   arch        the machine the kernel reports to it, as uname -m does
 *   ping A      an ICMP echo request to address A, IPv4 or IPv6, from a
 *               raw socket, up to three a second apart; ok and the data
 *               echoed whole, or none
 *   ping6 N     ping N's IPv6 address in /etc/hosts
 *   dns6 N      N's AAAA record, asked as dns asks for its A
 *   dnstcp6 N   the same over TCP
 *   hosts6 N    N's IPv6 address in /etc/hosts
 *   addr6 IF    IF's IPv6 address of global scope and its prefix
 *   slaac IF    the address SLAAC makes on IF from an advertisement, its
 *               interface identifier IF's MAC's EUI-64, waited for up to
 *               15 seconds; or none
 *   spin MS     use MS milliseconds of CPU time, by its own clock
 *   reap MS     spin MS in a child, and reap it
 *   hold N      take N MiB and touch every page, held until it exits
 *   fill PATH N write N MiB to PATH, and sync it
 */

#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netpacket/packet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/prctl.h>
#include <sys/reboot.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/statvfs.h>
#include <sys/syscall.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t caught;

static void catch(int signal)
{
	caught = signal;
}

/* Wait for one of `signals`, for 20 seconds at most; the one caught, or 0. */
static int await(const int *signals, unsigned n)
{
	struct sigaction action;

	memset(&action, 0, sizeof action);
	action.sa_handler = catch;
	caught = 0;
	for (unsigned i = 0u; i < n; i++)
		sigaction(signals[i], &action, NULL);
	printf("ready\n");
	alarm(20);
	while (caught == 0)
		pause();
	alarm(0);
	return caught;
}

static void print_size(void)
{
	struct winsize size;

	if (ioctl(0, TIOCGWINSZ, &size) == 0)
		printf("size=%ux%u\n", (unsigned)size.ws_col, (unsigned)size.ws_row);
	else
		printf("size=%s\n", strerror(errno));
}

/* `left` bytes from a xorshift that does not repeat in any window a ring
 * or a buffer would have, to `fd`; 0 if it would not take them. */
static int spew_to(int fd, unsigned long long left)
{
	unsigned      x = 1u;
	unsigned char buffer[4096];

	while (left > 0u) {
		size_t n = left < sizeof buffer ? (size_t)left : sizeof buffer;

		for (size_t j = 0u; j < n; j++) {
			x ^= x << 13;
			x ^= x >> 17;
			x ^= x << 5;
			buffer[j] = (unsigned char)x;
		}
		for (size_t done = 0u; done < n;) {
			ssize_t w = write(fd, buffer + done, n - done);

			if (w < 0 && errno == EINTR)
				continue;
			if (w < 0)
				return 0;
			done += (size_t)w;
		}
		left -= n;
	}
	return 1;
}

/* All of `fd` until its end, as "bytes=N fnv=H", into `out`. */
static void count_from(int fd, char *out, size_t cap)
{
	unsigned long long total = 0u;
	unsigned           hash  = 2166136261u;
	char               text[4096];
	ssize_t            n;

	while ((n = read(fd, text, sizeof text)) > 0 || (n < 0 && errno == EINTR))
		for (ssize_t j = 0; j < n; j++, total++)
			hash = (hash ^ (unsigned char)text[j]) * 16777619u;
	snprintf(out, cap, "bytes=%llu fnv=%08x", total, hash);
}

/* The one peer of tcp port `port`, within 20 seconds, or -1, said why. */
static int accept_one(const char *port)
{
	int                one = 1;
	int                fd  = socket(AF_INET, SOCK_STREAM, 0);
	int                peer;
	struct sockaddr_in at;
	struct pollfd      p;

	memset(&at, 0, sizeof at);
	at.sin_family = AF_INET;
	at.sin_port   = htons((unsigned short)atoi(port));
	setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
	if (fd < 0 || bind(fd, (struct sockaddr *)&at, sizeof at) != 0 || listen(fd, 1) != 0) {
		printf("serve=%s\n", strerror(errno));
		return -1;
	}
	printf("serving\n");
	p.fd     = fd;
	p.events = POLLIN;
	peer = poll(&p, 1u, 20000) == 1 ? accept(fd, NULL, NULL) : -1;
	if (peer < 0)
		printf("serve=nobody came\n");
	close(fd);
	return peer;
}

/* A line from `fd` within `seconds`, without its newline, into `out`. */
static void read_line(int fd, char *out, size_t cap, int seconds)
{
	size_t        n = 0u;
	struct pollfd p = { fd, POLLIN, 0 };

	while (n + 1u < cap && poll(&p, 1u, seconds * 1000) == 1) {
		char c;

		if (read(fd, &c, 1u) != 1 || c == '\n')
			break;
		out[n++] = c;
	}
	out[n] = '\0';
}

static void echo(const char *port)
{
	static char        datagram[65536];
	struct sockaddr_in senders[16];
	unsigned           count = 0u;
	int                fd = socket(AF_INET, SOCK_DGRAM, 0);
	struct sockaddr_in at;
	time_t             end = time(NULL) + 20;

	memset(&at, 0, sizeof at);
	at.sin_family = AF_INET;
	at.sin_port   = htons((unsigned short)atoi(port));
	if (fd < 0 || bind(fd, (struct sockaddr *)&at, sizeof at) != 0) {
		printf("echo=%s\n", strerror(errno));
		return;
	}
	printf("echoing\n");
	fflush(stdout);
	for (;;) {
		struct pollfd      p = { fd, POLLIN, 0 };
		struct sockaddr_in from;
		socklen_t          length = sizeof from;
		ssize_t            n;
		unsigned           i;

		if (time(NULL) >= end || poll(&p, 1u, 1000) < 0)
			break;
		if (p.revents == 0)
			continue;
		n = recvfrom(fd, datagram, sizeof datagram, 0, (struct sockaddr *)&from, &length);
		if (n < 0)
			continue;
		sendto(fd, datagram, (size_t)n, 0, (struct sockaddr *)&from, length);
		for (i = 0u; i < count; i++)
			if (senders[i].sin_port == from.sin_port &&
			    senders[i].sin_addr.s_addr == from.sin_addr.s_addr)
				break;
		if (i == count && count < 16u) {
			senders[count++] = from;
			printf("from %u\n", (unsigned)ntohs(from.sin_port));
		}
		if (n == 3 && memcmp(datagram, "end", 3u) == 0)
			break;
	}
	printf("echoed from %u\n", count);
	close(fd);
}

static void serve(const char *proto, const char *port)
{
	int                udp = strcmp(proto, "udp") == 0;
	int                one = 1;
	int                fd  = socket(AF_INET, udp ? SOCK_DGRAM : SOCK_STREAM, 0);
	struct sockaddr_in at;
	struct pollfd      p;
	char               text[256];

	memset(&at, 0, sizeof at);
	at.sin_family = AF_INET;
	at.sin_port   = htons((unsigned short)atoi(port));
	setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
	if (fd < 0 || bind(fd, (struct sockaddr *)&at, sizeof at) != 0 ||
	    (!udp && listen(fd, 1) != 0)) {
		printf("serve=%s\n", strerror(errno));
		return;
	}
	printf("serving\n");
	p.fd     = fd;
	p.events = POLLIN;
	if (poll(&p, 1u, 20000) != 1) {
		printf("serve=nobody came\n");
	} else if (udp) {
		struct sockaddr_in from;
		socklen_t          length = sizeof from;
		ssize_t            n = recvfrom(fd, text, sizeof text - 1u, 0,
		                                (struct sockaddr *)&from, &length);

		if (n < 0)
			n = 0;
		text[n] = '\0';
		sendto(fd, text, (size_t)n, 0, (struct sockaddr *)&from, length);
		printf("served=%s\n", text);
	} else {
		int peer = accept(fd, NULL, NULL);

		if (write(peer, "hello from the machine\n", 23u) != 23)
			printf("serve=%s\n", strerror(errno));
		read_line(peer, text, sizeof text, 5);
		printf("served=%s\n", text);
		close(peer);
	}
	close(fd);
}

static void dial(const char *address, const char *port)
{
	int                  six = strchr(address, ':') != NULL;   /* an IPv6 address */
	int                  fd = socket(six ? AF_INET6 : AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
	struct sockaddr_in6  to6;
	struct sockaddr_in   to;
	struct pollfd        p;
	int                  error = 0;
	socklen_t            length = sizeof error;
	char                 text[256];

	memset(&to, 0, sizeof to);
	to.sin_family = AF_INET;
	to.sin_port   = htons((unsigned short)atoi(port));
	inet_pton(AF_INET, address, &to.sin_addr);
	memset(&to6, 0, sizeof to6);
	to6.sin6_family = AF_INET6;
	to6.sin6_port   = to.sin_port;
	inet_pton(AF_INET6, address, &to6.sin6_addr);
	if ((six ? connect(fd, (struct sockaddr *)&to6, sizeof to6) :
	           connect(fd, (struct sockaddr *)&to, sizeof to)) != 0 && errno != EINPROGRESS) {
		printf("dial %s:%s=%s\n", address, port, strerror(errno));
		close(fd);
		return;
	}
	p.fd     = fd;
	p.events = POLLOUT;
	if (poll(&p, 1u, 5000) != 1) {
		printf("dial %s:%s=timed out\n", address, port);
	} else if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &length) != 0 || error != 0) {
		printf("dial %s:%s=%s\n", address, port, strerror(error));
	} else {
		read_line(fd, text, sizeof text, 5);
		printf("dial %s:%s=%s\n", address, port, text);
	}
	close(fd);
}

static void resolve(const char *server, const char *name)
{
	int                fd = socket(AF_INET, SOCK_DGRAM, 0);
	struct sockaddr_in to;
	struct pollfd      p;
	uint8_t            query[300] = { 0x68, 0x75, 0x01, 0x00, 0, 1, 0, 0, 0, 0, 0, 0 };
	uint8_t            answer[1500];
	size_t             n = 12u;
	ssize_t            got;

	/* The name as labels, then type A, class IN. */
	for (const char *label = name; *label != '\0' && n < sizeof query - 70u;) {
		size_t length = strcspn(label, ".");

		query[n++] = (uint8_t)length;
		memcpy(query + n, label, length);
		n += length;
		label += length;
		if (*label == '.')
			label++;
	}
	query[n++] = 0;
	query[n++] = 0;
	query[n++] = 1;
	query[n++] = 0;
	query[n++] = 1;
	memset(&to, 0, sizeof to);
	to.sin_family = AF_INET;
	to.sin_port   = htons(53);
	inet_pton(AF_INET, server, &to.sin_addr);
	if (sendto(fd, query, n, 0, (struct sockaddr *)&to, sizeof to) != (ssize_t)n) {
		printf("resolve=%s\n", strerror(errno));
		close(fd);
		return;
	}
	p.fd     = fd;
	p.events = POLLIN;
	if (poll(&p, 1u, 5000) != 1 || (got = recv(fd, answer, sizeof answer, 0)) < 12 ||
	    answer[0] != 0x68 || answer[1] != 0x75)
		printf("resolve=no answer\n");
	else
		printf("resolve=rcode %d answers %d\n", answer[3] & 0x0f, answer[6] << 8 | answer[7]);
	close(fd);
}

/* Ping `address` once a second, three times at most: ok where an echo
 * reply of ours comes back with the data sent. */
static void ping(const char *address)
{
	static const char  data[] = "hull ping";
	int                six = strchr(address, ':') != NULL;
	int                fd = six ? socket(AF_INET6, SOCK_RAW, IPPROTO_ICMPV6) :
	                            socket(AF_INET, SOCK_RAW, IPPROTO_ICMP);
	struct sockaddr_in to;
	struct sockaddr_in6 to6;
	uint8_t            request[8u + sizeof data - 1u];
	uint8_t            reply[1500];
	uint16_t           id = (uint16_t)getpid();

	if (fd < 0) {
		printf("ping=%s %s\n", address, strerror(errno));
		return;
	}
	memset(&to, 0, sizeof to);
	to.sin_family = AF_INET;
	memset(&to6, 0, sizeof to6);
	to6.sin6_family = AF_INET6;
	if (six ? inet_pton(AF_INET6, address, &to6.sin6_addr) != 1 :
	          inet_pton(AF_INET, address, &to.sin_addr) != 1) {
		printf("ping=%s not an address\n", address);
		close(fd);
		return;
	}
	for (unsigned try = 1u; try <= 3u; try++) {
		struct pollfd p = { fd, POLLIN, 0 };
		uint32_t      sum = 0u;

		memset(request, 0, sizeof request);
		request[0] = six ? 128u : 8u;   /* ICMPv6's echo request, whose sum the kernel takes */
		request[4] = (uint8_t)(id >> 8);
		request[5] = (uint8_t)id;
		request[7] = (uint8_t)try;
		memcpy(request + 8, data, sizeof data - 1u);
		for (size_t i = 0u; i + 1u < sizeof request; i += 2u)
			sum += (uint32_t)(request[i] << 8 | request[i + 1u]);
		if (sizeof request % 2u != 0u)
			sum += (uint32_t)request[sizeof request - 1u] << 8;
		while (sum >> 16)
			sum = (sum & 0xffffu) + (sum >> 16);
		if (!six) {
			request[2] = (uint8_t)(~sum >> 8);
			request[3] = (uint8_t)~sum;
		}
		if ((six ? sendto(fd, request, sizeof request, 0, (struct sockaddr *)&to6, sizeof to6) :
		           sendto(fd, request, sizeof request, 0, (struct sockaddr *)&to, sizeof to)) < 0) {
			printf("ping=%s %s\n", address, strerror(errno));
			close(fd);
			return;
		}
		/* A raw socket is given every ICMP message, IPv4's header first
		 * and ICMPv6's with none. */
		while (poll(&p, 1u, 1000) == 1) {
			ssize_t got = recv(fd, reply, sizeof reply, 0);
			size_t  at;

			if (got < (six ? 8 : 20))
				break;
			at = six ? 0u : (size_t)(reply[0] & 0x0fu) * 4u;
			if ((size_t)got == at + sizeof request && reply[at] == (six ? 129u : 0u) &&
			    reply[at + 4u] == request[4] && reply[at + 5u] == request[5] &&
			    memcmp(reply + at + 8u, data, sizeof data - 1u) == 0) {
				printf("ping=%s ok\n", address);
				close(fd);
				return;
			}
		}
	}
	printf("ping=%s none\n", address);
	close(fd);
}

/* The first nameserver /etc/resolv.conf names, into `out`: 1, or 0. */
static int nameserver(char out[64])
{
	FILE *conf = fopen("/etc/resolv.conf", "r");
	char  line[256];
	int   found = 0;

	while (conf != NULL && !found && fgets(line, sizeof line, conf) != NULL)
		found = sscanf(line, "nameserver %63s", out) == 1;
	if (conf != NULL)
		fclose(conf);
	return found;
}

/* Ask resolv.conf's nameserver for `name`'s A (`type` 1) or PTR (12),
 * three tries two seconds apart, as a stub resolver does; print the
 * answer's address or name, or its rcode. */
/* One answer to `query` from `to` over TCP, each after its length as 16
 * bits (RFC 7766): its length, or -1. */
static ssize_t ask_tcp(const struct sockaddr *to, socklen_t to_length, const uint8_t *query,
                       size_t n, uint8_t *answer, size_t cap)
{
	int           fd = socket(to->sa_family, SOCK_STREAM, 0);
	uint8_t       framed[2 + 300];
	uint8_t       length[2];
	size_t        want;
	size_t        got = 0u;
	struct pollfd p;

	framed[0] = (uint8_t)(n >> 8);
	framed[1] = (uint8_t)n;
	memcpy(framed + 2, query, n);
	p.fd     = fd;
	p.events = POLLIN;
	if (fd < 0 || connect(fd, to, to_length) != 0 ||
	    write(fd, framed, n + 2u) != (ssize_t)(n + 2u) || poll(&p, 1u, 15000) != 1 ||
	    read(fd, length, 2u) != 2) {
		if (fd >= 0)
			close(fd);
		return -1;
	}
	want = (size_t)(length[0] << 8 | length[1]);
	while (got < want && got < cap && poll(&p, 1u, 15000) == 1) {
		ssize_t r = read(fd, answer + got, (want < cap ? want : cap) - got);

		if (r <= 0)
			break;
		got += (size_t)r;
	}
	close(fd);
	return got == want ? (ssize_t)got : -1;
}

static void ask_dns(const char *step, const char *shown, const char *name, unsigned type)
{
	int                 tcp = strcmp(step, "dnstcp") == 0 || strcmp(step, "dnstcp6") == 0;
	int                 fd;
	struct sockaddr_in  to;
	struct sockaddr_in6 to6;
	struct sockaddr    *at;
	socklen_t           at_length;
	struct pollfd       p;
	char                server[64];
	uint8_t            query[300] = { 0x68, 0x76, 0x01, 0x00, 0, 1, 0, 0, 0, 0, 0, 0 };
	uint8_t            answer[1500];
	size_t             n = 12u;
	ssize_t            got = -1;

	if (!nameserver(server)) {
		printf("%s=%s no nameserver\n", step, shown);
		return;
	}
	/* Its family, the nameserver's: IPv6 where hull is at a segment's
	 * ::1, as on a segment of IPv6 alone. */
	memset(&to, 0, sizeof to);
	memset(&to6, 0, sizeof to6);
	to.sin_family    = AF_INET;
	to.sin_port      = htons(53);
	to6.sin6_family  = AF_INET6;
	to6.sin6_port    = htons(53);
	if (strchr(server, ':') != NULL) {
		inet_pton(AF_INET6, server, &to6.sin6_addr);
		at        = (struct sockaddr *)&to6;
		at_length = sizeof to6;
	} else {
		inet_pton(AF_INET, server, &to.sin_addr);
		at        = (struct sockaddr *)&to;
		at_length = sizeof to;
	}
	fd = socket(at->sa_family, SOCK_DGRAM, 0);
	for (const char *label = name; *label != '\0' && n < sizeof query - 70u;) {
		size_t length = strcspn(label, ".");

		query[n++] = (uint8_t)length;
		memcpy(query + n, label, length);
		n += length;
		label += length;
		if (*label == '.')
			label++;
	}
	query[n++] = 0;
	query[n++] = 0;
	query[n++] = (uint8_t)type;
	query[n++] = 0;
	query[n++] = 1;
	p.fd     = fd;
	p.events = POLLIN;
	/* Over TCP one connection, as a resolver keeps one: what it loses is
	 * the connection's to send again, not a fresh one's to hide. */
	for (int tries = 0; tries < (tcp ? 1 : 3) && got < 0; tries++) {
		if (tcp)
			got = ask_tcp(at, at_length, query, n, answer, sizeof answer);
		else if (sendto(fd, query, n, 0, at, at_length) == (ssize_t)n &&
		         poll(&p, 1u, 2000) == 1)
			got = recv(fd, answer, sizeof answer, 0);
	}
	close(fd);
	if (got < 12 || answer[0] != 0x68 || answer[1] != 0x76) {
		printf("%s=%s no answer from %s\n", step, shown, server);
		return;
	}
	/* One answer after the question echoed: a pointer, type, class, TTL,
	 * then its data. */
	if ((answer[6] << 8 | answer[7]) == 0 || (size_t)got < n + 12u) {
		printf("%s=%s rcode %d\n", step, shown, answer[3] & 0x0f);
		return;
	}
	{
		const uint8_t *data = answer + n + 12u;
		size_t         length = (size_t)(answer[n + 10u] << 8 | answer[n + 11u]);

		if ((size_t)got < n + 12u + length) {
			printf("%s=%s short answer\n", step, shown);
		} else if (type == 1u && length == 4u) {
			printf("%s=%s %u.%u.%u.%u\n", step, shown, data[0], data[1], data[2], data[3]);
		} else if (type == 28u && length == 16u) {
			char text[INET6_ADDRSTRLEN];

			inet_ntop(AF_INET6, data, text, sizeof text);
			printf("%s=%s %s\n", step, shown, text);
		} else {
			char   text[256];
			size_t at = 0u;

			for (size_t i = 0u; i < length && data[i] != 0u && at + 64u < sizeof text;) {
				size_t label = data[i];

				if (at != 0u)
					text[at++] = '.';
				memcpy(text + at, data + i + 1u, label);
				at += label;
				i += 1u + label;
			}
			text[at] = '\0';
			printf("%s=%s %s\n", step, shown, text);
		}
	}
}

/* Until this process has used `ms` of CPU time. */
static void spin(long ms)
{
	struct timespec now;
	long long       until;
	volatile unsigned long turns = 0u;

	clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &now);
	until = (long long)now.tv_sec * 1000 + now.tv_nsec / 1000000 + ms;
	do {
		for (unsigned i = 0u; i < 100000u; i++)
			turns++;
		clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &now);
	} while ((long long)now.tv_sec * 1000 + now.tv_nsec / 1000000 < until);
}

/* The raw frames link, shout and hear speak, of the IEEE's EtherType for
 * local experiments. */
#define EXPERIMENT 0x88b5

/* A packet socket bound to `name`, for that type, or -1: datagrams, so
 * the kernel writes the header from the address given and takes it off
 * what is read, the source left in the address. */
static int raw_socket(const char *name, struct sockaddr_ll *at)
{
	int fd = socket(AF_PACKET, SOCK_DGRAM | SOCK_CLOEXEC, htons(EXPERIMENT));

	memset(at, 0, sizeof *at);
	at->sll_family   = AF_PACKET;
	at->sll_protocol = htons(EXPERIMENT);
	at->sll_ifindex  = (int)if_nametoindex(name);
	if (fd >= 0 && (at->sll_ifindex == 0 || bind(fd, (struct sockaddr *)at, sizeof *at) != 0)) {
		close(fd);
		fd = -1;
	}
	return fd;
}

static void link_up(const char *name)
{
	struct ifreq request;
	int          fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);

	memset(&request, 0, sizeof request);
	snprintf(request.ifr_name, sizeof request.ifr_name, "%s", name);
	if (fd < 0 || ioctl(fd, SIOCGIFFLAGS, &request) != 0) {
		printf("link=%s\n", strerror(errno));
	} else {
		request.ifr_flags |= IFF_UP;
		printf("link=%s\n", ioctl(fd, SIOCSIFFLAGS, &request) == 0 ? "up" : strerror(errno));
	}
	if (fd >= 0)
		close(fd);
}

/* Broadcast `text` on `name`; with `answer`, once, else each second until
 * a frame comes back or 20 s pass. */
static void shout(const char *name, const char *text, int answer)
{
	struct sockaddr_ll at;
	uint8_t            frame[256];
	size_t             length = strlen(text);
	int                fd = raw_socket(name, &at);
	int                heard = 0;

	if (fd < 0 || length > 200u) {
		printf("shout=%s\n", fd < 0 ? strerror(errno) : "too long");
		if (fd >= 0)
			close(fd);
		return;
	}
	at.sll_halen = 6;
	memset(at.sll_addr, 0xff, 6u);
	frame[0] = (uint8_t)length;
	memcpy(frame + 1, text, length);
	for (int tries = 0; tries < (answer ? 1 : 20) && !heard; tries++) {
		struct pollfd p = { fd, POLLIN, 0 };

		if (sendto(fd, frame, 1u + length, 0, (struct sockaddr *)&at, sizeof at) < 0) {
			printf("shout=%s\n", strerror(errno));
			break;
		}
		/* A packet socket is given its own frames too, as outgoing. */
		while (!answer && !heard && poll(&p, 1u, 1000) > 0) {
			struct sockaddr_ll from;
			socklen_t          from_length = sizeof from;
			uint8_t            back[256];

			if (recvfrom(fd, back, sizeof back, 0, (struct sockaddr *)&from, &from_length) < 0)
				break;
			heard = from.sll_pkttype != PACKET_OUTGOING;
		}
	}
	if (!answer)
		printf("shout=%s\n", heard ? "answered" : "unanswered");
	close(fd);
}

static void hear(const char *name)
{
	struct sockaddr_ll at;
	int                fd = raw_socket(name, &at);
	struct pollfd      p = { fd, POLLIN, 0 };
	uint8_t            frame[256];
	socklen_t          from_length = sizeof at;
	ssize_t            n;

	if (fd < 0) {
		printf("hear=%s\n", strerror(errno));
		return;
	}
	if (poll(&p, 1u, 20000) <= 0 ||
	    (n = recvfrom(fd, frame, sizeof frame, 0, (struct sockaddr *)&at, &from_length)) < 1 ||
	    frame[0] + 1 > n) {
		printf("hear=nothing\n");
		close(fd);
		return;
	}
	printf("hear=%.*s from %02x:%02x:%02x:%02x:%02x:%02x\n", (int)frame[0], (const char *)frame + 1,
	       at.sll_addr[0], at.sll_addr[1], at.sll_addr[2], at.sll_addr[3], at.sll_addr[4],
	       at.sll_addr[5]);
	close(fd);
	shout(name, "heard", 1);
}

static void show_address(const char *name)
{
	struct ifreq request;
	char         address[INET_ADDRSTRLEN] = "none";
	unsigned     prefix = 0u;
	int          fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
	int          up = 0;

	memset(&request, 0, sizeof request);
	snprintf(request.ifr_name, sizeof request.ifr_name, "%s", name);
	if (fd >= 0 && ioctl(fd, SIOCGIFADDR, &request) == 0) {
		inet_ntop(AF_INET, &((struct sockaddr_in *)&request.ifr_addr)->sin_addr, address,
		          sizeof address);
		if (ioctl(fd, SIOCGIFNETMASK, &request) == 0)
			prefix = (unsigned)__builtin_popcount(
			        ((struct sockaddr_in *)&request.ifr_addr)->sin_addr.s_addr);
	}
	if (fd >= 0 && ioctl(fd, SIOCGIFFLAGS, &request) == 0)
		up = (request.ifr_flags & IFF_UP) != 0;
	printf("addr=%s %s/%u %s\n", name, address, prefix, up ? "up" : "down");
	if (fd >= 0)
		close(fd);
}

/* `name`'s address in /etc/hosts into `out`: the first line naming it
 * among its names, as nss_files answers. 0 where none does. */
static int lookup(const char *name, char *out, size_t cap)
{
	FILE *hosts = fopen("/etc/hosts", "r");
	char  line[512];
	int   found = 0;

	while (hosts != NULL && !found && fgets(line, sizeof line, hosts) != NULL) {
		char *save = NULL;
		char *address;
		char *alias;

		*strchrnul(line, '#') = '\0';
		if ((address = strtok_r(line, " \t\n", &save)) == NULL)
			continue;
		while (!found && (alias = strtok_r(NULL, " \t\n", &save)) != NULL)
			if (strcmp(alias, name) == 0) {
				snprintf(out, cap, "%s", address);
				found = 1;
			}
	}
	if (hosts != NULL)
		fclose(hosts);
	return found;
}

/* `name`'s IPv6 address in /etc/hosts, a line whose address has a colon,
 * into `out`: 1, or 0. */
static int lookup6(const char *name, char *out, size_t cap)
{
	FILE *hosts = fopen("/etc/hosts", "r");
	char  line[512];
	int   found = 0;

	while (hosts != NULL && !found && fgets(line, sizeof line, hosts) != NULL) {
		char *save = NULL;
		char *address;
		char *alias;

		*strchrnul(line, '#') = '\0';
		if ((address = strtok_r(line, " \t\n", &save)) == NULL || strchr(address, ':') == NULL)
			continue;
		while (!found && (alias = strtok_r(NULL, " \t\n", &save)) != NULL)
			if (strcmp(alias, name) == 0) {
				snprintf(out, cap, "%s", address);
				found = 1;
			}
	}
	if (hosts != NULL)
		fclose(hosts);
	return found;
}

/* IF's IPv6 address of global scope and its prefix, or none. */
static void addr6(const char *name)
{
	struct ifaddrs *all;
	int             said = 0;

	if (getifaddrs(&all) != 0) {
		printf("addr6=%s %s\n", name, strerror(errno));
		return;
	}
	for (struct ifaddrs *a = all; a != NULL && !said; a = a->ifa_next) {
		const struct sockaddr_in6 *at = (const struct sockaddr_in6 *)a->ifa_addr;
		const struct sockaddr_in6 *mask = (const struct sockaddr_in6 *)a->ifa_netmask;
		char                       text[INET6_ADDRSTRLEN];
		unsigned                   bits = 0u;

		if (at == NULL || at->sin6_family != AF_INET6 || strcmp(a->ifa_name, name) != 0 ||
		    IN6_IS_ADDR_LINKLOCAL(&at->sin6_addr))
			continue;
		for (unsigned i = 0u; mask != NULL && i < 16u; i++)
			bits += (unsigned)__builtin_popcount(mask->sin6_addr.s6_addr[i]);
		inet_ntop(AF_INET6, &at->sin6_addr, text, sizeof text);
		printf("addr6=%s %s/%u\n", name, text, bits);
		said = 1;
	}
	if (!said)
		printf("addr6=%s none\n", name);
	freeifaddrs(all);
}

/* The address SLAAC makes on IF from an advertised prefix, its interface
 * identifier the EUI-64 of IF's MAC, once it is there: up to 15 seconds,
 * the kernel soliciting, being advertised to, and checking its address
 * for duplicates before it is one. */
static void slaac(const char *name)
{
	struct ifreq request;
	uint8_t      eui[8];
	int          fd = socket(AF_INET6, SOCK_DGRAM | SOCK_CLOEXEC, 0);

	memset(&request, 0, sizeof request);
	snprintf(request.ifr_name, sizeof request.ifr_name, "%s", name);
	if (fd < 0 || ioctl(fd, SIOCGIFHWADDR, &request) != 0) {
		printf("slaac=%s %s\n", name, strerror(errno));
		if (fd >= 0)
			close(fd);
		return;
	}
	close(fd);
	memcpy(eui, request.ifr_hwaddr.sa_data, 3u);
	eui[0] ^= 0x02u;
	eui[3] = 0xffu;
	eui[4] = 0xfeu;
	memcpy(eui + 5, request.ifr_hwaddr.sa_data + 3, 3u);
	for (int tries = 0; tries < 30; tries++) {
		struct ifaddrs *all;

		if (getifaddrs(&all) == 0) {
			for (struct ifaddrs *a = all; a != NULL; a = a->ifa_next) {
				const struct sockaddr_in6 *at = (const struct sockaddr_in6 *)a->ifa_addr;
				char                       text[INET6_ADDRSTRLEN];

				if (at == NULL || at->sin6_family != AF_INET6 || strcmp(a->ifa_name, name) != 0 ||
				    IN6_IS_ADDR_LINKLOCAL(&at->sin6_addr) ||
				    memcmp(at->sin6_addr.s6_addr + 8, eui, 8u) != 0)
					continue;
				inet_ntop(AF_INET6, &at->sin6_addr, text, sizeof text);
				printf("slaac=%s %s\n", name, text);
				freeifaddrs(all);
				return;
			}
			freeifaddrs(all);
		}
		usleep(500000);
	}
	printf("slaac=%s none\n", name);
}

static void answer(const char *port)
{
	struct sockaddr_in at = { .sin_family = AF_INET, .sin_port = htons((uint16_t)atoi(port)) };
	struct sockaddr_in from;
	socklen_t          from_length = sizeof from;
	struct pollfd      p;
	char               text[256];
	char               host[64] = "";
	int                fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);

	p.fd = fd;
	p.events = POLLIN;
	if (fd < 0 || bind(fd, (struct sockaddr *)&at, sizeof at) != 0) {
		printf("answer=%s\n", strerror(errno));
	} else if (poll(&p, 1u, 60000) <= 0 ||
	           recvfrom(fd, text, sizeof text, 0, (struct sockaddr *)&from, &from_length) < 0) {
		printf("answer=nothing\n");
	} else {
		gethostname(host, sizeof host - 1u);
		inet_ntop(AF_INET, &from.sin_addr, text, sizeof text);
		printf("answer=from %s\n", text);
		sendto(fd, host, strlen(host), 0, (struct sockaddr *)&from, from_length);
		/* And again to an asker that asks again, until a second passes
		 * with none: an answer still in the tap when the machine's init
		 * exits goes with its namespace, and the asker, retrying, then
		 * found nobody, 2 runs in 28 of three chroot machines. */
		from_length = sizeof from;
		while (poll(&p, 1u, 1000) > 0 &&
		       recvfrom(fd, text, sizeof text, 0, (struct sockaddr *)&from, &from_length) >= 0) {
			sendto(fd, host, strlen(host), 0, (struct sockaddr *)&from, from_length);
			from_length = sizeof from;
		}
	}
	if (fd >= 0)
		close(fd);
}

static void ask(const char *name, const char *port)
{
	struct sockaddr_in to = { .sin_family = AF_INET, .sin_port = htons((uint16_t)atoi(port)) };
	char               address[64];
	char               reply[256];
	int                fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);

	if (!lookup(name, address, sizeof address) || inet_pton(AF_INET, address, &to.sin_addr) != 1) {
		printf("ask=%s unresolved\n", name);
	} else if (fd < 0) {
		printf("ask=%s\n", strerror(errno));
	} else {
		ssize_t n = -1;

		for (int tries = 0; tries < 300 && n < 0; tries++) {
			struct pollfd p = { fd, POLLIN, 0 };

			sendto(fd, "ask", 3u, 0, (struct sockaddr *)&to, sizeof to);
			if (poll(&p, 1u, 200) > 0)
				n = recv(fd, reply, sizeof reply - 1u, 0);
		}
		if (n < 0)
			printf("ask=%s at %s: no reply\n", name, address);
		else
			printf("ask=%s at %s: %.*s\n", name, address, (int)n, reply);
	}
	if (fd >= 0)
		close(fd);
}

int main(int argc, char **argv)
{
	char text[256];
	/* An image's init and its poweroff, for (init PATH)'s test. */
	{
		const char *base = strrchr(argv[0], '/') != NULL ? strrchr(argv[0], '/') + 1 : argv[0];

		if (strcmp(base, "poweroff") == 0) {
			sync();
			reboot(RB_POWER_OFF);
			return 1;
		}
		/* A stand-in for the image's own sshd, for (ssh true)'s test: it
		 * records how hull invoked it and the files it was pointed at into
		 * /sshd.stub, then listens on :22 and answers with an SSH banner,
		 * so the test checks hull's whole job -- find the sshd, place the
		 * host key, authorized_keys and config, start it on 22 -- without a
		 * real OpenSSH in the guest. */
		if (strcmp(base, "sshd") == 0) {
			const char *config = "";
			const char *hostkey = "";
			int         one = 1;
			int         port = 22;
			char        listen_addr[64] = "";
			int         fd;
			FILE       *rec;
			FILE       *cf;
			struct sockaddr_in at;

			for (int i = 1; i < argc; i++) {
				if (strcmp(argv[i], "-f") == 0 && i + 1 < argc)
					config = argv[++i];
				else if (strcmp(argv[i], "-h") == 0 && i + 1 < argc)
					hostkey = argv[++i];
			}
			/* Bind what the config says, as a real sshd would: the chroot
			 * engine points it at the machine's own address on a high port,
			 * the unprivileged user being unable to bind 22 on the host. */
			if (config[0] != '\0' && (cf = fopen(config, "r")) != NULL) {
				char line[256];

				while (fgets(line, sizeof line, cf) != NULL) {
					if (sscanf(line, " Port %d", &port) == 1)
						continue;
					sscanf(line, " ListenAddress %63s", listen_addr);
				}
				fclose(cf);
			}
			fd = socket(AF_INET, SOCK_STREAM, 0);
			memset(&at, 0, sizeof at);
			at.sin_family = AF_INET;
			at.sin_port   = htons((unsigned short)port);
			if (listen_addr[0] == '\0' || inet_pton(AF_INET, listen_addr, &at.sin_addr) != 1)
				at.sin_addr.s_addr = htonl(INADDR_ANY);
			setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
			if (fd < 0 || bind(fd, (struct sockaddr *)&at, sizeof at) != 0 || listen(fd, 1) != 0)
				return 1;
			if ((rec = fopen("/sshd.stub", "w")) != NULL) {
				/* A summary first, since a reader takes one line: the
				 * config and host key hull pointed sshd at. */
				fprintf(rec, "started config=%s hostkey=%s\n", config, hostkey);
				for (int i = 0; i < argc; i++)
					fprintf(rec, "argv%d=%s\n", i, argv[i]);
				fclose(rec);
			}
			for (;;) {
				int peer = accept(fd, NULL, NULL);

				if (peer < 0)
					continue;
				(void)!write(peer, "SSH-2.0-hull-stub\r\n", 19u);
				close(peer);
			}
		}
		if (getpid() == 1 && argc == 1) {
			for (;;)
				if (wait(NULL) < 0)
					pause();
		}
	}

	setvbuf(stdout, NULL, _IONBF, 0);
	/* Run as a machine's /bin/sh, as `hull ssh` runs one: what it was
	 * given, and whether that is on a terminal. */
	/* And as a build step's shell, `/bin/sh -c "probe STEP ..."`: the
	 * line's words after `probe` are its steps, so that a build runs with
	 * no shell in the tree. */
	if (strcmp(argv[0], "/bin/sh") == 0 && argc == 3 && strcmp(argv[1], "-c") == 0 &&
	    strncmp(argv[2], "probe ", 6u) == 0) {
		static char *words[64];
		int          n = 1;

		words[0] = argv[0];
		for (char *word = strtok(argv[2] + 6, " "); word != NULL && n < 63;
		     word = strtok(NULL, " "))
			words[n++] = word;
		words[n] = NULL;
		argv = words;
		argc = n;
	} else if (strcmp(argv[0], "/bin/sh") == 0) {
		for (int i = 1; i < argc; i++)
			printf("sh %d=%s\n", i, argv[i]);
		printf("tty=%s\n", isatty(0) ? "yes" : "no");
		return 0;
	}
	for (int i = 1; i < argc; i++) {
		const char *step = argv[i];
		const char *arg  = i + 1 < argc ? argv[i + 1] : "";

		if (strcmp(step, "say") == 0) {
			printf("%s\n", arg);
			i++;
		} else if (strcmp(step, "bare") == 0) {
			printf("%s", arg);
			fflush(stdout);
			i++;
		} else if (strcmp(step, "err") == 0) {
			fprintf(stderr, "%s\n", arg);
			i++;
		} else if (strcmp(step, "env") == 0) {
			printf("%s=%s\n", arg, getenv(arg) != NULL ? getenv(arg) : "(unset)");
			i++;
		} else if (strcmp(step, "cwd") == 0) {
			printf("cwd=%s\n", getcwd(text, sizeof text) != NULL ? text : "(unknown)");
		} else if (strcmp(step, "host") == 0) {
			printf("host=%s\n", gethostname(text, sizeof text) == 0 ? text : "(unknown)");
		} else if (strcmp(step, "uid") == 0) {
			printf("uid=%u\n", (unsigned)getuid());
		} else if (strcmp(step, "euid") == 0) {
			printf("euid=%u\n", (unsigned)geteuid());
		} else if (strcmp(step, "write") == 0) {
			int fd = open(arg, O_WRONLY | O_CREAT | O_TRUNC, 0644);

			printf("write=%s\n", fd >= 0 ? "ok" : strerror(errno));
			if (fd >= 0)
				close(fd);
			i++;
		} else if (strcmp(step, "tell") == 0 && i + 2 < argc) {
			/* A line to a device, a serial port's say: no create, no
			 * controlling terminal taken. */
			int     fd = open(arg, O_WRONLY | O_NOCTTY | O_CLOEXEC);
			ssize_t put = -1;

			if (fd >= 0) {
				char line[256];
				int  length = snprintf(line, sizeof line, "%s\n", argv[i + 2]);

				put = write(fd, line, (size_t)length);
				close(fd);
			}
			printf("tell=%s\n", put > 0 ? "ok" : strerror(errno));
			i += 2;
		} else if (strcmp(step, "remove") == 0) {
			printf("remove=%s\n", unlink(arg) == 0 ? "ok" : strerror(errno));
			i++;
		} else if (strcmp(step, "mkdir") == 0) {
			printf("mkdir=%s\n", mkdir(arg, 0755) == 0 ? "ok" : strerror(errno));
			i++;
		} else if (strcmp(step, "rmdir") == 0) {
			printf("rmdir=%s\n", rmdir(arg) == 0 ? "ok" : strerror(errno));
			i++;
		} else if (strcmp(step, "list") == 0) {
			struct dirent **names;
			int             n = scandir(arg, &names, NULL, alphasort);

			if (n < 0) {
				printf("list=%s\n", strerror(errno));
			} else {
				printf("list=");
				for (int k = 0; k < n; k++) {
					if (strcmp(names[k]->d_name, ".") != 0 && strcmp(names[k]->d_name, "..") != 0)
						printf(" %s", names[k]->d_name);
					free(names[k]);
				}
				free(names);
				printf("\n");
			}
			i++;
		} else if (strcmp(step, "present") == 0) {
			/* A ready command's: on while the file is there, else 1. */
			if (access(arg, F_OK) != 0) {
				printf("absent=%s\n", arg);
				return 1;
			}
			i++;
		} else if (strcmp(step, "kind") == 0) {
			struct stat file;

			if (lstat(arg, &file) != 0)
				printf("kind=%s\n", strerror(errno));
			else if (S_ISCHR(file.st_mode))
				printf("kind=chr %u:%u\n", major(file.st_rdev), minor(file.st_rdev));
			else
				printf("kind=%s\n", S_ISREG(file.st_mode) ? "reg" : S_ISDIR(file.st_mode) ? "dir" :
				       S_ISLNK(file.st_mode) ? "lnk" : "other");
			i++;
		} else if (strcmp(step, "owner") == 0) {
			struct stat file;

			if (lstat(arg, &file) != 0)
				printf("owner=%s\n", strerror(errno));
			else
				printf("owner=%u:%u %04o\n", (unsigned)file.st_uid, (unsigned)file.st_gid,
				       (unsigned)(file.st_mode & 07777));
			i++;
		} else if (strcmp(step, "session") == 0) {
			printf("leader=%d\n", getsid(0) == getpid());
		} else if (strcmp(step, "read") == 0) {
			FILE *file = fopen(arg, "r");

			if (file == NULL || fgets(text, sizeof text, file) == NULL)
				printf("read=%s\n", strerror(errno));
			else
				printf("read=%s", text);
			if (file != NULL)
				fclose(file);
			i++;
		} else if (strcmp(step, "load") == 0) {
			/* As modprobe would, a compressed module the kernel
			 * unpacks: 4 is MODULE_INIT_COMPRESSED_FILE. */
			struct utsname kernel;
			char           path[4400];
			int            fd = -1;

			if (uname(&kernel) == 0) {
				snprintf(path, sizeof path, "/lib/modules/%s/%s", kernel.release, arg);
				fd = open(path, O_RDONLY | O_CLOEXEC);
			}
			if (fd < 0)
				printf("load=%s\n", strerror(errno));
			else if (syscall(SYS_finit_module, fd, "", 4) != 0 && errno != EEXIST)
				printf("load=%s\n", strerror(errno));
			else
				printf("load=ok\n");
			if (fd >= 0)
				close(fd);
			i++;
		} else if (strcmp(step, "stdin") == 0) {
			ssize_t n = read(0, text, sizeof text - 1u);

			text[n > 0 ? n : 0] = '\0';
			printf("stdin=%s", n > 0 ? text : "(none)\n");
		} else if (strcmp(step, "count") == 0) {
			count_from(0, text, sizeof text);
			printf("%s\n", text);
		} else if (strcmp(step, "spew") == 0) {
			if (!spew_to(1, strtoull(arg, NULL, 10)))
				return 1;
			i++;
		} else if (strcmp(step, "line") == 0 && i + 2 < argc) {
			FILE *file = fopen(arg, "r");
			int   found = 0;

			while (file != NULL && !found && fgets(text, sizeof text, file) != NULL) {
				text[strcspn(text, "\n")] = '\0';
				found = strcmp(text, argv[i + 2]) == 0;
			}
			if (file != NULL)
				fclose(file);
			printf("line=%s\n", found ? "yes" : "no");
			i += 2;
		} else if (strcmp(step, "lines") == 0 && i + 2 < argc) {
			unsigned long count = strtoul(arg, NULL, 10);

			for (unsigned long k = 1u; k <= count; k++)
				printf("%s %06lu\n", argv[i + 2], k);
			i += 2;
		} else if (strcmp(step, "pour") == 0 && i + 2 < argc) {
			int peer = accept_one(arg);

			/* A peer that goes is an error to report, as a server
			 * would, rather than the end of the probe. */
			signal(SIGPIPE, SIG_IGN);

			if (peer >= 0) {
				printf("poured=%s\n", spew_to(peer, strtoull(argv[i + 2], NULL, 10)) ?
				                      argv[i + 2] : strerror(errno));
				close(peer);
			}
			i += 2;
		} else if (strcmp(step, "drink") == 0) {
			int peer = accept_one(arg);

			if (peer >= 0) {
				count_from(peer, text, sizeof text);
				dprintf(peer, "%s\n", text);
				printf("drank %s\n", text);
				close(peer);
			}
			i++;
		} else if (strcmp(step, "ticks") == 0) {
			/* In a child, so that a pause is seen to stop more than
			 * the workload's own process. */
			pid_t child = fork();

			if (child == 0) {
				for (long t = 1; t <= atol(arg); t++) {
					struct timespec tenth = { 0, 100000000L };

					printf("tick=%ld\n", t);
					nanosleep(&tenth, NULL);
				}
				_exit(0);
			}
			if (child > 0)
				waitpid(child, NULL, 0);
			i++;
		} else if (strcmp(step, "spin") == 0) {
			spin(atol(arg));
			printf("spun=%s\n", arg);
			i++;
		} else if (strcmp(step, "reap") == 0) {
			pid_t child = fork();

			if (child == 0) {
				spin(atol(arg));
				_exit(0);
			}
			waitpid(child, NULL, 0);
			printf("reaped=%s\n", arg);
			i++;
		} else if (strcmp(step, "hold") == 0) {
			size_t         n = (size_t)atol(arg) << 20;
			volatile char *held = malloc(n);

			for (size_t at = 0u; held != NULL && at < n; at += 4096u)
				held[at] = 1;
			printf("held=%s\n", held != NULL ? arg : "no");
			i++;
		} else if (strcmp(step, "stamp") == 0) {
			/* The time, to the nanosecond: what no two builds write the
			 * same, for `hull build --check` to find. */
			struct timespec now;
			FILE           *f = fopen(arg, "w");

			clock_gettime(CLOCK_REALTIME, &now);
			if (f != NULL) {
				fprintf(f, "%lld.%09ld\n", (long long)now.tv_sec, now.tv_nsec);
				fclose(f);
			}
			printf("stamp=%s\n", f != NULL ? "ok" : strerror(errno));
			i++;
		} else if (strcmp(step, "fill") == 0 && i + 2 < argc) {
			static char block[1u << 20];
			int         fd = open(arg, O_WRONLY | O_CREAT | O_TRUNC, 0644);
			long        n = atol(argv[i + 2]);
			long        done = 0;

			memset(block, 'f', sizeof block);
			while (fd >= 0 && done < n && write(fd, block, sizeof block) == (ssize_t)sizeof block)
				done++;
			/* Synced, so that it has reached storage, which a cgroup
			 * counts only once it has. */
			if (fd >= 0 && fsync(fd) != 0)
				done = -1;
			printf("filled=%ld\n", done);
			if (fd >= 0)
				close(fd);
			i += 2;
		} else if (strcmp(step, "sleep") == 0) {
			sleep((unsigned)atoi(arg));
			i++;
		} else if (strcmp(step, "pid") == 0) {
			printf("pid=%d\n", (int)getpid());
		} else if (strcmp(step, "procs") == 0) {
			DIR           *proc  = opendir("/proc");
			struct dirent *entry;
			int            count = 0;

			while (proc != NULL && (entry = readdir(proc)) != NULL)
				count += entry->d_name[0] >= '1' && entry->d_name[0] <= '9';
			printf("procs=%d\n", proc != NULL ? count : -1);
			if (proc != NULL)
				closedir(proc);
		} else if (strcmp(step, "orphan") == 0) {
			pid_t child = fork();

			if (child == 0) {
				if (fork() == 0) {
					sleep(1);
					_exit(0);
				}
				_exit(0);
			}
			waitpid(child, NULL, 0);
			sleep(2);
		} else if (strcmp(step, "nnp") == 0) {
			printf("nnp=%d\n", prctl(PR_GET_NO_NEW_PRIVS, 0, 0, 0, 0));
		} else if (strcmp(step, "exit") == 0) {
			return atoi(arg);
		} else if (strcmp(step, "die") == 0) {
			raise(SIGTERM);
		} else if (strcmp(step, "trap") == 0) {
			static const int signals[] = { SIGHUP, SIGINT, SIGTERM };

			printf("signal=%d\n", await(signals, 3u));
		} else if (strcmp(step, "tty") == 0) {
			printf("tty=%s\n", isatty(0) ? "yes" : "no");
			print_size();
			printf("controlling=%s\n", tcgetpgrp(0) == getpgrp() ? "yes" : "no");
		} else if (strcmp(step, "winch") == 0) {
			static const int signals[] = { SIGWINCH };

			await(signals, 1u);
			print_size();
		} else if (strcmp(step, "serve") == 0 && i + 2 < argc) {
			serve(arg, argv[i + 2]);
			i += 2;
		} else if (strcmp(step, "echo") == 0) {
			echo(arg);
			i++;
		} else if (strcmp(step, "dial") == 0 && i + 2 < argc) {
			dial(arg, argv[i + 2]);
			i += 2;
		} else if (strcmp(step, "dns") == 0 || strcmp(step, "dnstcp") == 0) {
			ask_dns(step, arg, arg, 1u);
			i++;
		} else if (strcmp(step, "arch") == 0) {
			struct utsname self;

			printf("arch=%s\n", uname(&self) == 0 ? self.machine : strerror(errno));
		} else if (strcmp(step, "ping") == 0) {
			ping(arg);
			i++;
		} else if (strcmp(step, "ptr") == 0) {
			unsigned a, b, c, d;
			char     reverse[64];

			if (sscanf(arg, "%u.%u.%u.%u", &a, &b, &c, &d) == 4) {
				snprintf(reverse, sizeof reverse, "%u.%u.%u.%u.in-addr.arpa", d, c, b, a);
				ask_dns("ptr", arg, reverse, 12u);
			} else {
				printf("ptr=%s not an address\n", arg);
			}
			i++;
		} else if (strcmp(step, "resolve") == 0 && i + 2 < argc) {
			resolve(arg, argv[i + 2]);
			i += 2;
		} else if (strcmp(step, "chown") == 0 && i + 2 < argc) {
			uid_t id = (uid_t)strtoul(argv[i + 2], NULL, 10);

			printf("chown=%s\n", chown(arg, id, (gid_t)id) == 0 ? "ok" : strerror(errno));
			i += 2;
		} else if (strcmp(step, "fsflags") == 0) {
			struct statvfs fs;

			if (statvfs(arg, &fs) != 0)
				printf("fsflags=%s\n", strerror(errno));
			else
				printf("fsflags=%s%s%s size=%llu\n", (fs.f_flag & ST_NOSUID) ? "nosuid," : "",
				       (fs.f_flag & ST_NODEV) ? "nodev," : "",
				       (fs.f_flag & ST_NOEXEC) ? "noexec" : "exec",
				       (unsigned long long)fs.f_blocks * fs.f_frsize / (1024u * 1024u));
			i++;
		} else if (strcmp(step, "target") == 0) {
			char    to[4096];
			ssize_t n = readlink(arg, to, sizeof to - 1u);

			if (n < 0)
				printf("target=%s\n", strerror(errno));
			else
				printf("target=%.*s\n", (int)n, to);
			i++;
		} else if (strcmp(step, "nest") == 0) {
			int   status = 0;
			pid_t child = fork();

			if (child == 0)
				_exit(unshare(CLONE_NEWUSER | CLONE_NEWNET) == 0 ? 0 : errno);
			if (child < 0 || waitpid(child, &status, 0) != child || !WIFEXITED(status))
				printf("nest=%s\n", strerror(errno));
			else
				printf("nest=%s\n", WEXITSTATUS(status) == 0 ? "ok" :
				       strerror(WEXITSTATUS(status)));
		} else if (strcmp(step, "link") == 0) {
			link_up(arg);
			i++;
		} else if (strcmp(step, "shout") == 0 && i + 2 < argc) {
			shout(arg, argv[i + 2], 0);
			i += 2;
		} else if (strcmp(step, "hear") == 0) {
			hear(arg);
			i++;
		} else if (strcmp(step, "addr") == 0) {
			show_address(arg);
			i++;
		} else if (strcmp(step, "dnstcp6") == 0) {
			ask_dns(step, arg, arg, 28u);
			i++;
		} else if (strcmp(step, "dns6") == 0) {
			ask_dns(step, arg, arg, 28u);
			i++;
		} else if (strcmp(step, "ping6") == 0) {
			char address[64];

			if (lookup6(arg, address, sizeof address))
				ping(address);
			else
				printf("ping=%s none in /etc/hosts\n", arg);
			i++;
		} else if (strcmp(step, "hosts6") == 0) {
			char address[64];

			printf("hosts6=%s %s\n", arg, lookup6(arg, address, sizeof address) ? address : "none");
			i++;
		} else if (strcmp(step, "slaac") == 0) {
			slaac(arg);
			i++;
		} else if (strcmp(step, "addr6") == 0) {
			addr6(arg);
			i++;
		} else if (strcmp(step, "hosts") == 0) {
			char address[64];

			printf("hosts=%s %s\n", arg, lookup(arg, address, sizeof address) ? address : "none");
			i++;
		} else if (strcmp(step, "answer") == 0) {
			answer(arg);
			i++;
		} else if (strcmp(step, "ask") == 0 && i + 2 < argc) {
			ask(arg, argv[i + 2]);
			i += 2;
		} else if (strcmp(step, "wait") == 0) {
			printf("waiting\n");
			alarm(20);
			pause();
		}
	}
	return 0;
}
