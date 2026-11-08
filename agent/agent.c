/*
 * agent.c -- the guest agent (project.md Appendix B), PID 1 in a vm or
 * emu machine.
 *
 * Booted as the initramfs's /init. It mounts the kernel's filesystems,
 * loads the modules the engine put beside it, in the order /hull/modules
 * names them, brings up lo and eth0 at the address the engine's passt
 * expects, and listens on vsock port 1 for the one host connection
 * (B.1, B.2). What the host sends is held to agent/protocol.situ's
 * host_message before anything reads it, as everything the core reads is.
 *
 * `spec` carries the resolved machine. The agent mounts the machine's
 * directory, shared by the host over 9p with the tag `root`, and each
 * volume, tagged v0, v1, ... in the order the spec lists them; then it
 * moves the directory onto / and chroots into it, as switch_root does.
 * Under (mutable overlay) the share is read-only beneath an overlayfs
 * whose upper layer is a tmpfs, so what the machine writes is held in its
 * memory and goes with it.
 * `start` forks the workload, a session of its own, with the spec's
 * environment, workdir and no_new_privs. Its stdout and stderr come back
 * as `data` on channel 0, which is the workload's, as it is for `signal`.
 *
 * The agent is the machine's init: it reaps whatever is orphaned to it,
 * and once the workload has ended and its output has been read it says
 * `exited`, exactly as the workload ended, ends what is left, says `bye`
 * and powers the machine off once the host has closed, which it does on
 * hearing bye. A `stop` is SIGTERM, then SIGKILL at its
 * timeout. A host that goes away is (on_disconnect stop): the same, with
 * nobody to tell.
 *
 * `exec` runs a command in the machine on a channel the host allocated:
 * with the machine's environment, workdir and no_new_privs, on pipes or on
 * a terminal of the size the request gives. Its output flows on B.2's
 * credit: the host starts with 64 KiB of it and gives it back as it takes
 * the output, and a channel out of credit is not read, so its command
 * blocks writing, as on a pipe. Its stdin flows the other way: the agent
 * gives the host 64 KiB and gives credit back as bytes reach the command,
 * and says `eof` once the command's input has closed. The workload's
 * output, on channel 0, flows on the same credit since 2026-10-29, so the
 * run that started the machine holds it back as a pipe would.
 *
 * `connect` forwards one of the machine's ports: a TCP connection to the
 * machine's own address, on a channel the host allocated, flowing on the
 * same credit as an exec. The host's `eof` shuts its write side, so a
 * client may half-close; the connection's end, read from the machine, is
 * `exited` with 0, and the host's `signal` on it ends it at once. With
 * `(proto udp)` it is one client's datagrams, on a udp socket connected
 * there: each `data` is one datagram, sent whole or dropped, as udp is,
 * and neither way waits for credit. Only the host's `signal` ends it.
 *
 * One thread, one poll(): the connection, every channel's descriptors,
 * and a signalfd for SIGCHLD.
 */

#define _GNU_SOURCE

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/un.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/prctl.h>
#include <sys/reboot.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include <arpa/inet.h>
#include <net/if.h>
#include <net/route.h>
#include <netinet/in.h>

#include <linux/fsverity.h>
#include <linux/vm_sockets.h>

#include "ip6.h"
#include "sshd.h"
#include "access.h"
#include "user.h"
#include "tree.h"
#include "wire.h"

/* The commit, where the build that made this one wrote it, as hull's. */
#if defined(__has_include)
#if __has_include("commit.h")
#include "commit.h"
#endif
#endif
#ifndef HULL_COMMIT
#define HULL_COMMIT "unknown"
#endif

#define AGENT_VERSION "0.1"
#define AGENT_PORT    1u
#define MAX_ARGS      256u
#define MAX_ENV       256u
#define MAX_VOLUMES   64u
#define MAX_CHANNELS  65u      /* the workload's, and 64 execs */
#define WINDOW        65536u   /* each exec's credit, both ways */

#ifndef MODULE_INIT_COMPRESSED_FILE
#define MODULE_INIT_COMPRESSED_FILE 4
#endif

/* The workload, once the spec has said what it is. */
static char  *argv_[MAX_ARGS + 1u];
static char  *envp_[MAX_ENV + 1u];
static char  *workdir;
static char  *user_;        /* (user ...), or NULL for the machine's root */
static int    stop_signal_ = SIGTERM;   /* what a stop sends first */
#define MAX_LIMITS 32u
static char   shm_size[32];             /* (shm_size ...), tmpfs's words, or "" */
static char  *limits_[MAX_LIMITS];      /* (ulimits ...) */
static unsigned limit_count;
static char  *sysctls_[MAX_LIMITS];     /* (sysctls ...) */
static unsigned sysctl_count;
static char  *init_;       /* (init PATH): the image's init, PID 1 once handed to */
static int    handed;      /* PID 1 is the image's init, and this is the agent beside it */
static int    run_made;    /* /run is a tmpfs made here, as an initramfs makes it */
static int    no_new_privs;
static int    applied;     /* the spec is applied: the root is the machine's */
static int    ssh_enabled; /* (ssh true): supervise the image's own sshd */
static char   ssh_bin[256]; /* the image's sshd the agent found, absolute */
static int    ssh_fetched; /* none in the image: `hull sshd fetch`'s dropbear */
static char  *images[MAX_VOLUMES]; /* each (block ...) volume's guest path, to unmount */
static unsigned image_count;

/* A process the host follows on a channel: the workload on 0, each exec
 * on its own. */
typedef struct channel {
	int      used;
	uint32_t ch;
	pid_t    pid;
	int      out;          /* -1 once closed; on a terminal, its master */
	int      err;          /* -1 once closed, and always on a terminal */
	int      in;           /* -1 once closed; on a terminal, the master */
	int      tty;          /* the master, or -1 */
	int      credited;     /* its output flows on the host's credit */
	size_t   credit;       /* output the host will take */
	uint8_t *input;        /* stdin the host has sent and the command not read */
	size_t   head;
	size_t   queued;
	int      input_ended;  /* the host's eof */
	int      reaped;
	int      status;
	int      socket;       /* a connection: `out` and `in` are one socket */
	int      datagram;     /* a udp client's: a datagram a message, no credit */
} channel;

static channel channels[MAX_CHANNELS];
static int     stopping;
static struct timespec kill_at;

static hull_wire host;
static int       connected;

/*
 * (on_disconnect keep): a host that goes leaves the workload running, and
 * the agent listens for the next, which a core that comes after the one
 * that went makes when it takes the machine back. Any local user can
 * reach a guest's vsock port, so a host after the first is believed only
 * once it says `resume` with the token the first gave with `start`, and
 * until then it is answered only `hello`. Meanwhile the workload's output
 * is held, its last HELD_LIMIT bytes, for the host that resumes; execs
 * and forwarded connections, whose frontends went with that host, end.
 * A workload that ends meanwhile is reported to that host, and the machine
 * powers off once it has been.
 */
#define HELD_LIMIT (256u * 1024u)
#define TOKEN_MAX  64u

static int      keep_;
static uint8_t  token_[TOKEN_MAX];
static size_t   token_length;
static int      believed = 1;    /* the host may do more than hello and resume */
static int      awaiting;        /* the workload ended with no host to tell */
static uint8_t *held;            /* records: fd, a u32 length, the bytes */
static size_t   held_length;

/* Diagnostics go to the console, which the engine keeps in a file. */
static void say(const char *what, const char *detail)
{
	fprintf(stderr, "hull agent: %s%s%s\n", what, detail != NULL ? ": " : "",
	        detail != NULL ? detail : "");
}

/* Seconds the image's own shutdown is given before the machine is
 * powered off regardless. */
#define SHUTDOWN_GRACE 20

static void power_off(void)
{
	/* Handed to the image's init, its own shutdown, which every init of
	 * note answers as /sbin/poweroff, so that what it started ends as it
	 * would; the kernel's after the grace, if it has not ended this. */
	if (handed) {
		pid_t asked = fork();

		if (asked == 0) {
			char *const argv[] = { "poweroff", NULL };
			sigset_t    none;

			sigemptyset(&none);
			sigprocmask(SIG_SETMASK, &none, NULL);
			execv("/sbin/poweroff", argv);
			_exit(127);
		}
		for (int i = 0; i < SHUTDOWN_GRACE * 4; i++) {
			struct timespec quarter = { 0, 250000000L };

			nanosleep(&quarter, NULL);
		}
	}
	sync();
	/* An image volume is unmounted, the last first, so that its
	 * filesystem is left clean: ext4 marks its journal as needing
	 * recovery while mounted, and a later machine mounting it read-only,
	 * on a read-only disk, could not replay it. One still busy is made
	 * read-only, which leaves it as clean. An image's init unmounts its
	 * own as it shuts down. */
	while (image_count > 0u) {
		const char *at = images[--image_count];

		if (umount(at) != 0)
			mount(NULL, at, NULL, MS_REMOUNT | MS_RDONLY, NULL);
	}
	reboot(RB_POWER_OFF);
	for (;;)
		pause();
}

/* --- sending ------------------------------------------------------------- */

static void send_msg(hull_msg *m)
{
	if (connected && hull_wire_send(host.fd, m) != 0)
		connected = 0;
	hull_msg_free(m);
}

static void send_simple(const char *type)
{
	hull_msg m = { 0 };

	hull_msg_raw(&m, type);
	hull_msg_channel(&m, 0u);
	hull_msg_raw(&m, ")");
	send_msg(&m);
}

/* Say bye and power off once the host has heard it. A send is only
 * queued when it returns: powering off at once drops what the vsock
 * device has not yet carried, which on a loaded host was the exit and the
 * bye, and the host then reports a machine that died without its agent's
 * word. So the agent ends its side and waits, 3 seconds at most, for the
 * host's close, which it makes on reading bye. */
static void farewell(void)
{
	struct timespec end;

	send_simple("(3:bye");
	if (connected && shutdown(host.fd, SHUT_WR) == 0) {
		clock_gettime(CLOCK_MONOTONIC, &end);
		end.tv_sec += 3;
		for (;;) {
			struct timespec now;
			struct pollfd   fd = { host.fd, POLLIN, 0 };
			uint8_t         discard[512];
			long long       left;

			clock_gettime(CLOCK_MONOTONIC, &now);
			left = (long long)(end.tv_sec - now.tv_sec) * 1000 +
			       (end.tv_nsec - now.tv_nsec) / 1000000L;
			if (left <= 0 || poll(&fd, 1u, (int)left) <= 0 ||
			    read(host.fd, discard, sizeof discard) <= 0)
				break;
		}
	}
	power_off();
}

static void send_error(uint32_t ch, const char *reason)
{
	hull_msg m = { 0 };

	hull_msg_raw(&m, "(5:error");
	hull_msg_channel(&m, ch);
	hull_msg_raw(&m, "(6:reason");
	hull_msg_text(&m, reason);
	hull_msg_raw(&m, "))");
	send_msg(&m);
}

/* The workload's output while no host is there to read it: appended, and
 * the oldest records dropped while more than HELD_LIMIT is held. */
static void hold(int fd, const uint8_t *bytes, size_t n)
{
	size_t record = 5u + n;
	size_t at = 0u;

	if (record > HELD_LIMIT)
		return;
	if (held == NULL && (held = malloc(HELD_LIMIT)) == NULL)
		return;
	while (held_length - at + record > HELD_LIMIT) {
		uint32_t length;

		memcpy(&length, held + at + 1u, sizeof length);
		at += 5u + length;
	}
	memmove(held, held + at, held_length - at);
	held_length -= at;
	held[held_length] = (uint8_t)fd;
	memcpy(held + held_length + 1u, &(uint32_t){ (uint32_t)n }, 4u);
	memcpy(held + held_length + 5u, bytes, n);
	held_length += record;
}

static void send_data(uint32_t ch, int fd, const uint8_t *bytes, size_t n)
{
	hull_msg m = { 0 };

	if (!connected && keep_ && ch == 0u) {
		hold(fd, bytes, n);
		return;
	}

	hull_msg_raw(&m, "(4:data(3:buf");
	hull_msg_atom(&m, bytes, n);
	hull_msg_raw(&m, ")");
	hull_msg_channel(&m, ch);
	hull_msg_raw(&m, fd == 2 ? "(2:fd1:2))" : "(2:fd1:1))");
	send_msg(&m);
}

static void send_on(const char *type, uint32_t ch)
{
	hull_msg m = { 0 };

	hull_msg_raw(&m, type);
	hull_msg_channel(&m, ch);
	hull_msg_raw(&m, ")");
	send_msg(&m);
}

static void send_credit(uint32_t ch, size_t bytes)
{
	hull_msg m = { 0 };

	if (bytes == 0u)
		return;
	hull_msg_raw(&m, "(6:credit(5:bytes");
	hull_msg_number(&m, (long long)bytes);
	hull_msg_raw(&m, ")");
	hull_msg_channel(&m, ch);
	hull_msg_raw(&m, ")");
	send_msg(&m);
}

static void send_status(uint32_t ch, int wstatus)
{
	hull_msg m = { 0 };

	hull_msg_raw(&m, "(6:exited");
	hull_msg_channel(&m, ch);
	hull_msg_raw(&m, "(4:code");
	hull_msg_number(&m, WIFEXITED(wstatus) ? WEXITSTATUS(wstatus) : 0);
	hull_msg_raw(&m, ")(6:signal");
	hull_msg_number(&m, WIFSIGNALED(wstatus) ? WTERMSIG(wstatus) : 0);
	hull_msg_raw(&m, "))");
	send_msg(&m);
}

/* The guest's own stats, for `(sub (what stats))` (B.3): cpu is the guest
 * kernel's busy time from /proc/stat in microseconds, memory is used
 * (MemTotal - MemAvailable) from /proc/meminfo in bytes. io is 0: a hull vm
 * backs its root on 9p, not a block device, so the guest has no disk io to
 * count -- the host-side figure (section 4.10) is where a vm's io shows. */
static void send_guest_stats(uint32_t ch)
{
	hull_msg           m = { 0 };
	unsigned long long cpu = 0u;
	unsigned long long mem = 0u;
	FILE              *f;

	if ((f = fopen("/proc/stat", "r")) != NULL) {
		unsigned long long u = 0, n = 0, s = 0, idle = 0, iow = 0, irq = 0, soft = 0;

		if (fscanf(f, "cpu %llu %llu %llu %llu %llu %llu %llu", &u, &n, &s, &idle, &iow, &irq,
		           &soft) >= 4) {
			long hz = sysconf(_SC_CLK_TCK);

			cpu = (u + n + s + irq + soft) * 1000000ull / (unsigned long long)(hz > 0 ? hz : 100);
		}
		fclose(f);
	}
	if ((f = fopen("/proc/meminfo", "r")) != NULL) {
		char               line[256];
		unsigned long long total = 0u;
		unsigned long long avail = 0u;

		while (fgets(line, sizeof line, f) != NULL) {
			if (total == 0u)
				sscanf(line, "MemTotal: %llu kB", &total);
			if (avail == 0u)
				sscanf(line, "MemAvailable: %llu kB", &avail);
		}
		if (total >= avail)
			mem = (total - avail) * 1024ull;
		fclose(f);
	}
	hull_msg_raw(&m, "(5:stats");
	hull_msg_channel(&m, ch);
	hull_msg_raw(&m, "(3:cpu");
	hull_msg_number(&m, (long long)cpu);
	hull_msg_raw(&m, ")(2:io");
	hull_msg_number(&m, 0);
	hull_msg_raw(&m, ")(3:mem");
	hull_msg_number(&m, (long long)mem);
	hull_msg_raw(&m, "))");
	send_msg(&m);
}

/* --- the spec ------------------------------------------------------------ */

static void load_modules(void)
{
	FILE *list = fopen("/hull/modules", "r");
	char  name[128];
	char  path[200];

	while (list != NULL && fgets(name, sizeof name, list) != NULL) {
		int fd;

		name[strcspn(name, "\n")] = '\0';
		if (name[0] == '\0')
			continue;
		snprintf(path, sizeof path, "/hull/lib/%s", name);
		fd = open(path, O_RDONLY | O_CLOEXEC);
		if (fd < 0 || (syscall(SYS_finit_module, fd, "", MODULE_INIT_COMPRESSED_FILE) != 0 &&
		               errno != EEXIST))
			say(name, strerror(errno));
		if (fd >= 0)
			close(fd);
	}
	if (list != NULL)
		fclose(list);
}

/*
 * The links the engine lists in /hull/devices, `HOST GUEST` a line: each
 * serial or parallel port of (devices ...) at the path it has on the host,
 * linked to the guest's own, ttyS0 or parport0, so a program finds it
 * where it would there. devtmpfs is one for every mount of it, so the
 * machine's /dev has them too. A path that is the guest's own already, or
 * a host pty's, under the guest's own devpts, is left as it is.
 */
/* What the engine says to the workload, /hull/notices: said on its
 * stderr, channel 0's, before it runs, and not to an exec. Read at boot,
 * while /hull is there. */
static char notices[4096];

static void read_notices(void)
{
	FILE  *file = fopen("/hull/notices", "r");
	size_t got;

	if (file == NULL)
		return;
	got = fread(notices, 1u, sizeof notices - 1u, file);
	notices[got] = '\0';
	fclose(file);
}

static void link_devices(void)
{
	FILE *list = fopen("/hull/devices", "r");
	char  line[600];

	while (list != NULL && fgets(line, sizeof line, list) != NULL) {
		char  target[64];
		char *host = line;
		char *guest = strrchr(line, ' ');

		line[strcspn(line, "\n")] = '\0';
		if (guest == NULL || strncmp(host, "/dev/", 5u) != 0)
			continue;
		*guest++ = '\0';
		snprintf(target, sizeof target, "/dev/%s", guest);
		/* A host pty's path is in the guest's own devpts. */
		if (strcmp(host, target) == 0 || strncmp(host, "/dev/pts/", 9u) == 0)
			continue;
		for (char *slash = strchr(host + 5, '/'); slash != NULL; slash = strchr(slash + 1, '/')) {
			*slash = '\0';
			mkdir(host, 0755);
			*slash = '/';
		}
		if (symlink(target, host) != 0 && errno != EEXIST)
			say(host, strerror(errno));
	}
	if (list != NULL)
		fclose(list);
}

/* The network, as the engine's passt gives it (engine/qemu.c): lo, and
 * eth0 at GUEST_ADDRESS/24 routed through GATEWAY_ADDRESS. A machine's
 * resolv.conf is /hull/resolv.conf, naming passt's DNS. */
#define GUEST_ADDRESS   "10.0.2.15"
#define GATEWAY_ADDRESS "10.0.2.2"
#define DNS_ADDRESS     "10.0.2.3"

static void set_address(struct sockaddr *to, const char *address)
{
	struct sockaddr_in in;

	memset(&in, 0, sizeof in);
	in.sin_family = AF_INET;
	inet_pton(AF_INET, address, &in.sin_addr);
	memcpy(to, &in, sizeof in);
}

static int bring_up(int fd, const char *name)
{
	struct ifreq request;

	memset(&request, 0, sizeof request);
	snprintf(request.ifr_name, sizeof request.ifr_name, "%s", name);
	if (ioctl(fd, SIOCGIFFLAGS, &request) != 0)
		return -1;
	request.ifr_flags |= IFF_UP | IFF_RUNNING;
	return ioctl(fd, SIOCSIFFLAGS, &request);
}

/* The value of `key=` on the kernel's command line, into `out`: 1, or 0
 * where it is not there. */
static int cmdline_value(const char *key, char *out, size_t cap)
{
	char   line[1024];
	FILE  *cmdline = fopen("/proc/cmdline", "r");
	size_t length = strlen(key);
	int    given = 0;

	if (cmdline != NULL && fgets(line, sizeof line, cmdline) != NULL)
		for (char *word = strtok(line, " \n"); word != NULL && !given; word = strtok(NULL, " \n"))
			if (strncmp(word, key, length) == 0 && word[length] == '=') {
				snprintf(out, cap, "%s", word + length + 1u);
				given = 1;
			}
	if (cmdline != NULL)
		fclose(cmdline);
	return given;
}

/* Whether the kernel's command line has hull.passt=1. */
static int passt_given(void)
{
	char value[8];

	return cmdline_value("hull.passt", value, sizeof value) && strcmp(value, "1") == 0;
}

/* On a host bridge (engine/qemu.c): hull.bridge=ADDRESS/PREFIX,GATEWAY on
 * hull0, eth0 given them and a route out, the host's resolvers from
 * hull.dns=A,B in /hull/resolv.conf; or hull.bridge=self, eth0 up and its
 * addressing the workload's. 1 where it is on a bridge. */
static int bridged(int fd)
{
	char           value[96];
	char           dns[256];
	char           address[24];
	char           gateway[24];
	unsigned       prefix = 0u;
	struct ifreq   request;
	struct rtentry route;

	if (!cmdline_value("hull.bridge", value, sizeof value))
		return 0;
	memset(&request, 0, sizeof request);
	snprintf(request.ifr_name, sizeof request.ifr_name, "eth0");
	if (strcmp(value, "self") != 0) {
		if (sscanf(value, "%23[0-9.]/%u,%23[0-9.]", address, &prefix, gateway) != 3 ||
		    prefix < 1u || prefix > 30u) {
			say("hull.bridge", value);
			return 1;
		}
		set_address(&request.ifr_addr, address);
		if (ioctl(fd, SIOCSIFADDR, &request) != 0)
			say("eth0", strerror(errno));
		memset(&request.ifr_netmask, 0, sizeof request.ifr_netmask);
		((struct sockaddr_in *)&request.ifr_netmask)->sin_family = AF_INET;
		((struct sockaddr_in *)&request.ifr_netmask)->sin_addr.s_addr =
		    htonl(0xffffffffu << (32u - prefix));
		if (ioctl(fd, SIOCSIFNETMASK, &request) != 0)
			say("eth0", strerror(errno));
	}
	if (bring_up(fd, "eth0") != 0)
		say("eth0", strerror(errno));
	if (strcmp(value, "self") == 0)
		return 1;
	memset(&route, 0, sizeof route);
	set_address(&route.rt_dst, "0.0.0.0");
	set_address(&route.rt_genmask, "0.0.0.0");
	set_address(&route.rt_gateway, gateway);
	route.rt_flags = RTF_UP | RTF_GATEWAY;
	if (ioctl(fd, SIOCADDRT, &route) != 0)
		say("route", strerror(errno));
	if (cmdline_value("hull.dns", dns, sizeof dns)) {
		FILE *resolv = fopen("/hull/resolv.conf", "w");

		for (char *server = strtok(dns, ","); resolv != NULL && server != NULL;
		     server = strtok(NULL, ","))
			fprintf(resolv, "nameserver %s\n", server);
		if (resolv == NULL || fclose(resolv) != 0)
			say("resolv.conf", strerror(errno));
	}
	return 1;
}

static void network(void)
{
	int          fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
	struct ifreq request;
	struct rtentry route;
	FILE        *resolv;

	if (fd < 0) {
		say("network", strerror(errno));
		return;
	}
	if (bring_up(fd, "lo") != 0)
		say("lo", strerror(errno));
	/* A machine of (network none) has no passt NIC: lo alone, said
	 * nowhere, and eth0 is its first segment's, if it has one, which the
	 * engine says by leaving hull.passt off the command line; a bridge's
	 * eth0 is said by hull.bridge. */
	if (if_nametoindex("eth0") != 0 && bridged(fd)) {
		close(fd);
		return;
	}
	if (if_nametoindex("eth0") == 0 || !passt_given()) {
		close(fd);
		return;
	}
	memset(&request, 0, sizeof request);
	snprintf(request.ifr_name, sizeof request.ifr_name, "eth0");
	set_address(&request.ifr_addr, GUEST_ADDRESS);
	if (ioctl(fd, SIOCSIFADDR, &request) != 0)
		say("eth0", strerror(errno));
	set_address(&request.ifr_netmask, "255.255.255.0");
	if (ioctl(fd, SIOCSIFNETMASK, &request) != 0 || bring_up(fd, "eth0") != 0)
		say("eth0", strerror(errno));
	memset(&route, 0, sizeof route);
	set_address(&route.rt_dst, "0.0.0.0");
	set_address(&route.rt_genmask, "0.0.0.0");
	set_address(&route.rt_gateway, GATEWAY_ADDRESS);
	route.rt_flags = RTF_UP | RTF_GATEWAY;
	if (ioctl(fd, SIOCADDRT, &route) != 0)
		say("route", strerror(errno));
	close(fd);
	resolv = fopen("/hull/resolv.conf", "w");
	if (resolv == NULL || fprintf(resolv, "nameserver %s\n", DNS_ADDRESS) < 0 ||
	    fclose(resolv) != 0)
		say("resolv.conf", strerror(errno));
}

/* Mount the share `tag` at `at`: 9p, or under (share virtiofs) virtiofs for
 * the root and the volumes. The engine's own, its modules and the like, are
 * small and stay 9p. */
static int share(const char *tag, const char *at, int readonly)
{
	char how[16];

	if ((strcmp(tag, "root") == 0 || (tag[0] == 'v' && isdigit((unsigned char)tag[1]))) &&
	    cmdline_value("hull.share", how, sizeof how) && strcmp(how, "virtiofs") == 0)
		return mount(tag, at, "virtiofs", readonly ? MS_RDONLY : 0u, NULL);
	return mount(tag, at, "9p", readonly ? MS_RDONLY : 0u,
	             "trans=virtio,version=9p2000.L,msize=262144");
}

/* The partition whose GPT name is `name`, its node into `node`, a
 * bootable export's (section 3.13): looked for for five seconds, the disk
 * maybe still probing, as mount_block waits for one. Empty where none. */
static void partition_node(const char *name, char *node, size_t cap)
{
	struct timespec tenth = { 0, 100000000L };

	node[0] = '\0';
	for (int tries = 0; name[0] != '\0' && node[0] == '\0' && tries < 50; tries++) {
		DIR           *block = opendir("/sys/class/block");
		struct dirent *each;

		while (block != NULL && node[0] == '\0' && (each = readdir(block)) != NULL) {
			char  path[300];
			char  field[128];
			FILE *uevent;

			if (each->d_name[0] == '.')
				continue;
			snprintf(path, sizeof path, "/sys/class/block/%s/uevent", each->d_name);
			if ((uevent = fopen(path, "r")) == NULL)
				continue;
			while (fgets(field, sizeof field, uevent) != NULL) {
				field[strcspn(field, "\n")] = '\0';
				if (strncmp(field, "PARTNAME=", 9u) == 0 && strcmp(field + 9, name) == 0)
					snprintf(node, cap, "/dev/%s", each->d_name);
			}
			fclose(uevent);
		}
		if (block != NULL)
			closedir(block);
		if (node[0] == '\0')
			nanosleep(&tenth, NULL);
	}
}

/* The virtio disk whose serial is `tag`, an image volume's, mounted at
 * `at` as the filesystem its superblock says, which the engine read on
 * the host to put its module here. Its disk may still be probing: looked
 * for for two seconds. 0, or -1 with errno: ENODEV where there is no such
 * disk, EMEDIUMTYPE where it holds no filesystem hull knows. */
/* A hosted machine's fetched sshd, a whole disk of ext4 beside the one the
 * root is a partition of, there being no share for it (cli/vbox_host.c):
 * whichever disk holds a filesystem at its start. A SATA disk may still be
 * probing. 0, or -1. */
static int hosted_sshd(void)
{
	static const char *const nodes[] = { "/dev/sdb", "/dev/sda" };
	struct timespec          tenth = { 0, 100000000L };

	for (int tries = 0; tries < 50; tries++) {
		for (unsigned i = 0u; i < 2u; i++) {
			const char *type;

			if (hull_access_filesystem(nodes[i], &type) != NULL &&
			    mount(nodes[i], "/machine/run/hull/sshd", type, MS_RDONLY, NULL) == 0)
				return 0;
		}
		nanosleep(&tenth, NULL);
	}
	return -1;
}

static int mount_block(const char *tag, const char *at, int readonly)
{
	struct timespec tenth = { 0, 100000000L };
	char            node[300] = "";
	const char     *type;

	for (int tries = 0; node[0] == '\0' && tries < 20; tries++) {
		DIR           *block = opendir("/sys/block");
		struct dirent *disk;

		while (block != NULL && node[0] == '\0' && (disk = readdir(block)) != NULL) {
			char  path[300];
			char  serial[64];
			FILE *file;

			if (strncmp(disk->d_name, "vd", 2u) != 0)
				continue;
			snprintf(path, sizeof path, "/sys/block/%s/serial", disk->d_name);
			if ((file = fopen(path, "r")) == NULL)
				continue;
			if (fgets(serial, sizeof serial, file) != NULL) {
				serial[strcspn(serial, "\n")] = '\0';
				if (strcmp(serial, tag) == 0)
					snprintf(node, sizeof node, "/dev/%s", disk->d_name);
			}
			fclose(file);
		}
		if (block != NULL)
			closedir(block);
		if (node[0] == '\0')
			nanosleep(&tenth, NULL);
	}
	if (node[0] == '\0') {
		errno = ENODEV;
		return -1;
	}
	if (hull_access_filesystem(node, &type) == NULL) {
		errno = EMEDIUMTYPE;
		return -1;
	}
	/* Read-only, a journal an earlier machine left unreplayed is not
	 * replayed, which a read-only disk would refuse: the image is read
	 * as it was last made clean. */
	return mount(node, at, type, readonly ? MS_RDONLY : 0u,
	             readonly && (strcmp(type, "ext4") == 0 || strcmp(type, "xfs") == 0) ?
	             "norecovery" : NULL);
}

/* The directory as an overlay at /machine: its share read-only beneath,
 * and above it a tmpfs, whose root takes the directory's owner and mode
 * so that / is what it is in place. A reason it cannot be, or NULL. */
static const char *overlay(void)
{
	struct stat lower;

	mkdir("/hull/lower", 0755);
	mkdir("/hull/upper", 0755);
	if (share("root", "/hull/lower", 1) != 0)
		return "the machine's directory could not be mounted";
	if (mount("tmpfs", "/hull/upper", "tmpfs", 0u, "mode=0755") != 0 ||
	    mkdir("/hull/upper/data", 0755) != 0 || mkdir("/hull/upper/work", 0700) != 0 ||
	    stat("/hull/lower", &lower) != 0 ||
	    chown("/hull/upper/data", lower.st_uid, lower.st_gid) != 0 ||
	    chmod("/hull/upper/data", lower.st_mode & 07777) != 0) {
		say("overlay", strerror(errno));
		return "the overlay's upper layer could not be made";
	}
	if (mount("overlay", "/machine", "overlay", 0u,
	          "lowerdir=/hull/lower,upperdir=/hull/upper/data,workdir=/hull/upper/work") != 0) {
		say("overlay", strerror(errno));
		return "the machine's directory could not be mounted as an overlay";
	}
	return NULL;
}

/* A composefs root (section 3.2): the image, a file in the share tagged
 * cfs, mounted as erofs, and an overlay over it with the root share, the
 * store's objects, as its data-only lower layer, whose files the image's
 * redirect to; above them a tmpfs for the machine's writes, its root
 * taking the image root's owner and mode, unless the root is read-only.
 * A reason it cannot be, or NULL. */
/* fs-verity on every object of the disk at /hull/objects, `ab/abcdef...`,
 * as root may on its own filesystem: the kernel builds each Merkle tree
 * and from then on checks every read of the file against it. A file
 * already so is left. A reason it cannot be, or NULL. */
static const char *enable_verity(void)
{
	DIR           *top = opendir("/hull/objects");
	struct dirent *sub;

	if (top == NULL)
		return "the objects disk could not be read";
	while ((sub = readdir(top)) != NULL) {
		char           dir_path[300];
		DIR           *dir;
		struct dirent *object;

		/* `ab`, two hex digits: ".." is two characters too. */
		if (strlen(sub->d_name) != 2u || sub->d_name[0] == '.')
			continue;
		snprintf(dir_path, sizeof dir_path, "/hull/objects/%s", sub->d_name);
		if ((dir = opendir(dir_path)) == NULL)
			continue;
		while ((object = readdir(dir)) != NULL) {
			struct fsverity_enable_arg arg;
			int                        fd;

			if (object->d_name[0] == '.')
				continue;
			memset(&arg, 0, sizeof arg);
			arg.version = 1u;
			arg.hash_algorithm = FS_VERITY_HASH_ALG_SHA256;
			arg.block_size = 4096u;
			if ((fd = openat(dirfd(dir), object->d_name, O_RDONLY | O_CLOEXEC)) < 0 ||
			    (ioctl(fd, FS_IOC_ENABLE_VERITY, &arg) != 0 && errno != EEXIST)) {
				static char reason[160];

				snprintf(reason, sizeof reason,
				         "fs-verity could not be enabled on the tree's objects: %s",
				         strerror(errno));
				if (fd >= 0)
					close(fd);
				closedir(dir);
				closedir(top);
				return reason;
			}
			close(fd);
		}
		closedir(dir);
	}
	closedir(top);
	return NULL;
}

static const char *composefs_root(const char *image, int readonly, int verity)
{
	char        at[4300];
	char        options[256];
	const char *leaf = strrchr(image, '/');
	struct stat top;

	mkdir("/hull/cfs", 0755);
	mkdir("/hull/meta", 0755);
	mkdir("/hull/objects", 0755);
	if (leaf == NULL || share("cfs", "/hull/cfs", 1) != 0)
		return "the composefs image's directory could not be mounted";
	snprintf(at, sizeof at, "/hull/cfs%s", leaf);
	if (mount(at, "/hull/meta", "erofs", MS_RDONLY, NULL) != 0) {
		say("composefs", strerror(errno));
		return "the composefs image could not be mounted as erofs";
	}
	/* The objects: the store's share, or, with fs-verity, the disk of
	 * them the engine attached, verity enabled on each and every file's
	 * digest then required to be the one the image pins. */
	if (verity) {
		const char *reason;

		if (mount_block("objects", "/hull/objects", 0) != 0) {
			say("verity", strerror(errno));
			return "the tree's objects disk could not be mounted";
		}
		if ((reason = enable_verity()) != NULL)
			return reason;
	} else if (share("root", "/hull/objects", 1) != 0) {
		return "the store's objects could not be mounted";
	}
	if (readonly) {
		snprintf(options, sizeof options,
		         "lowerdir=/hull/meta::/hull/objects,metacopy=on,redirect_dir=on%s",
		         verity ? ",verity=require" : "");
	} else {
		mkdir("/hull/upper", 0755);
		if (mount("tmpfs", "/hull/upper", "tmpfs", 0u, "mode=0755") != 0 ||
		    mkdir("/hull/upper/data", 0755) != 0 || mkdir("/hull/upper/work", 0700) != 0 ||
		    stat("/hull/meta", &top) != 0 ||
		    chown("/hull/upper/data", top.st_uid, top.st_gid) != 0 ||
		    chmod("/hull/upper/data", top.st_mode & 07777) != 0) {
			say("composefs", strerror(errno));
			return "the composefs root's upper layer could not be made";
		}
		snprintf(options, sizeof options,
		         "lowerdir=/hull/meta::/hull/objects,upperdir=/hull/upper/data,"
		         "workdir=/hull/upper/work,metacopy=on,redirect_dir=on%s",
		         verity ? ",verity=require" : "");
	}
	if (mount("overlay", "/machine", "overlay", readonly ? MS_RDONLY : 0u, options) != 0) {
		say("composefs", strerror(errno));
		return "the composefs root's overlay could not be mounted";
	}
	return NULL;
}

/* Make `path`, and what leads to it, as directories. */
static void make_path(char *path)
{
	for (char *at = path + 1; *at != '\0'; at++) {
		if (*at == '/') {
			*at = '\0';
			mkdir(path, 0755);
			*at = '/';
		}
	}
	mkdir(path, 0755);
}

/* Apply `machine`: what the workload is, its root and its volumes. A
 * reason it cannot be, or NULL. */
/*
 * J.3's secrets, from the spec's `(secrets (NAME VALUE) ...)`: a file each,
 * 0400, in /run/hull/secrets, on a tmpfs of its own made read-only once
 * they are written, under a tmpfs at the machine's /run, which it must
 * have; nothing is made in the machine's directory, whose /run FHS has
 * emptied at every boot. Those with `(expose env)` join envp_ from
 * `*count`. Returns a reason, or NULL.
 */
static const char *place_secrets(const hull_sexpr_doc *doc, const hull_sexpr_form *machine,
                                 const hull_sexpr_form *given, unsigned *count)
{
	hull_sexpr_form pair;
	hull_sexpr_form name;
	hull_sexpr_form value;
	hull_sexpr_form wanted;
	int             more;

	if (!run_made &&
	    mount("tmpfs", "/machine/run", "tmpfs", MS_NOSUID | MS_NODEV, "mode=0755,size=1m") != 0)
		return "the machine has no /run for its secrets";
	if (mkdir("/machine/run/hull", 0755) != 0 || mkdir("/machine/run/hull/secrets", 0700) != 0 ||
	    mount("tmpfs", "/machine/run/hull/secrets", "tmpfs", MS_NOSUID | MS_NODEV | MS_NOEXEC,
	          "mode=0700,size=16m") != 0)
		return "the secrets' tmpfs could not be made";
	hull_sexpr_child(doc, given, &pair);
	for (more = hull_sexpr_next(doc, &pair, &pair); more; more = hull_sexpr_next(doc, &pair, &pair)) {
		char path[300];
		int  fd;

		if (!hull_sexpr_child(doc, &pair, &name) || !hull_sexpr_next(doc, &name, &value) ||
		    name.kind != HULL_SEXPR_ATOM || value.kind != HULL_SEXPR_ATOM ||
		    name.content_length == 0u || name.content_length > 255u)
			return "a secret is not a name and a value";
		/* The spec's rule for a name: letters, digits, `_` and `-`. */
		for (uint32_t i = 0u; i < name.content_length; i++) {
			char c = (char)doc->data[name.content + i];

			if (!isalnum((unsigned char)c) && c != '_' && c != '-')
				return "a secret's name is not one";
		}
		snprintf(path, sizeof path, "/machine/run/hull/secrets/%.*s", (int)name.content_length,
		         (const char *)doc->data + name.content);
		fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0400);
		if (fd < 0 || write(fd, doc->data + value.content, value.content_length) !=
		                  (ssize_t)value.content_length) {
			if (fd >= 0)
				close(fd);
			return "a secret could not be written";
		}
		close(fd);
	}
	if (mount(NULL, "/machine/run/hull/secrets", NULL,
	          MS_REMOUNT | MS_RDONLY | MS_NOSUID | MS_NODEV | MS_NOEXEC, NULL) != 0)
		return "the secrets could not be made read-only";

	/* The ones the machine puts in its environment. */
	if (!hull_access_find(doc, machine, "secrets", &wanted))
		return NULL;
	hull_sexpr_child(doc, &wanted, &pair);
	for (more = hull_sexpr_next(doc, &pair, &pair); more; more = hull_sexpr_next(doc, &pair, &pair)) {
		char           *expose = hull_access_value(doc, &pair, "expose");
		hull_sexpr_form other;
		int             exposed = expose != NULL && strcmp(expose, "env") == 0;
		int             found = 0;

		free(expose);
		if (!exposed || !hull_sexpr_child(doc, &pair, &name))
			continue;
		hull_sexpr_child(doc, given, &other);
		while (!found && hull_sexpr_next(doc, &other, &other)) {
			hull_sexpr_form key;

			if (hull_sexpr_child(doc, &other, &key) &&
			    key.content_length == name.content_length &&
			    memcmp(doc->data + key.content, doc->data + name.content,
			           name.content_length) == 0 &&
			    hull_sexpr_next(doc, &key, &value))
				found = 1;
		}
		if (!found || *count == MAX_ENV ||
		    memchr(doc->data + value.content, '\0', value.content_length) != NULL ||
		    (envp_[*count] = malloc(name.content_length + value.content_length + 2u)) == NULL)
			return "a secret to expose has no value, holds a NUL, or is one env name too many";
		sprintf(envp_[(*count)++], "%.*s=%.*s", (int)name.content_length,
		        (const char *)doc->data + name.content, (int)value.content_length,
		        (const char *)doc->data + value.content);
	}
	return NULL;
}

/*
 * (ssh true)'s material, from the spec's `(ssh (host_key V)(authorized_keys
 * V)(config V))` the core generates and delivers: each written 0400 in
 * /run/hull/ssh on a tmpfs of its own under the machine's /run, as secrets
 * are, so the image's sshd reads its host key, the user's authorized_keys
 * and a minimal config from absolute paths and nothing is left in the
 * machine's tree. Called before the pivot, as place_secrets is. Returns a
 * reason, or NULL.
 */
static const char *place_ssh(const hull_sexpr_doc *doc, const hull_sexpr_form *given)
{
	static const char *const names[] = { "host_key", "authorized_keys", "config", NULL };

	if (!run_made &&
	    mount("tmpfs", "/machine/run", "tmpfs", MS_NOSUID | MS_NODEV, "mode=0755,size=1m") != 0)
		return "the machine has no /run for its ssh material";
	run_made = 1;
	if (mkdir("/machine/run/hull", 0755) != 0 && errno != EEXIST)
		return "the ssh material's directory could not be made";
	if (mkdir("/machine/run/hull/ssh", 0700) != 0 ||
	    mount("tmpfs", "/machine/run/hull/ssh", "tmpfs", MS_NOSUID | MS_NODEV | MS_NOEXEC,
	          "mode=0700,size=1m") != 0)
		return "the ssh material's tmpfs could not be made";
	for (unsigned i = 0u; names[i] != NULL; i++) {
		hull_sexpr_form pair;
		hull_sexpr_form key;
		hull_sexpr_form value;
		char            path[256];
		int             fd;

		/* `(host_key VALUE)` etc.: find the pair, then its value atom; the
		 * value is binary (a host key has NUL-free but newline-bearing
		 * bytes), so it is taken by offset, not as a C string. */
		if (!hull_access_find(doc, given, names[i], &pair) ||
		    !hull_sexpr_child(doc, &pair, &key) || !hull_sexpr_next(doc, &key, &value) ||
		    value.kind != HULL_SEXPR_ATOM)
			return "the ssh material is not host_key, authorized_keys and config";
		snprintf(path, sizeof path, "/machine/run/hull/ssh/%s", names[i]);
		fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0400);
		if (fd < 0 || write(fd, doc->data + value.content, value.content_length) !=
		                  (ssize_t)value.content_length) {
			if (fd >= 0)
				close(fd);
			return "an ssh material file could not be written";
		}
		close(fd);
	}
	if (mount(NULL, "/machine/run/hull/ssh", NULL,
	          MS_REMOUNT | MS_RDONLY | MS_NOSUID | MS_NODEV | MS_NOEXEC, NULL) != 0)
		return "the ssh material could not be made read-only";
	return NULL;
}

/*
 * `text` written to `staged` in /hull and bound read-only over `target`,
 * a file of the machine's: made where it is absent or a link and the
 * root's writes are a tmpfs's, and refused where they would be the user's
 * own tree. A reason, or NULL.
 */
static const char *bind_named(const char *text, const char *staged, const char *target,
                              int made)
{
	static char reason[200];
	struct stat file;
	FILE       *out = fopen(staged, "w");
	int         fd;

	if (out == NULL || fputs(text, out) == EOF || fclose(out) != 0) {
		snprintf(reason, sizeof reason, "%s cannot be written", staged);
		return reason;
	}
	if (lstat(target, &file) != 0 || !S_ISREG(file.st_mode)) {
		if (lstat(target, &file) == 0 && !S_ISLNK(file.st_mode)) {
			snprintf(reason, sizeof reason, "an addressed segment's names go over %s, which "
			         "is not a file", target + strlen("/machine"));
			return reason;
		}
		if (!made) {
			snprintf(reason, sizeof reason, "an addressed segment's names go over %s, which "
			         "is not in the machine, and is made only above an overlay",
			         target + strlen("/machine"));
			return reason;
		}
		mkdir("/machine/etc", 0755);
		if ((unlink(target) != 0 && errno != ENOENT) ||
		    (fd = open(target, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644)) < 0) {
			snprintf(reason, sizeof reason, "%s cannot be made", target + strlen("/machine"));
			return reason;
		}
		close(fd);
	}
	if (mount(staged, target, NULL, MS_BIND, NULL) != 0 ||
	    mount(NULL, target, NULL, MS_BIND | MS_REMOUNT | MS_RDONLY, NULL) != 0) {
		snprintf(reason, sizeof reason, "the names cannot be bound over %s",
		         target + strlen("/machine"));
		return reason;
	}
	return NULL;
}

/*
 * The segments hull addresses, the spec's `(names ...)` (Appendix D): the
 * address on each one's NIC, which is up, and every sibling's name in
 * /hull/hosts, bound over the machine's /etc/hosts as resolv.conf is --
 * made where the root's writes are a tmpfs's, and refused where they
 * would be the user's own tree. Before the pivot. A reason, or NULL.
 */
static const char *place_names(const hull_sexpr_doc *doc, const hull_sexpr_form *machine,
                               const hull_sexpr_form *names, int made)
{
	hull_sexpr_form segments;
	hull_sexpr_form item;
	unsigned        nic = 0u;
	char           *hosts;
	char           *network = hull_access_value(doc, machine, "network");
	char           *bridge = hull_access_value(doc, machine, "bridge");
	const char     *reason;
	int             uplinked;
	int             routed;
	int             routed6;
	int             owned;
	int             fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);

	/* A segment's NIC follows passt's, which (network none) has not. */
	if (network == NULL || strcmp(network, "none") != 0)
		nic = 1u;
	uplinked = nic != 0u;
	routed   = uplinked;   /* passt's, or a bridge's, default route */
	routed6  = uplinked;
	/* On a bridge its owner named, the machine's addressing is its own. */
	owned = network != NULL && strcmp(network, "bridge") == 0 && bridge != NULL;
	free(network);
	free(bridge);
	if (fd < 0)
		return "a segment's NIC cannot be set";
	if (hull_access_find(doc, machine, "segments", &segments) &&
	    hull_sexpr_child(doc, &segments, &item))
		for (; hull_sexpr_next(doc, &item, &item); nic++) {
			struct ifreq        request;
			struct sockaddr_in *mask;
			char               *segment = hull_access_segment_name(doc, &item);
			char                address[16];
			char                address6[46];
			unsigned            prefix = 0u;
			unsigned            prefix6 = 0u;
			int                 named = segment != NULL &&
			                            hull_access_named(doc, names, segment, address, &prefix);
			int                 named6 = segment != NULL &&
			                             hull_access_named6(doc, names, segment, address6, &prefix6);
			int                 gatewayed = (named || named6) &&
			                                hull_access_gateway(doc, names, segment);

			free(segment);
			if (!named && !named6)
				continue;
			memset(&request, 0, sizeof request);
			snprintf(request.ifr_name, sizeof request.ifr_name, "eth%u", nic);
			if (named) {
				set_address(&request.ifr_addr, address);
				if (ioctl(fd, SIOCSIFADDR, &request) != 0) {
					close(fd);
					return "a segment's NIC's IPv4 address cannot be set";
				}
				mask = (struct sockaddr_in *)&request.ifr_netmask;
				memset(&request.ifr_netmask, 0, sizeof request.ifr_netmask);
				mask->sin_family = AF_INET;
				mask->sin_addr.s_addr = htonl(0xffffffffu << (32u - prefix));
				if (ioctl(fd, SIOCSIFNETMASK, &request) != 0) {
					close(fd);
					return "a segment's NIC's netmask cannot be set";
				}
			}
			if (bring_up(fd, request.ifr_name) != 0) {
				close(fd);
				return "a segment's NIC cannot be brought up";
			}
			/* Its IPv6 address, where the segment has an IPv6 network. */
			if (named6 && hull_ip6_add(request.ifr_name, address6, prefix6) != 0) {
				close(fd);
				return "a segment's NIC's IPv6 address cannot be set";
			}
			/* The first segment whose .1 is a gateway is the way out of a
			 * machine with none of its own (Appendix D), and the first whose
			 * ::1 is, by IPv6. */
			if (!routed6 && gatewayed && named6) {
				if (hull_ip6_route(request.ifr_name, address6, prefix6) != 0) {
					close(fd);
					return "a segment's IPv6 route out through its ::1 cannot be set";
				}
				routed6 = 1;
			}
			if (!routed && gatewayed && named) {
				struct rtentry route;
				struct in_addr at;
				char           gateway[16];

				inet_pton(AF_INET, address, &at);
				at.s_addr = htonl((ntohl(at.s_addr) & (0xffffffffu << (32u - prefix))) + 1u);
				inet_ntop(AF_INET, &at, gateway, sizeof gateway);
				memset(&route, 0, sizeof route);
				set_address(&route.rt_dst, "0.0.0.0");
				set_address(&route.rt_genmask, "0.0.0.0");
				set_address(&route.rt_gateway, gateway);
				route.rt_flags = RTF_UP | RTF_GATEWAY;
				route.rt_dev   = request.ifr_name;
				if (ioctl(fd, SIOCADDRT, &route) != 0) {
					close(fd);
					return "a segment's route out through its .1 cannot be set";
				}
				routed = 1;
			}
		}
	close(fd);
	if ((hosts = hull_access_hosts(doc, names)) == NULL)
		return "the spec's names cannot be read";
	reason = bind_named(hosts, "/hull/hosts", "/machine/etc/hosts", made);
	free(hosts);
	if (reason != NULL)
		return reason;
	/* hull at the segments' .1 is the one to ask, which names a sibling
	 * that joins later and forwards the rest to the host's resolvers where
	 * the machine has a way out (Appendix D). Over passt's resolv.conf
	 * only where there is one to cover, or one can be made, as before. */
	if (!owned) {
		char       *resolver = hull_access_resolver(doc, names);
		struct stat file;

		if (resolver == NULL)
			return "the spec's names cannot be read";
		if (!uplinked || made ||
		    (lstat("/machine/etc/resolv.conf", &file) == 0 && S_ISREG(file.st_mode)))
			reason = bind_named(resolver, "/hull/resolv.conf", "/machine/etc/resolv.conf",
			                    made);
		free(resolver);
		if (reason != NULL)
			return reason;
	}
	return NULL;
}

static const char *apply(const hull_sexpr_doc *doc, const hull_sexpr_form *machine,
                         const hull_sexpr_form *secrets, const hull_sexpr_form *ssh,
                         const hull_sexpr_form *names)
{
	hull_sexpr_form entry;
	hull_sexpr_form item;
	hull_sexpr_form security;
	char           *hostname;
	char           *mutability;
	const char     *reason;
	unsigned        n = 0u;
	unsigned        volumes = 0u;
	char            root_name[256];
	int             hosted = cmdline_value("hull.root", root_name, sizeof root_name);
	int             readonly_root = 0;
	int             verity = 0;
	int             layered;
	int             more;

	if (!hull_access_find(doc, machine, "command", &entry))
		return "the spec has no command";
	hull_sexpr_child(doc, &entry, &item);
	for (more = hull_sexpr_next(doc, &item, &item); more; more = hull_sexpr_next(doc, &item, &item)) {
		if (n == MAX_ARGS)
			return "the command has too many arguments";
		if ((argv_[n++] = hull_access_text(doc, &item)) == NULL)
			return "an argument holds a NUL byte";
	}
	if (n == 0u)
		return "the command is empty";

	n = 0u;
	if (hull_access_find(doc, machine, "env", &entry)) {
		hull_sexpr_child(doc, &entry, &item);
		for (more = hull_sexpr_next(doc, &item, &item); more;
		     more = hull_sexpr_next(doc, &item, &item)) {
			hull_sexpr_form name;
			hull_sexpr_form value;
			char           *key;
			char           *text;

			if (n == MAX_ENV)
				return "env has too many names";
			hull_sexpr_child(doc, &item, &name);
			hull_sexpr_next(doc, &name, &value);
			key  = hull_access_text(doc, &name);
			text = hull_access_text(doc, &value);
			if (key != NULL && text != NULL &&
			    (envp_[n] = malloc(strlen(key) + strlen(text) + 2u)) != NULL)
				sprintf(envp_[n], "%s=%s", key, text);
			free(key);
			free(text);
			if (envp_[n++] == NULL)
				return "an env value holds a NUL byte";
		}
	}
	workdir = hull_access_value(doc, machine, "workdir");
	init_ = hull_access_value(doc, machine, "init");
	if (init_ != NULL && init_[0] != '/')
		return "the machine's init is not an absolute path";
	{
		char *name = hull_access_value(doc, machine, "stop_signal");

		stop_signal_ = name != NULL ? hull_access_signal(name) : SIGTERM;
		free(name);
		if (stop_signal_ == 0)
			return "(stop_signal ...) names no signal";
	}
	{
		char *size = hull_access_value(doc, machine, "shm_size");

		snprintf(shm_size, sizeof shm_size, "%s", size != NULL ? size : "");
		free(size);
	}
	user_ = hull_access_value(doc, machine, "user");
	if (user_ != NULL && init_ != NULL)
		return "(user ...) is whom a command runs as; an (init ...) starts its own";
	{
		char *disconnect = hull_access_value(doc, machine, "on_disconnect");

		keep_ = disconnect != NULL && strcmp(disconnect, "keep") == 0;
		free(disconnect);
	}
	if (hull_access_find(doc, machine, "security", &security)) {
		readonly_root = hull_access_true(doc, &security, "readonly_root");
		verity = hull_access_true(doc, &security, "verity");
		no_new_privs  = hull_access_true(doc, &security, "no_new_privs");
	}
	hostname = hull_access_value(doc, machine, "hostname");
	if (hostname != NULL && sethostname(hostname, strlen(hostname)) != 0) {
		free(hostname);
		return "the hostname could not be set";
	}
	free(hostname);
	mutability = hull_access_value(doc, machine, "mutable");
	/* The guest lays its own overlay over the share, a tmpfs upper, so its
	 * writes are fast and held in its own memory, for (mutable overlay)
	 * whatever the share is: a directory in place, or the merged tree of a
	 * host-side overlay where qemu runs (layers, or owners chowned there,
	 * section 4.11), which the guest takes as a read-only lower. The
	 * exceptions are a kept (upper PATH) and an (export PATH): their writes
	 * must reach the host-side overlay, to be kept on the host's disk or
	 * taken as the export's tar, so there the share is the guest's writable
	 * root and the writes cross to it, not to a guest tmpfs. */
	layered    = mutability != NULL && strcmp(mutability, "overlay") == 0 &&
	             !hull_access_find(doc, machine, "upper", &entry) &&
	             !hull_access_find(doc, machine, "export", &entry);
	free(mutability);

	/* A read-only root has nothing for an overlay to hold, so it is the
	 * share itself, read-only, either way. */
	mkdir("/machine", 0755);
	/* (projection block): the root an image the engine built from the
	 * tree for this run, a virtio disk by the tag a share would have,
	 * its writes the image's and gone with it -- scratch, as an overlay
	 * laid here is (section 3.9). */
	{
		char *projection = hull_access_value(doc, machine, "projection");
		int   block_root = projection != NULL && strcmp(projection, "block") == 0;

		char *source = hull_access_value(doc, machine, "source");
		char  image[4200] = "";   /* a composefs: source's image */

		if (source != NULL && strncmp(source, "composefs:", 10u) == 0)
			snprintf(image, sizeof image, "%s", source + 10);
		free(source);
		free(projection);
		if (image[0] != '\0') {
			if ((reason = composefs_root(image, readonly_root, verity)) != NULL)
				return reason;
			layered = 1;
		} else if (block_root) {
			char        named[256];
			char        node[300];
			const char *type;

			/* A hosted export's root, its partition named on the kernel's
			 * command line; else the virtio disk the engine made of it. */
			if (cmdline_value("hull.root", named, sizeof named)) {
				partition_node(named, node, sizeof node);
				if (node[0] == '\0' || hull_access_filesystem(node, &type) == NULL ||
				    mount(node, "/machine", type, readonly_root ? MS_RDONLY : 0u, NULL) != 0) {
					say("the root's partition", node[0] != '\0' ? strerror(errno) : named);
					return "the machine's root partition could not be mounted";
				}
			} else if (mount_block("root", "/machine", readonly_root) != 0) {
				say("the root's image", strerror(errno));
				return "the machine's root image could not be mounted";
			}
			layered = 1;
		} else if (layered && !readonly_root) {
			if ((reason = overlay()) != NULL)
				return reason;
		} else if (share("root", "/machine", readonly_root) != 0) {
			return "the machine's directory could not be mounted";
		}
	}
	/* The machine's resolv.conf names passt's DNS, as docker writes
	 * its own; only over a file, since a link would be followed here,
	 * outside the machine, and only where passt is, which wrote it. */
	{
		struct stat file;

		if (access("/hull/resolv.conf", F_OK) == 0 &&
		    lstat("/machine/etc/resolv.conf", &file) == 0 && S_ISREG(file.st_mode) &&
		    (mount("/hull/resolv.conf", "/machine/etc/resolv.conf", NULL, MS_BIND, NULL) != 0 ||
		     (readonly_root && mount(NULL, "/machine/etc/resolv.conf", NULL,
		                             MS_BIND | MS_REMOUNT | MS_RDONLY, NULL) != 0)))
			say("resolv.conf", strerror(errno));
	}
	if (hull_access_find(doc, machine, "volumes", &entry)) {
		hull_sexpr_child(doc, &entry, &item);
		for (more = hull_sexpr_next(doc, &item, &item); more;
		     more = hull_sexpr_next(doc, &item, &item)) {
			hull_sexpr_form found;
			char            tag[16];
			char            at[4200];
			char           *guest = hull_access_value(doc, &item, "guest");
			char           *mode  = hull_access_value(doc, &item, "mode");
			int             readonly = mode != NULL && strcmp(mode, "ro") == 0;
			int             block = hull_access_find(doc, &item, "block", &found);

			free(mode);
			if (guest == NULL || guest[0] != '/' || strstr(guest, "..") != NULL ||
			    volumes == MAX_VOLUMES) {
				free(guest);
				return "a volume's guest path is not one the agent mounts";
			}
			snprintf(tag, sizeof tag, "v%u", volumes++);
			snprintf(at, sizeof at, "/machine%s", guest);
			free(guest);
			/* A guest path that is not there is made, where the root
			 * can be written. */
			make_path(at);
			if (block ? mount_block(tag, at, readonly) != 0 :
			    hosted ? mount(tag, at, "vboxsf", readonly ? MS_RDONLY : 0u, NULL) != 0 :
			    share(tag, at, readonly) != 0) {
				static char reason_[4400];

				snprintf(reason_, sizeof reason_, "%s could not be mounted at %s: %s",
				         block ? "a volume's image" : "a volume", at + strlen("/machine"),
				         errno == ENODEV && block ? "its disk is not in the machine" :
				         errno == EMEDIUMTYPE && block ? "the guest's kernel mounts no "
				                                         "filesystem it holds" :
				         strerror(errno));
				return reason_;
			}
			if (block && !readonly)
				images[image_count++] = strdup(at + strlen("/machine"));
		}
	}
	if (names != NULL &&
	    (reason = place_names(doc, machine, names, layered && !readonly_root)) != NULL)
		return reason;
	/* The host's modules, which are this kernel's, where modprobe looks,
	 * read-only. Their place is made only above an overlay, whose upper
	 * is a tmpfs: in a directory used in place it would be made in the
	 * user's own tree, unasked, so there it is used only if it is there,
	 * and a root without it boots without them. */
	{
		struct utsname kernel;
		struct stat    place;
		char           at[4200];

		if (uname(&kernel) == 0) {
			snprintf(at, sizeof at, "/machine/lib/modules/%s", kernel.release);
			if (layered && !readonly_root)
				make_path(at);
			if (stat(at, &place) == 0 && S_ISDIR(place.st_mode) && share("modules", at, 1) != 0)
				say("modules", strerror(errno));
		}
	}
	/* Handed to an image's init, /run as a distribution's initramfs leaves
	 * it, which the init keeps rather than makes: initramfs-tools and
	 * dracut both mount it nosuid, nodev and noexec, and systemd making
	 * its own gives no noexec, so a unit's exec restriction resting on it
	 * went unenforced, which netcfgd's sandbox_writes.sh control found. A
	 * root without /run boots without it, said. */
	if (init_ != NULL) {
		struct stat place;

		if (stat("/machine/run", &place) != 0 || !S_ISDIR(place.st_mode))
			say("/run", "the machine has none for its init");
		else if (mount("tmpfs", "/machine/run", "tmpfs", MS_NOSUID | MS_NODEV | MS_NOEXEC,
		               "mode=0755,size=10%") != 0)
			say("/run", strerror(errno));
		else
			run_made = 1;
	}
	if (hull_access_find(doc, machine, "tmpfs", &entry)) {
		hull_sexpr_child(doc, &entry, &item);
		for (more = hull_sexpr_next(doc, &item, &item); more;
		     more = hull_sexpr_next(doc, &item, &item)) {
			char  at[4200];
			char *path = hull_access_text(doc, &item);

			if (path == NULL || path[0] != '/' || strstr(path, "..") != NULL) {
				free(path);
				return "a tmpfs path is not one the agent mounts";
			}
			snprintf(at, sizeof at, "/machine%s", path);
			free(path);
			make_path(at);
			if (mount("tmpfs", at, "tmpfs", MS_NOSUID | MS_NODEV, "mode=1777") != 0) {
				static char reason_[4400];

				snprintf(reason_, sizeof reason_, "a tmpfs could not be mounted at %s: %s",
				         at + strlen("/machine"), strerror(errno));
				return reason_;
			}
		}
	}
	/* (hosts ...) after what the machine's /etc/hosts says by now, its own or
	 * its segments', and (dns ...) over its resolver. */
	{
		static char base[8192];
		FILE       *file = fopen("/machine/etc/hosts", "r");
		char       *dns = hull_access_dns(doc, machine);
		char       *hosts;
		int         wrong;

		snprintf(base, sizeof base, "127.0.0.1\tlocalhost\n");
		if (file != NULL) {
			base[fread(base, 1u, sizeof base - 1u, file)] = '\0';
			fclose(file);
		}
		hosts = hull_access_extra_hosts(doc, machine, base, &wrong);
		if (wrong)
			return "(hosts ...) is each NAME:ADDRESS";
		reason = hosts != NULL ? bind_named(hosts, "/hull/hosts.more", "/machine/etc/hosts", 1) : NULL;
		free(hosts);
		if (reason == NULL && dns != NULL)
			reason = bind_named(dns, "/hull/resolv.more", "/machine/etc/resolv.conf", 1);
		free(dns);
		if (reason != NULL)
			return reason;
	}
	{
		int count = hull_access_strings(doc, machine, "ulimits", limits_, MAX_LIMITS);

		if (count < 0)
			return "(ulimits ...) has too many entries";
		limit_count = (unsigned)count;
		for (unsigned i = 0u; i < limit_count; i++) {
			unsigned long long soft;
			unsigned long long hard;
			int                resource;

			if (hull_access_limit(limits_[i], &resource, &soft, &hard) != 0)
				return "(ulimits ...) is each NAME=SOFT or NAME=SOFT:HARD, soft at most hard";
		}
		if ((count = hull_access_strings(doc, machine, "sysctls", sysctls_, MAX_LIMITS)) < 0)
			return "(sysctls ...) has too many entries";
		sysctl_count = (unsigned)count;
		for (unsigned i = 0u; i < sysctl_count; i++) {
			char        path[4200];
			const char *value;

			if (hull_access_sysctl(sysctls_[i], path, sizeof path, &value) != 0)
				return "(sysctls ...) is each NAME=VALUE";
		}
	}
	if (secrets != NULL && (reason = place_secrets(doc, machine, secrets, &n)) != NULL)
		return reason;
	ssh_enabled = hull_access_true(doc, machine, "ssh");
	if (ssh_enabled) {
		char named_root[256];

		if (ssh == NULL)
			return "(ssh true) asked for, but no ssh material was delivered";
		if ((reason = place_ssh(doc, ssh)) != NULL)
			return reason;
		/* The fetched sshd, where the engine shared it: used only where
		 * the image has none (below). */
		if (mkdir("/machine/run/hull/sshd", 0755) == 0 &&
		    (cmdline_value("hull.root", named_root, sizeof named_root) ?
		     hosted_sshd() : share("sshd", "/machine/run/hull/sshd", 1)) != 0)
			rmdir("/machine/run/hull/sshd");
	}
	/* The kernel's filesystems inside, where the machine has a place for
	 * them; then the directory becomes /. */
	if (mount("proc", "/machine/proc", "proc", 0u, NULL) != 0 && errno != ENOENT)
		say("proc", strerror(errno));
	if (mount("devtmpfs", "/machine/dev", "devtmpfs", 0u, NULL) != 0 && errno != ENOENT)
		say("dev", strerror(errno));
	if (mount("sysfs", "/machine/sys", "sysfs", 0u, NULL) != 0 && errno != ENOENT)
		say("sys", strerror(errno));
	/* A terminal for an exec is a pseudo-terminal of the machine's own. */
	mkdir("/machine/dev/pts", 0755);
	if (mount("devpts", "/machine/dev/pts", "devpts", 0u, "ptmxmode=0666") != 0 &&
	    errno != ENOENT)
		say("devpts", strerror(errno));
	/* What devtmpfs leaves to userspace and a container has made for it:
	 * without these `> /dev/stderr` fails, or as root makes a plain file, and
	 * shared memory has nowhere to be. Where the machine has a /dev at all. */
	{
		static const char *const links[][2] = {
			{ "/machine/dev/fd", "/proc/self/fd" }, { "/machine/dev/stdin", "/proc/self/fd/0" },
			{ "/machine/dev/stdout", "/proc/self/fd/1" },
			{ "/machine/dev/stderr", "/proc/self/fd/2" },
		};

		for (unsigned i = 0u; i < sizeof links / sizeof links[0]; i++)
			if (symlink(links[i][1], links[i][0]) != 0 && errno != EEXIST && errno != ENOENT)
				say("dev", strerror(errno));
		mkdir("/machine/dev/shm", 01777);
		{
			char options[64];

			snprintf(options, sizeof options, "mode=1777%s%s", shm_size[0] != '\0' ? ",size=" : "",
			         shm_size);
			if (mount("tmpfs", "/machine/dev/shm", "tmpfs", MS_NOSUID | MS_NODEV, options) != 0 &&
			    errno != ENOENT)
				say("/dev/shm", strerror(errno));
		}
	}
	if (chdir("/machine") != 0 || mount(".", "/", NULL, MS_MOVE, NULL) != 0 ||
	    chroot(".") != 0 || chdir("/") != 0)
		return "the machine's directory could not become its root";
	/* Its (sysctls ...), the guest's kernel being its own. */
	for (unsigned i = 0u; i < sysctl_count; i++) {
		char        path[4200];
		const char *value;
		FILE       *file;

		if (hull_access_sysctl(sysctls_[i], path, sizeof path, &value) == 0 &&
		    ((file = fopen(path, "w")) == NULL || fputs(value, file) == EOF || fclose(file) != 0)) {
			static char reason_[4400];

			snprintf(reason_, sizeof reason_, "(sysctls ...) %s: %s", sysctls_[i], strerror(errno));
			return reason_;
		}
	}
	/* The image's own sshd, now that its root is in place: the first of the
	 * usual two paths that is executable. Absent, the dropbear `hull sshd
	 * fetch` put beside the machine (section 8.1 item 3), which logs in as
	 * the image's root: a root with no passwd entry for root, or a / that
	 * others may write, which dropbear holds against every key, is refused
	 * here rather than given an sshd no key can log in to. Neither, and
	 * (ssh true) is refused rather than run silently without one
	 * (section 6). */
	if (ssh_enabled) {
		const char *found;

		if ((reason = hull_sshd_choose(&found)) != NULL)
			return reason;
		if (found != NULL)
			snprintf(ssh_bin, sizeof ssh_bin, "%s", found);
		else
			ssh_fetched = 1;
	}
	applied = 1;
	return NULL;
}

/* --- channels ----------------------------------------------------------- */

static channel *channel_of(uint32_t ch)
{
	for (unsigned i = 0u; i < MAX_CHANNELS; i++)
		if (channels[i].used && channels[i].ch == ch)
			return &channels[i];
	return NULL;
}

static channel *workload(void)
{
	return channel_of(0u);
}

static void close_fd(int *fd)
{
	if (*fd >= 0)
		close(*fd);
	*fd = -1;
}

/* Close a channel's input; a terminal's master stays, being its output. */
static void close_input(channel *c)
{
	/* A socket's other descriptor holds it open: its write side is shut. */
	if (c->socket && c->in >= 0)
		shutdown(c->in, SHUT_WR);
	if (c->tty < 0)
		close_fd(&c->in);
	else
		c->in = -1;
	c->queued = 0u;
}

/* Fork `argv` for channel `ch`, with the machine's environment, workdir
 * and no_new_privs, `cwd` and `extra` over them. On a terminal of `size`
 * when `size` is not NULL; with stdin from /dev/null when `input` is 0. */
/* hull_tree_tar_fs's sink for an exec of (tree true): stdout, whole. */
static int tree_out(void *context, const uint8_t *data, size_t n)
{
	(void)context;
	while (n > 0u) {
		ssize_t wrote = write(1, data, n);

		if (wrote < 0 && errno == EINTR)
			continue;
		if (wrote <= 0)
			return -1;
		data += wrote;
		n -= (size_t)wrote;
	}
	return 0;
}

static const char *spawn(uint32_t ch, char *const *argv, const char *cwd, char *const *extra,
                         const struct winsize *size, int input)
{
	channel *c = NULL;
	char    *envp[2u * MAX_ENV + 1u];
	unsigned n = 0u;
	int      in[2] = { -1, -1 }, out[2] = { -1, -1 }, err[2] = { -1, -1 };
	int      master = -1;
	char    *slave = NULL;
	pid_t    pid;

	if (!applied)
		return "nothing runs before the spec";
	if (channel_of(ch) != NULL)
		return "the channel is in use";
	for (unsigned i = 0u; i < MAX_CHANNELS && c == NULL; i++)
		if (!channels[i].used)
			c = &channels[i];
	if (c == NULL)
		return "the machine runs as many commands as it can";
	for (unsigned i = 0u; envp_[i] != NULL; i++)
		envp[n++] = envp_[i];
	for (unsigned i = 0u; extra != NULL && extra[i] != NULL; i++)
		envp[n++] = extra[i];
	envp[n] = NULL;

	if (size != NULL) {
		master = posix_openpt(O_RDWR | O_NOCTTY | O_CLOEXEC);
		if (master < 0 || grantpt(master) != 0 || unlockpt(master) != 0 ||
		    (slave = ptsname(master)) == NULL || ioctl(master, TIOCSWINSZ, size) != 0) {
			close_fd(&master);
			return "no terminal could be opened";
		}
	} else if ((input && pipe2(in, O_CLOEXEC) != 0) || pipe2(out, O_CLOEXEC) != 0 ||
	           pipe2(err, O_CLOEXEC) != 0) {
		close_fd(&in[0]);
		close_fd(&in[1]);
		close_fd(&out[0]);
		close_fd(&out[1]);
		return "pipe";
	}
	pid = fork();
	if (pid < 0) {
		close_fd(&master);
		close_fd(&in[0]);
		close_fd(&in[1]);
		close_fd(&out[0]);
		close_fd(&out[1]);
		close_fd(&err[0]);
		close_fd(&err[1]);
		return "fork";
	}
	if (pid == 0) {
		sigset_t none;

		sigemptyset(&none);
		sigprocmask(SIG_SETMASK, &none, NULL);
		setsid();
		if (slave != NULL) {
			int tty = open(slave, O_RDWR);

			if (tty < 0 || ioctl(tty, TIOCSCTTY, 0) != 0)
				_exit(126);
			dup2(tty, 0);
			dup2(tty, 1);
			dup2(tty, 2);
			if (tty > 2)
				close(tty);
		} else {
			int null = input ? in[0] : open("/dev/null", O_RDONLY);

			if (null >= 0)
				dup2(null, 0);
			dup2(out[1], 1);
			dup2(err[1], 2);
		}
		if (cwd != NULL && chdir(cwd) != 0) {
			fprintf(stderr, "hull agent: workdir %s: %s\n", cwd, strerror(errno));
			_exit(127);
		}
		for (unsigned i = 0u; i < limit_count && argv[0] != NULL; i++) {
			unsigned long long soft;
			unsigned long long hard;
			struct rlimit      limit;
			int                resource;

			if (hull_access_limit(limits_[i], &resource, &soft, &hard) != 0)
				continue;
			limit.rlim_cur = (rlim_t)soft;
			limit.rlim_max = (rlim_t)hard;
			if (setrlimit(resource, &limit) != 0) {
				fprintf(stderr, "hull agent: %s: %s\n", limits_[i], strerror(errno));
				_exit(126);
			}
		}
		/* A (tree true) is hull's own, and reads what the user may not. */
		if (user_ != NULL && argv[0] != NULL) {
			static char home[300];
			hull_user   user;
			const char *wrong = hull_user_resolve("", user_, &user);
			unsigned    i = 0u;

			if (wrong != NULL || hull_user_become(&user) != 0) {
				fprintf(stderr, "hull agent: %s\n", wrong != NULL ? wrong : strerror(errno));
				_exit(126);
			}
			while (i < n && strncmp(envp[i], "HOME=", 5u) != 0)
				i++;
			if (i == n) {
				snprintf(home, sizeof home, "HOME=%s", user.home);
				envp[n] = home;
				envp[n + 1u] = NULL;
			}
		}
		if (no_new_privs)
			prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0);
		if (ch == 0u && notices[0] != '\0' && write(2, notices, strlen(notices)) < 0) {
			/* its stderr is gone; nothing to tell */
		}
		/* (tree true): no command, the machine's root as a tar on
		 * stdout, by the writer an (export PATH) uses, for `hull commit`
		 * of a running machine. */
		if (argv[0] == NULL) {
			char detail[600];
			int  failed = chdir("/") != 0 ||
			              hull_tree_tar_fs("/", 1, tree_out, NULL, detail, sizeof detail) != 0;

			if (failed)
				fprintf(stderr, "hull: the machine's tree: %s\n", detail);
			_exit(failed ? 1 : 0);
		}
		hull_access_execute(argv, envp);
		fprintf(stderr, "hull agent: %s: %s\n", argv[0], strerror(errno));
		_exit(127);
	}
	memset(c, 0, sizeof *c);
	c->used = 1;
	c->ch   = ch;
	c->pid  = pid;
	c->tty  = master;
	if (master >= 0) {
		c->out = master;
		c->err = -1;
		c->in  = master;
	} else {
		close(out[1]);
		close(err[1]);
		c->out = out[0];
		c->err = err[0];
		c->in  = -1;
		if (input) {
			close(in[0]);
			c->in = in[1];
		}
	}
	if (c->in >= 0)
		fcntl(c->in, F_SETFL, fcntl(c->in, F_GETFL) | O_NONBLOCK);
	c->credited = 1;
	c->credit   = WINDOW;
	if (c->in >= 0 && (c->input = malloc(WINDOW)) == NULL)
		close_input(c);
	{
		hull_msg m = { 0 };

		hull_msg_raw(&m, "(7:started");
		hull_msg_channel(&m, ch);
		hull_msg_raw(&m, "(3:pid");
		hull_msg_number(&m, pid);
		hull_msg_raw(&m, "))");
		send_msg(&m);
	}
	if (c->in >= 0)
		send_credit(ch, WINDOW);
	return NULL;
}

/* (init PATH): PID 1 becomes the image's init, and the agent goes on in a
 * child of it, the reaper of what it starts, as it was. PID 1's signals
 * are given back first, since a mask outlives exec; and should the exec
 * fail, PID 1 reaps for ever rather than end, which would panic the
 * kernel. */
static const char *hand_over(void)
{
	pid_t   agent;
	int     execed[2];
	char    failed;
	ssize_t got;

	if (access(init_, X_OK) != 0)
		return "the machine has no init it can run at its (init ...) path";
	if (pipe2(execed, O_CLOEXEC) != 0)
		return "the agent could not make a pipe";
	agent = fork();
	if (agent < 0)
		return "the agent could not be forked";
	if (agent > 0) {
		char *const argv[] = { init_, NULL };
		sigset_t    none;

		close(execed[0]);
		sigemptyset(&none);
		sigprocmask(SIG_SETMASK, &none, NULL);
		signal(SIGCHLD, SIG_DFL);
		signal(SIGPIPE, SIG_DFL);
		execv(init_, argv);
		say("init", strerror(errno));
		if (write(execed[1], "x", 1u) != 1) {
			/* the agent reads the pipe's end alone, and goes on */
		}
		close(execed[1]);
		for (;;)
			if (wait(NULL) < 0)
				pause();
	}
	/* Not before the image's init is PID 1, as chroot's init waits: the
	 * exec closes the pipe, and a failed one says so on it. Without the
	 * wait the workload could run, and look, while PID 1 was the agent. */
	close(execed[1]);
	while ((got = read(execed[0], &failed, 1u)) < 0 && errno == EINTR)
		;
	close(execed[0]);
	if (got > 0)
		return "the image's init could not be run as PID 1";
	handed = 1;
	if (prctl(PR_SET_CHILD_SUBREAPER, 1L, 0L, 0L, 0L) != 0)
		say("subreaper", strerror(errno));
	return NULL;
}

/* The image's sshd, or the fetched dropbear, supervised beside the
 * workload: a child off every channel, so its exit is reaped like any
 * orphan and never ends the machine (which only channel 0 does). It reads
 * its host key, authorized_keys and config from the tmpfs place_ssh made,
 * by absolute path. */
static void start_sshd(void)
{
	pid_t    child = fork();
	sigset_t none;

	if (child != 0)
		return;
	/* The agent's SIGCHLD is blocked for its signalfd, and a blocked mask
	 * survives exec: an sshd that inherits it never hears a login's shell
	 * exit, so every session held open until the machine went. */
	sigemptyset(&none);
	sigprocmask(SIG_SETMASK, &none, NULL);
	signal(SIGCHLD, SIG_DFL);
	signal(SIGPIPE, SIG_DFL);
	setsid();
	if (ssh_fetched) {
		hull_sshd_exec_dropbear("22");
		_exit(127);
	}
	execl(ssh_bin, "sshd", "-D", "-e", "-f", "/run/hull/ssh/config",
	      "-h", "/run/hull/ssh/host_key", (char *)NULL);
	_exit(127);
}

static const char *start(void)
{
	const char *reason;

	if (workload() != NULL)
		return "the workload has been started already";
	if (init_ != NULL && !handed && (reason = hand_over()) != NULL)
		return reason;
	if (ssh_enabled)
		start_sshd();
	return spawn(0u, argv_, workdir, NULL, NULL, 0);
}

static const struct {
	const char *name;
	int         number;
} signals[] = {
	{ "HUP", SIGHUP }, { "INT", SIGINT }, { "QUIT", SIGQUIT }, { "TERM", SIGTERM },
	{ "USR1", SIGUSR1 }, { "USR2", SIGUSR2 }, { "KILL", SIGKILL },
};

/* B.3 spells a signal SIGTERM; the frontend protocol, TERM. Either. */
static int signal_named(const char *name)
{
	if (strncmp(name, "SIG", 3u) == 0)
		name += 3;
	for (unsigned i = 0u; i < sizeof signals / sizeof signals[0]; i++)
		if (strcmp(signals[i].name, name) == 0)
			return signals[i].number;
	return 0;
}

static void settle(channel *c);

/* The agent reaps what it signals, so a pid it holds is still the
 * process's own. */
static void signal_channel(channel *c, int number)
{
	/* A connection has no process: any signal resets it, so the
	 * machine's end hears, and it is done. A shutdown() would not do:
	 * after SHUT_RD Linux goes on taking what the machine sends, and a
	 * close then is a FIN, so a writer in the machine blocks, never told.
	 * Its pid is 0, which kill() must never see. */
	if (c != NULL && c->socket) {
		struct linger now = { 1, 0 };

		if (c->out >= 0)
			setsockopt(c->out, SOL_SOCKET, SO_LINGER, &now, sizeof now);
		close_fd(&c->in);
		close_fd(&c->out);
		c->queued = 0u;
		/* Done now, said now: the main loop settles a connection only
		 * after a poll, and with nothing else to wake it that poll is
		 * for ever. ssh signals its ProxyCommand as it exits, so this
		 * is how most `hull proxy-ssh` sessions end, and one in a few
		 * left the frontend waiting until the machine went. */
		settle(c);
	} else if (c != NULL && !c->reaped) {
		kill(c->pid, number);
	}
}

/* Begin a stop: SIGTERM now, SIGKILL once `timeout_ms` has passed. */
static void begin_stop(long long timeout_ms)
{
	if (stopping)
		return;
	stopping = 1;
	clock_gettime(CLOCK_MONOTONIC, &kill_at);
	kill_at.tv_sec  += (time_t)(timeout_ms / 1000);
	kill_at.tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
	if (kill_at.tv_nsec >= 1000000000L) {
		kill_at.tv_sec++;
		kill_at.tv_nsec -= 1000000000L;
	}
	signal_channel(workload(), stop_signal_);
}

/* Milliseconds until the stop's SIGKILL, or -1 for none pending. */
static int until_kill(void)
{
	struct timespec now;
	long long       left;
	channel        *w = workload();

	if (!stopping || w == NULL || w->reaped)
		return -1;
	clock_gettime(CLOCK_MONOTONIC, &now);
	left = (long long)(kill_at.tv_sec - now.tv_sec) * 1000 +
	       (kill_at.tv_nsec - now.tv_nsec) / 1000000L;
	if (left <= 0) {
		signal_channel(w, SIGKILL);
		return -1;
	}
	return left > 1000 ? 1000 : (int)left;
}

/* Read what one of a channel's outputs holds, within its credit; 0 once
 * it is closed. A terminal's master reads EIO once nothing holds the
 * other end, which is its end. */
static int drain(channel *c, int *fd, int number)
{
	uint8_t buffer[16384];
	size_t  want = sizeof buffer;
	ssize_t n;

	/* A datagram whole, an empty one too, on no credit. A refusal is the
	 * ICMP of one sent before, and the client may send again. */
	if (c->datagram) {
		static uint8_t datagram[65536];

		n = recv(*fd, datagram, sizeof datagram, 0);
		if (n >= 0)
			send_data(c->ch, number, datagram, (size_t)n);
		else if (errno != EAGAIN && errno != EINTR && errno != ECONNREFUSED)
			close_fd(fd);
		return n >= 0;
	}
	if (c->credited && c->credit < want)
		want = c->credit;
	if (want == 0u)
		return 0;
	n = read(*fd, buffer, want);
	if (n > 0) {
		send_data(c->ch, number, buffer, (size_t)n);
		if (c->credited)
			c->credit -= (size_t)n;
		return 1;
	}
	if (n < 0 && (errno == EINTR || (errno == EAGAIN && !c->reaped)))
		return 0;
	if (*fd == c->tty)
		c->in = c->tty = -1;
	close_fd(fd);
	return 0;
}

/* Write what the host sent as far as the command takes it, and give the
 * host that much credit back. */
static void feed(channel *c)
{
	size_t wrote = 0u;

	while (c->in >= 0 && c->queued > 0u) {
		size_t  tail = (c->head + c->queued) % WINDOW;
		size_t  n    = c->head < tail ? c->queued : WINDOW - c->head;
		ssize_t done = write(c->in, c->input + c->head, n);

		(void)tail;
		if (done < 0 && (errno == EAGAIN || errno == EINTR))
			break;
		if (done <= 0) {
			/* The command's input has closed: what waits is dropped,
			 * its credit given back, and the host told. */
			wrote += c->queued;
			close_input(c);
			send_on("(3:eof", c->ch);
			break;
		}
		c->head    = (c->head + (size_t)done) % WINDOW;
		c->queued -= (size_t)done;
		wrote     += (size_t)done;
	}
	if (c->in >= 0 && c->queued == 0u && c->input_ended && c->tty < 0)
		close_input(c);
	send_credit(c->ch, wrote);
}

/* A channel whose process is reaped and whose output has been read is
 * done: an exec says so; the workload ends the machine. */
static void finish(void);

static void settle(channel *c)
{
	if ((!c->reaped && !c->socket) || c->out >= 0 || c->err >= 0)
		return;
	if (c->ch == 0u) {
		finish();
		return;
	}
	send_status(c->ch, c->status);
	close_fd(&c->in);
	free(c->input);
	memset(c, 0, sizeof *c);
}

/* A reaped channel's output read while there is any and credit for it,
 * then closed, and its end said once both are. */
static void wind_up(channel *c)
{
	while (c->out >= 0 && (!c->credited || c->credit > 0u) && drain(c, &c->out, 1))
		;
	while (c->err >= 0 && (!c->credited || c->credit > 0u) && drain(c, &c->err, 2))
		;
	if (c->out >= 0 && (!c->credited || c->credit > 0u))
		close_fd(&c->out);
	if (c->err >= 0 && (!c->credited || c->credit > 0u))
		close_fd(&c->err);
	settle(c);
}

/* --- the host's messages ------------------------------------------------- */

/* A believed host back: the workload's start as it was said, what it wrote
 * meanwhile, and its end if it has ended. */
static void finish(void);

static void resume(void)
{
	channel *w = workload();
	hull_msg m = { 0 };
	size_t   at = 0u;

	believed = 1;
	hull_msg_raw(&m, "(7:started");
	hull_msg_channel(&m, 0u);
	hull_msg_raw(&m, "(3:pid");
	hull_msg_number(&m, w->pid);
	hull_msg_raw(&m, "))");
	send_msg(&m);
	while (at < held_length) {
		uint32_t length;

		memcpy(&length, held + at + 1u, sizeof length);
		send_data(0u, held[at], held + at + 5u, length);
		at += 5u + length;
	}
	held_length = 0u;
	if (awaiting) {
		awaiting = 0;
		finish();
	}
}

/* The host has gone under keep: what its frontends ran ends with them. */
static void drop_channels(void)
{
	for (unsigned i = 0u; i < MAX_CHANNELS; i++) {
		channel *c = &channels[i];

		if (!c->used || c->ch == 0u)
			continue;
		if (c->socket) {
			close_fd(&c->out);
			c->in = -1;
		} else if (!c->reaped) {
			kill(c->pid, SIGKILL);
		}
	}
}

/* `(connect (ch n) (port n))`: a connection to the machine's own address
 * on `port`, as docker's proxy makes one to a container's. A connect that
 * has not been answered in two seconds is refused. With `(proto udp)` it
 * is a udp socket connected there, one client's datagrams. */
static const char *connect_port(const hull_sexpr_doc *body, uint32_t ch)
{
	hull_sexpr_form    value;
	long long          port = 0;
	channel           *c = NULL;
	struct sockaddr_in to;
	int                fd;
	int                error = 0;
	int                udp;
	socklen_t          length = sizeof error;

	if (!applied)
		return "nothing runs before the spec";
	if (ch == 0u)
		return "channel 0 is the workload's";
	if (channel_of(ch) != NULL)
		return "the channel is in use";
	if (!hull_msg_find(body, "port", &value) || !hull_msg_long(body, &value, &port) ||
	    port < 1 || port > 65535)
		return "a port is 1 to 65535";
	udp = hull_msg_find(body, "proto", &value);
	for (unsigned i = 0u; i < MAX_CHANNELS && c == NULL; i++)
		if (!channels[i].used)
			c = &channels[i];
	if (c == NULL)
		return "the machine runs as many commands as it can";
	memset(&to, 0, sizeof to);
	to.sin_family = AF_INET;
	to.sin_port   = htons((uint16_t)port);
	inet_pton(AF_INET, GUEST_ADDRESS, &to.sin_addr);
	fd = socket(AF_INET, (udp ? SOCK_DGRAM : SOCK_STREAM) | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
	if (fd < 0)
		return strerror(errno);
	if (udp && connect(fd, (struct sockaddr *)&to, sizeof to) != 0)
		error = errno;
	else if (!udp && connect(fd, (struct sockaddr *)&to, sizeof to) != 0) {
		struct pollfd wait = { fd, POLLOUT, 0 };

		if (errno != EINPROGRESS)
			error = errno;
		else if (poll(&wait, 1u, 2000) != 1)
			error = ETIMEDOUT;
		else if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &length) != 0)
			error = errno;
	}
	if (error != 0) {
		close(fd);
		return strerror(error);
	}
	memset(c, 0, sizeof *c);
	c->input = udp ? NULL : malloc(WINDOW);
	c->in    = fcntl(fd, F_DUPFD_CLOEXEC, 0);
	if ((!udp && c->input == NULL) || c->in < 0) {
		free(c->input);
		close_fd(&c->in);
		close(fd);
		c->input = NULL;
		return "out of memory";
	}
	c->used     = 1;
	c->ch       = ch;
	c->out      = fd;
	c->err      = -1;
	c->tty      = -1;
	c->credited = !udp;
	c->credit   = WINDOW;
	c->socket   = 1;
	c->datagram = udp;
	{
		hull_msg m = { 0 };

		hull_msg_raw(&m, "(7:started");
		hull_msg_channel(&m, ch);
		hull_msg_raw(&m, "(3:pid1:0))");
		send_msg(&m);
	}
	send_credit(ch, WINDOW);
	return NULL;
}

/* `(exec (argv ...) (ch n) (cwd p)? (env ...)? (tty ...) (user u)?)`, or
 * `(tree true)` in place of the argv: the machine's root as a tar. */
static const char *exec(const hull_sexpr_doc *body, const hull_sexpr_form *message, uint32_t ch)
{
	hull_sexpr_form entry;
	hull_sexpr_form item;
	hull_sexpr_form tty;
	char           *argv[MAX_ARGS + 1u] = { NULL };
	char           *extra[MAX_ENV + 1u] = { NULL };
	char           *cwd;
	struct winsize  size = { 0 };
	unsigned        n = 0u;
	int             more;
	const char     *reason = NULL;

	if (ch == 0u)
		return "channel 0 is the workload's";
	if (hull_access_find(body, message, "user", &entry))
		return "an exec as another user is not supported yet";
	if (hull_access_find(body, message, "argv", &entry)) {
		hull_sexpr_child(body, &entry, &item);
		for (more = hull_sexpr_next(body, &item, &item); more && reason == NULL;
		     more = hull_sexpr_next(body, &item, &item)) {
			if (n == MAX_ARGS)
				reason = "the command has too many arguments";
			else if ((argv[n++] = hull_access_text(body, &item)) == NULL)
				reason = "an argument holds a NUL byte";
		}
	}
	if (reason == NULL && n == 0u && !hull_access_true(body, message, "tree"))
		reason = "an exec names a command";
	n = 0u;
	if (reason == NULL && hull_access_find(body, message, "env", &entry)) {
		hull_sexpr_child(body, &entry, &item);
		for (more = hull_sexpr_next(body, &item, &item); more && reason == NULL;
		     more = hull_sexpr_next(body, &item, &item)) {
			hull_sexpr_form name;
			hull_sexpr_form value;
			char           *key;
			char           *text;

			if (n == MAX_ENV) {
				reason = "env has too many names";
				break;
			}
			hull_sexpr_child(body, &item, &name);
			hull_sexpr_next(body, &name, &value);
			key  = hull_access_text(body, &name);
			text = hull_access_text(body, &value);
			if (key != NULL && text != NULL &&
			    (extra[n] = malloc(strlen(key) + strlen(text) + 2u)) != NULL)
				sprintf(extra[n], "%s=%s", key, text);
			free(key);
			free(text);
			if (extra[n++] == NULL)
				reason = "an env value holds a NUL byte";
		}
	}
	cwd = hull_access_value(body, message, "cwd");
	if (reason == NULL && hull_access_find(body, message, "tty", &tty)) {
		hull_sexpr_form window;
		char           *cols = NULL;
		char           *rows = NULL;

		hull_sexpr_child(body, &tty, &window);
		if (hull_sexpr_next(body, &window, &window) && window.kind == HULL_SEXPR_LIST) {
			cols = hull_access_value(body, &tty, "cols");
			rows = hull_access_value(body, &tty, "rows");
			size.ws_col = (unsigned short)(cols != NULL ? atoi(cols) : 0);
			size.ws_row = (unsigned short)(rows != NULL ? atoi(rows) : 0);
			if (size.ws_col == 0u || size.ws_row == 0u)
				reason = "a terminal is 1 to 65535 each way";
		}
		free(cols);
		free(rows);
	}
	if (reason == NULL)
		reason = spawn(ch, argv, cwd != NULL ? cwd : workdir, extra,
		               size.ws_col != 0u ? &size : NULL, 1);
	free(cwd);
	for (unsigned i = 0u; argv[i] != NULL; i++)
		free(argv[i]);
	/* What spawn() kept of the environment is the child's copy. */
	for (unsigned i = 0u; extra[i] != NULL; i++)
		free(extra[i]);
	return reason;
}

static void handle(const hull_sexpr_doc *body)
{
	hull_sexpr_form message;
	hull_sexpr_form value;
	long long       ch = 0;
	const char     *reason;

	hull_sexpr_first(body, &message);
	if (hull_msg_find(body, "ch", &value))
		hull_msg_long(body, &value, &ch);
	if (hull_msg_is(body, "hello")) {
		hull_msg m = { 0 };

		hull_msg_raw(&m, "(5:hello(13:agent_version");
		hull_msg_text(&m, AGENT_VERSION);
		hull_msg_raw(&m, ")");
		hull_msg_channel(&m, 0u);
		hull_msg_raw(&m, "(8:features)(5:proto1:1))");
		send_msg(&m);
	} else if (!believed && !hull_msg_is(body, "resume")) {
		/* A host after the first, not yet believed: nothing but hello and
		 * resume, and the connection dropped, as for a second one. */
		connected = 0;
	} else if (hull_msg_is(body, "spec")) {
		hull_sexpr_form machine;
		hull_sexpr_form secrets;
		hull_sexpr_form ssh;
		hull_sexpr_form names;

		if (applied)
			reason = "the spec has been applied already";
		else if (!hull_access_find(body, &message, "machine", &machine))
			reason = "the spec holds no machine";
		else
			reason = apply(body, &machine,
			               hull_access_find(body, &message, "secrets", &secrets) ? &secrets : NULL,
			               hull_access_find(body, &message, "ssh", &ssh) ? &ssh : NULL,
			               hull_access_find(body, &message, "names", &names) ? &names : NULL);
		if (reason != NULL)
			send_error(0u, reason);
		else
			send_simple("(5:ready");
	} else if (hull_msg_is(body, "start")) {
		if (hull_msg_find(body, "token", &value) && value.content_length > 0u &&
		    value.content_length <= TOKEN_MAX) {
			memcpy(token_, body->data + value.content, value.content_length);
			token_length = value.content_length;
		}
		if ((reason = start()) != NULL)
			send_error(0u, reason);
	} else if (hull_msg_is(body, "resume")) {
		uint8_t differ = 1u;

		if (keep_ && workload() != NULL && token_length > 0u &&
		    hull_msg_find(body, "token", &value) && value.content_length == token_length) {
			differ = 0u;
			for (size_t i = 0u; i < token_length; i++)
				differ |= (uint8_t)(token_[i] ^ body->data[value.content + i]);
		}
		if (differ || believed) {
			say("resume", "refused");
			connected = 0;
		} else {
			resume();
		}
	} else if (hull_msg_is(body, "signal")) {
		char name[16];
		int  number;

		if (!hull_msg_find(body, "signal", &value) ||
		    !hull_msg_string(body, &value, name, sizeof name) ||
		    (number = signal_named(name)) == 0)
			send_error((uint32_t)ch, "not a signal the agent forwards");
		else if (channel_of((uint32_t)ch) == NULL)
			send_error((uint32_t)ch, "no such channel");
		else
			signal_channel(channel_of((uint32_t)ch), number);
	} else if (hull_msg_is(body, "exec")) {
		if ((reason = exec(body, &message, (uint32_t)ch)) != NULL)
			send_error((uint32_t)ch, reason);
	} else if (hull_msg_is(body, "sub")) {
		char what[16] = "";

		if (hull_msg_find(body, "what", &value) &&
		    hull_msg_string(body, &value, what, sizeof what) && strcmp(what, "stats") == 0)
			send_guest_stats((uint32_t)ch);
		else
			send_error((uint32_t)ch, "the agent's sub reports only stats");
	} else if (hull_msg_is(body, "connect")) {
		if ((reason = connect_port(body, (uint32_t)ch)) != NULL)
			send_error((uint32_t)ch, reason);
	} else if (hull_msg_is(body, "data")) {
		channel        *c = channel_of((uint32_t)ch);
		hull_sexpr_form buf;

		hull_msg_find(body, "buf", &buf);
		if (c == NULL || ch == 0) {
			send_error((uint32_t)ch, "no such channel");
		} else if (c->datagram) {
			/* Sent whole or dropped, as udp is. */
			if (c->in >= 0)
				send(c->in, body->data + buf.content, buf.content_length,
				     MSG_DONTWAIT | MSG_NOSIGNAL);
		} else if (c->in < 0 || c->input_ended) {
			send_credit(c->ch, buf.content_length);
		} else if (c->queued + buf.content_length > WINDOW) {
			send_error(c->ch, "more input than its credit");
		} else {
			for (size_t i = 0u; i < buf.content_length; i++)
				c->input[(c->head + c->queued + i) % WINDOW] = body->data[buf.content + i];
			c->queued += buf.content_length;
			feed(c);
		}
	} else if (hull_msg_is(body, "eof")) {
		channel *c = channel_of((uint32_t)ch);

		if (c != NULL && ch != 0 && !c->datagram) {
			c->input_ended = 1;
			feed(c);
		}
	} else if (hull_msg_is(body, "winch")) {
		channel       *c = channel_of((uint32_t)ch);
		struct winsize size = { 0 };
		long long      cols = 0;
		long long      rows = 0;

		if (hull_msg_find(body, "cols", &value))
			hull_msg_long(body, &value, &cols);
		if (hull_msg_find(body, "rows", &value))
			hull_msg_long(body, &value, &rows);
		size.ws_col = (unsigned short)cols;
		size.ws_row = (unsigned short)rows;
		if (c == NULL || c->tty < 0 || cols < 1 || cols > 65535 || rows < 1 || rows > 65535 ||
		    ioctl(c->tty, TIOCSWINSZ, &size) != 0)
			send_error((uint32_t)ch, "the command has no terminal");
	} else if (hull_msg_is(body, "credit")) {
		channel  *c = channel_of((uint32_t)ch);
		long long bytes = 0;

		if (hull_msg_find(body, "bytes", &value))
			hull_msg_long(body, &value, &bytes);
		if (c != NULL && c->credited && bytes > 0)
			c->credit += (size_t)bytes;
	} else if (hull_msg_is(body, "stop")) {
		long long timeout = 10000;

		if (hull_msg_find(body, "timeout_ms", &value))
			hull_msg_long(body, &value, &timeout);
		if (workload() == NULL)
			farewell();
		begin_stop(timeout < 0 ? 0 : timeout);
	} else if (hull_msg_is(body, "ping")) {
		struct timespec up;
		hull_msg        m = { 0 };

		clock_gettime(CLOCK_BOOTTIME, &up);
		hull_msg_raw(&m, "(4:pong");
		hull_msg_channel(&m, 0u);
		hull_msg_raw(&m, "(6:uptime");
		hull_msg_number(&m, (long long)up.tv_sec);
		hull_msg_raw(&m, "))");
		send_msg(&m);
	} else {
		send_error((uint32_t)ch, "unsupported");
	}
}

/* --- PID 1 --------------------------------------------------------------- */

/* The host's serial port, where the engine gives one in place of vsock: a
 * virtio-serial port named hull.agent, its node, else empty. */
static char host_port[300];
/* A host dropped from the port and not yet gone from it: what it still
 * writes is its, not the next host's, and a port carries no boundary
 * between them as a new vsock connection does. */
static int  port_dropped;
/* The host's port is a UART, named by hull.host= on the kernel's command
 * line: a machine that gives no virtio-serial port, VirtualBox's. */
static int  port_uart;
/* What the agent says on a UART once it may be written to (listen_host). */
#define UART_READY "hull-agent ready\n"
#define PORT_WAIT 3000u /* looks for a port the engine said is coming, 10 ms apart */

/* The port named hull.agent among `ports`, in host_port. */
static void find_named_port(DIR *ports)
{
	struct dirent *port;

	while (ports != NULL && host_port[0] == '\0' && (port = readdir(ports)) != NULL) {
		char  path[300];
		char  name[64] = "";
		FILE *file;

		if (port->d_name[0] == '.')
			continue;
		snprintf(path, sizeof path, "/sys/class/virtio-ports/%s/name", port->d_name);
		if ((file = fopen(path, "r")) == NULL)
			continue;
		if (fgets(name, sizeof name, file) != NULL) {
			name[strcspn(name, "\n")] = '\0';
			if (strcmp(name, "hull.agent") == 0)
				snprintf(host_port, sizeof host_port, "/dev/%s", port->d_name);
		}
		fclose(file);
	}
}

static void find_host_port(void)
{
	DIR  *ports;
	char  named[64];
	int   expected;

	/* hull.host=ttyS1: a UART, which carries no name of its own. */
	expected = cmdline_value("hull.host", named, sizeof named);
	if (expected && strncmp(named, "tty", 3u) == 0 && strchr(named, '/') == NULL) {
		snprintf(host_port, sizeof host_port, "/dev/%s", named);
		port_uart = 1;
		return;
	}
	/* hull.host=port: one is coming. virtio-console announces its ports
	 * by message after its probe, so a machine at init in 200 ms is here
	 * first, and looking once would leave it listening on a vsock it does
	 * not have. Thirty seconds, for TCG. */
	for (unsigned tries = 0u; tries < PORT_WAIT; tries++) {
		ports = opendir("/sys/class/virtio-ports");
		find_named_port(ports);
		if (ports != NULL)
			closedir(ports);
		if (!expected || (host_port[0] != '\0' && access(host_port, R_OK | W_OK) == 0))
			return;
		host_port[0] = '\0';
		nanosleep(&(struct timespec){ .tv_nsec = 10000000 }, NULL);
	}
	say("hull.host", "no port named hull.agent came");
}

/* What is listened on for the host: the vsock port, or the serial port,
 * which is then the connection itself once the host is on it. */
static int listen_host(void)
{
	struct sockaddr_vm address;
	int                fd;

	if (host_port[0] != '\0') {
		int            port = open(host_port, O_RDWR | O_NOCTTY | O_CLOEXEC);
		struct termios raw;

		/* A UART raw: no line discipline between the host's frames and
		 * the wire, no echo of them back, and no hangup from a modem
		 * line that is not there. */
		if (port >= 0 && port_uart && tcgetattr(port, &raw) == 0) {
			cfmakeraw(&raw);
			raw.c_cflag |= CLOCAL | CREAD;
			cfsetispeed(&raw, B115200);
			cfsetospeed(&raw, B115200);
			tcsetattr(port, TCSANOW, &raw);
			/* Opening an 8250 clears its FIFO, and what came before the
			 * line was raw may have been echoed: so the host waits for this
			 * line, below the wire, before it sends a byte (engine/qemu.c
			 * dial). Anything it sent before is thrown away here. */
			tcflush(port, TCIOFLUSH);
			if (write(port, UART_READY, sizeof UART_READY - 1u) < 0)
				say(host_port, strerror(errno));
		}
		return port;
	}
	fd = socket(AF_VSOCK, SOCK_STREAM | SOCK_CLOEXEC, 0);

	memset(&address, 0, sizeof address);
	address.svm_family = AF_VSOCK;
	address.svm_cid    = VMADDR_CID_ANY;
	address.svm_port   = AGENT_PORT;
	if (fd < 0 || bind(fd, (struct sockaddr *)&address, sizeof address) != 0 ||
	    listen(fd, 1) != 0) {
		/* Not left open: keep calls this again while no host comes. */
		if (fd >= 0)
			close(fd);
		return -1;
	}
	return fd;
}

/* The host's connection, where `listener` says one is ready: accepted on
 * vsock; on the serial port the port itself, the host being on it once it
 * has written. */
static int accept_host(int *listener, short revents)
{
	if (host_port[0] == '\0')
		return accept4(*listener, NULL, NULL, SOCK_CLOEXEC);
	/* After a drop: the old host's bytes read and thrown away until it
	 * hangs up, then the port opened again, which discards what is left
	 * unread, so the next host's stream starts at its first byte. */
	if (port_dropped) {
		if (revents & POLLHUP) {
			close(*listener);
			*listener = listen_host();
			port_dropped = 0;
		} else if (revents & POLLIN) {
			char junk[512];

			if (read(*listener, junk, sizeof junk) <= 0)
				port_dropped = 0;
		}
		return -1;
	}
	/* POLLHUP is no host, and a port with none says POLLIN too, a read
	 * not blocking where it would only return 0: a host is data on a port
	 * that is not hung up. Taking the hung-up port, reading its end and
	 * opening it again lost the hello of a host arriving meanwhile, so a
	 * core taking the machine back was never answered, measured. */
	if ((revents & POLLIN) == 0 || (revents & POLLHUP) != 0) {
		struct timespec fifth = { 0, 200000000L };

		nanosleep(&fifth, NULL);   /* not spun on: it says so until one comes */
		return -1;
	}
	return *listener;
}

/* Each channel that has ended said before the machine goes, as the loop
 * would have: the loop reads every output, then settles the channels in
 * their order, so one whose end came in the same turn as the workload's
 * exit came after channel 0 and was never said -- the machine powered off
 * first, and the host ended it as killed with it, a client exiting 137
 * with all its data delivered: 2 runs of 72 of a dial at a load of 15,
 * measured, a connection's end read and unsaid. An exec reaped in that
 * turn is the same case by the loop, though not reproduced: the agent
 * reaps faster than a workload can see an exec's zombie and exit. A
 * connection whose end is waiting is read out too; one still open, or an
 * exec still running, ends with the machine. */
static void settle_ended(void)
{
	for (unsigned i = 0u; i < MAX_CHANNELS; i++) {
		channel *c = &channels[i];

		if (!c->used || c->ch == 0u || c->datagram)
			continue;
		if (c->socket) {
			while (c->out >= 0 && drain(c, &c->out, 1))
				;
			if (c->out < 0)
				settle(c);
		} else if (c->reaped) {
			wind_up(c);
		}
	}
}

/* The workload has ended and said all it will: tell the host, end what is
 * left, and power off. */
static void finish(void)
{
	/* Under keep, only to a host that is there and believed: a host just
	 * connected is not yet, and is told nothing until it resumes. */
	if (keep_ && (!connected || !believed)) {
		awaiting = 1;
		return;
	}
	settle_ended();
	send_status(0u, workload()->status);
	/* What the image's init started is its own to end, as it shuts down. */
	if (!handed)
		kill(-1, SIGKILL);
	farewell();
}

/* --- a bootable export --------------------------------------------------- */

/*
 * `hull export --bootable` (section 3.13) puts /hull/boot in the image's
 * initramfs: `root NAME`, the GPT name of the partition holding the tree,
 * and `init PATH`, what becomes PID 1 in it. There is no host to speak
 * to: the agent mounts that partition, moves the kernel's filesystems
 * into it, makes it / as switch_root does and runs the tree's own init
 * there, the modules it loaded staying loaded. Should nothing be
 * runnable it says so and powers off, rather than panic the kernel.
 */
static void boot_standalone(FILE *boot)
{
	char            line[256];
	char            root[256] = "";
	char            init[256] = "/sbin/init";
	char            node[300] = "";
	const char     *type;

	while (fgets(line, sizeof line, boot) != NULL) {
		line[strcspn(line, "\n")] = '\0';
		if (strncmp(line, "root ", 5u) == 0)
			snprintf(root, sizeof root, "%s", line + 5);
		else if (strncmp(line, "init ", 5u) == 0 && line[5] == '/')
			snprintf(init, sizeof init, "%s", line + 5);
	}
	fclose(boot);
	partition_node(root, node, sizeof node);
	mkdir("/machine", 0755);
	if (node[0] == '\0') {
		say("boot: no partition is named", root);
	} else if (hull_access_filesystem(node, &type) == NULL) {
		say("boot: no filesystem the agent knows on", node);
	} else if (mount(node, "/machine", type, 0u, NULL) != 0) {
		say("boot: the root could not be mounted", strerror(errno));
	} else {
		const char *const places[] = { "/dev", "/proc", "/sys" };
		const char *const inits[] = { init, "/sbin/init", "/etc/init", "/bin/init", "/bin/sh" };

		for (unsigned i = 0u; i < 3u; i++) {
			char at[64];

			snprintf(at, sizeof at, "/machine%s", places[i]);
			if (mount(places[i], at, NULL, MS_MOVE, NULL) != 0)
				umount2(places[i], MNT_DETACH);
		}
		if (chdir("/machine") != 0 || mount(".", "/", NULL, MS_MOVE, NULL) != 0 ||
		    chroot(".") != 0 || chdir("/") != 0) {
			say("boot: the root could not become /", strerror(errno));
		} else {
			/* The kernel's own order after the one named, as it
			 * tries init= and then these. */
			for (unsigned i = 0u; i < sizeof inits / sizeof inits[0]; i++) {
				char *const argv[] = { (char *)inits[i], NULL };

				if (i > 0u && strcmp(inits[i], init) == 0)
					continue;
				execv(inits[i], argv);
				if (i == 0u)
					say(init, strerror(errno));
			}
			say("boot", "the tree has no init it can run");
		}
	}
	sync();
	reboot(RB_POWER_OFF);
	for (;;)
		pause();
}

/* `hull-agent relay PATH`, in a virtualbox machine's outer vm: the unix
 * socket VBoxHeadless serves the guest's UART on joined to stdin and
 * stdout, which a `hull exec` carries to the core (project.md section
 * 12). The socket comes once VirtualBox is up, so it is waited for; both
 * ways end with either side. */
static int relay(const char *path)
{
	struct sockaddr_un address;
	struct pollfd      fds[2];
	char               buffer[16384];
	int                fd = socket(AF_UNIX, SOCK_STREAM, 0);

	signal(SIGPIPE, SIG_IGN);
	memset(&address, 0, sizeof address);
	address.sun_family = AF_UNIX;
	if (fd < 0 || strlen(path) >= sizeof address.sun_path)
		return 1;
	memcpy(address.sun_path, path, strlen(path));
	for (int tries = 0; connect(fd, (struct sockaddr *)&address, sizeof address) != 0; tries++) {
		struct timespec pause = { 0, 100000000L };

		if (tries == 3000)
			return 1;
		nanosleep(&pause, NULL);
	}
	fds[0].fd     = 0;
	fds[1].fd     = fd;
	fds[0].events = fds[1].events = POLLIN;
	for (;;) {
		if (poll(fds, 2u, -1) < 0) {
			if (errno == EINTR)
				continue;
			return 1;
		}
		for (unsigned i = 0u; i < 2u; i++) {
			int     to = i == 0u ? fd : 1;
			ssize_t got;

			if (fds[i].revents == 0)
				continue;
			got = read(fds[i].fd, buffer, sizeof buffer);
			if (got <= 0)
				return 0;
			for (ssize_t done = 0; done < got;) {
				ssize_t put = write(to, buffer + done, (size_t)(got - done));

				if (put <= 0)
					return 0;
				done += put;
			}
		}
	}
}

int main(int argc, char **argv)
{
	sigset_t       mask;
	int            children;
	int            listener;
	hull_wire_status wire_status;

	/* The commit it was built from, as hull's own --version says it, so a
	 * result resting on a vm can name the agent it ran under too. */
	if (argc == 2 && strcmp(argv[1], "--version") == 0) {
		printf("hull agent\ncommit %s\n", HULL_COMMIT);
		return 0;
	}
	if (argc == 3 && strcmp(argv[1], "relay") == 0)
		return relay(argv[2]);
	if (getpid() != 1) {
		fprintf(stderr, "hull agent: runs as a machine's init, PID 1\n");
		return 2;
	}
	mkdir("/dev", 0755);
	mkdir("/proc", 0555);
	mkdir("/sys", 0555);
	mount("devtmpfs", "/dev", "devtmpfs", 0u, NULL);
	mount("proc", "/proc", "proc", 0u, NULL);
	mount("sysfs", "/sys", "sysfs", 0u, NULL);
	{
		int console = open("/dev/hvc0", O_WRONLY | O_CLOEXEC);

		if (console >= 0) {
			dup2(console, 1);
			dup2(console, 2);
		}
	}
	load_modules();
	{
		FILE *boot = fopen("/hull/boot", "r");

		if (boot != NULL)
			boot_standalone(boot);
	}
	link_devices();
	read_notices();
	network();

	sigemptyset(&mask);
	sigaddset(&mask, SIGCHLD);
	sigprocmask(SIG_BLOCK, &mask, NULL);
	children = signalfd(-1, &mask, SFD_CLOEXEC | SFD_NONBLOCK);
	find_host_port();
	listener = listen_host();
	if (listener < 0) {
		say(host_port[0] != '\0' ? host_port : "vsock", strerror(errno));
		power_off();
	}
	/* One controller (B.2): the first connection is the host's, and the
	 * listener is closed so that no second can be made. The serial port
	 * is one connection by its nature, and is waited on until the host
	 * has written to it. */
	{
		int fd = -1;

		while (fd < 0) {
			struct pollfd ready = { listener, POLLIN, 0 };

			if (poll(&ready, 1u, -1) < 0 && errno != EINTR)
				power_off();
			fd = accept_host(&listener, ready.revents);
			if (fd < 0 && host_port[0] == '\0')
				power_off();
		}
		if (fd != listener)
			close(listener);
		listener = -1;
		if (hull_wire_open(&host, fd, HULL_WIRE_FROM_HOST) != 0)
			power_off();
		connected = 1;
	}

	for (;;) {
		struct pollfd fds[2u + 3u * MAX_CHANNELS];
		channel      *owner[2u + 3u * MAX_CHANNELS];
		int           role[2u + 3u * MAX_CHANNELS];  /* 1, 2: output; 0: input */
		nfds_t        n = 0u;
		int           timeout;

		/* The host gone is (on_disconnect stop), with nobody to tell; or,
		 * under keep, the next host is listened for. */
		if (!connected && workload() == NULL)
			power_off();
		if (!connected && !keep_ && !stopping)
			begin_stop(0);
		if (!connected && keep_ && listener < 0) {
			if (host.fd >= 0)
				hull_wire_close(&host);
			drop_channels();
			listener = listen_host();
			/* A virtio port says when its host hangs up; a UART never
			 * does, so after a drop it is taken as it is. */
			port_dropped = host_port[0] != '\0' && !port_uart;
			if (listener < 0)
				say("vsock", strerror(errno));
		}
		fds[n++] = (struct pollfd){ connected ? host.fd : listener, POLLIN, 0 };
		fds[n++] = (struct pollfd){ children, POLLIN, 0 };
		for (unsigned i = 0u; i < MAX_CHANNELS; i++) {
			channel *c = &channels[i];
			int      room = !c->credited || c->credit > 0u;

			if (!c->used)
				continue;
			if (c->out >= 0 && room) {
				owner[n] = c;
				role[n]  = 1;
				fds[n++] = (struct pollfd){ c->out, POLLIN, 0 };
			}
			if (c->err >= 0 && room) {
				owner[n] = c;
				role[n]  = 2;
				fds[n++] = (struct pollfd){ c->err, POLLIN, 0 };
			}
			if (c->in >= 0 && c->queued > 0u) {
				owner[n] = c;
				role[n]  = 0;
				fds[n++] = (struct pollfd){ c->in, POLLOUT, 0 };
			}
		}
		timeout = until_kill();
		if (poll(fds, n, timeout) < 0 && errno != EINTR)
			power_off();
		for (nfds_t i = 2u; i < n; i++) {
			channel *c = owner[i];

			if (fds[i].revents == 0 || !c->used)
				continue;
			if (role[i] == 0)
				feed(c);
			else if (role[i] == 1 && c->out == fds[i].fd)
				drain(c, &c->out, 1);
			else if (role[i] == 2 && c->err == fds[i].fd)
				drain(c, &c->err, 2);
		}
		if (fds[1].revents != 0) {
			struct signalfd_siginfo info;
			pid_t                   pid;
			int                     wstatus;

			while (read(children, &info, sizeof info) > 0)
				;
			while ((pid = waitpid(-1, &wstatus, WNOHANG)) > 0) {
				for (unsigned i = 0u; i < MAX_CHANNELS; i++) {
					channel *c = &channels[i];

					if (!c->used || c->pid != pid)
						continue;
					c->reaped = 1;
					c->status = wstatus;
					/* An orphan can hold its pipes open: what is there
					 * is read, as its credit allows, and they are not
					 * waited on after. */
					if (c->out >= 0)
						fcntl(c->out, F_SETFL, fcntl(c->out, F_GETFL) | O_NONBLOCK);
					if (c->err >= 0)
						fcntl(c->err, F_SETFL, fcntl(c->err, F_GETFL) | O_NONBLOCK);
				}
			}
		}
		/* A reaped channel's output is read while there is any and
		 * credit for it, then closed. */
		for (unsigned i = 0u; i < MAX_CHANNELS; i++) {
			channel *c = &channels[i];

			/* A connection is done once its end has been read; it has
			 * no process, and its socket is not to be closed early. */
			if (c->used && c->socket) {
				settle(c);
				continue;
			}
			if (!c->used || !c->reaped)
				continue;
			wind_up(c);
		}
		if (!connected && listener >= 0 && fds[0].revents != 0) {
			int fd = accept_host(&listener, fds[0].revents);

			if (fd >= 0 && hull_wire_open(&host, fd, HULL_WIRE_FROM_HOST) == 0) {
				if (fd != listener)
					close(listener);
				listener = -1;
				connected = 1;
				believed  = 0;
			} else if (fd >= 0 && fd != listener) {
				close(fd);
			}
		} else if (connected && fds[0].revents != 0) {
			hull_sexpr_doc body;

			wire_status = hull_wire_fill(&host);
			/* Read no further once a message has dropped the host. */
			while (connected && wire_status == HULL_WIRE_OK &&
			       (wire_status = hull_wire_next(&host, &body)) == HULL_WIRE_OK)
				handle(&body);
			if (wire_status == HULL_WIRE_CLOSED || wire_status == HULL_WIRE_BROKEN)
				connected = 0;
		}
	}
}
