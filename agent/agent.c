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
 * output, on channel 0, has no credit: the host never holds it back.
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
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
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

#include <linux/vm_sockets.h>

#include "access.h"
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
static char  *init_;       /* (init PATH): the image's init, PID 1 once handed to */
static int    handed;      /* PID 1 is the image's init, and this is the agent beside it */
static int    run_made;    /* /run is a tmpfs made here, as an initramfs makes it */
static int    no_new_privs;
static int    applied;     /* the spec is applied: the root is the machine's */
static int    ssh_enabled; /* (ssh true): supervise the image's own sshd */
static char   ssh_bin[256]; /* the image's sshd the agent found, absolute */

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

/* Mount the 9p share `tag` at `at`. */
static int share(const char *tag, const char *at, int readonly)
{
	return mount(tag, at, "9p", readonly ? MS_RDONLY : 0u,
	             "trans=virtio,version=9p2000.L,msize=262144");
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
	const char     *reason;
	int             uplinked;
	int             fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);

	/* A segment's NIC follows passt's, which (network none) has not. */
	if (network == NULL || strcmp(network, "none") != 0)
		nic = 1u;
	uplinked = nic != 0u;
	free(network);
	if (fd < 0)
		return "a segment's NIC cannot be set";
	if (hull_access_find(doc, machine, "segments", &segments) &&
	    hull_sexpr_child(doc, &segments, &item))
		for (; hull_sexpr_next(doc, &item, &item); nic++) {
			struct ifreq        request;
			struct sockaddr_in *mask;
			char               *segment = hull_access_segment_name(doc, &item);
			char                address[16];
			unsigned            prefix = 0u;
			int                 named = segment != NULL &&
			                            hull_access_named(doc, names, segment, address, &prefix);

			free(segment);
			if (!named)
				continue;
			memset(&request, 0, sizeof request);
			snprintf(request.ifr_name, sizeof request.ifr_name, "eth%u", nic);
			set_address(&request.ifr_addr, address);
			if (ioctl(fd, SIOCSIFADDR, &request) != 0) {
				close(fd);
				return "a segment's NIC's IPv4 address cannot be set";
			}
			mask = (struct sockaddr_in *)&request.ifr_netmask;
			memset(&request.ifr_netmask, 0, sizeof request.ifr_netmask);
			mask->sin_family = AF_INET;
			mask->sin_addr.s_addr = htonl(0xffffffffu << (32u - prefix));
			if (ioctl(fd, SIOCSIFNETMASK, &request) != 0 || bring_up(fd, request.ifr_name) != 0) {
				close(fd);
				return "a segment's NIC cannot be brought up";
			}
		}
	close(fd);
	if ((hosts = hull_access_hosts(doc, names)) == NULL)
		return "the spec's names cannot be read";
	reason = bind_named(hosts, "/hull/hosts", "/machine/etc/hosts", made);
	free(hosts);
	if (reason != NULL)
		return reason;
	/* With no passt, its segments are its only network, and hull at their
	 * .1 the one to ask; with passt, passt's DNS stays, and the file's
	 * siblings are named by /etc/hosts. */
	if (!uplinked) {
		char *resolver = hull_access_resolver(doc, names);

		if (resolver == NULL)
			return "the spec's names cannot be read";
		reason = bind_named(resolver, "/hull/resolv.conf", "/machine/etc/resolv.conf", made);
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
	int             readonly_root = 0;
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
		char *disconnect = hull_access_value(doc, machine, "on_disconnect");

		keep_ = disconnect != NULL && strcmp(disconnect, "keep") == 0;
		free(disconnect);
	}
	if (hull_access_find(doc, machine, "security", &security)) {
		readonly_root = hull_access_true(doc, &security, "readonly_root");
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
	 * section 4.11), which the guest takes as a read-only lower. The one
	 * exception is a kept (upper PATH): its writes must reach the host's
	 * disk, so there the share is the guest's writable root and the writes
	 * cross to the host-side overlay's kept upper, not a guest tmpfs. */
	layered    = mutability != NULL && strcmp(mutability, "overlay") == 0 &&
	             !hull_access_find(doc, machine, "upper", &entry);
	free(mutability);

	/* A read-only root has nothing for an overlay to hold, so it is the
	 * share itself, read-only, either way. */
	mkdir("/machine", 0755);
	if (layered && !readonly_root) {
		if ((reason = overlay()) != NULL)
			return reason;
	} else if (share("root", "/machine", readonly_root) != 0) {
		return "the machine's directory could not be mounted";
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
			char  tag[16];
			char  at[4200];
			char *guest = hull_access_value(doc, &item, "guest");
			char *mode  = hull_access_value(doc, &item, "mode");
			int   readonly = mode != NULL && strcmp(mode, "ro") == 0;

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
			if (share(tag, at, readonly) != 0)
				return "a volume could not be mounted";
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
	if (secrets != NULL && (reason = place_secrets(doc, machine, secrets, &n)) != NULL)
		return reason;
	ssh_enabled = hull_access_true(doc, machine, "ssh");
	if (ssh_enabled) {
		if (ssh == NULL)
			return "(ssh true) asked for, but no ssh material was delivered";
		if ((reason = place_ssh(doc, ssh)) != NULL)
			return reason;
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
	if (chdir("/machine") != 0 || mount(".", "/", NULL, MS_MOVE, NULL) != 0 ||
	    chroot(".") != 0 || chdir("/") != 0)
		return "the machine's directory could not become its root";
	/* The image's own sshd, now that its root is in place: the first of the
	 * usual two paths that is executable. Absent, (ssh true) is refused
	 * here rather than run silently without one (section 6). Bundling one
	 * for a bare rootfs is a later increment (section 8.1). */
	if (ssh_enabled) {
		static const char *const where[] = { "/usr/sbin/sshd", "/usr/bin/sshd", NULL };
		unsigned                 i;

		for (i = 0u; where[i] != NULL && access(where[i], X_OK) != 0; i++)
			;
		if (where[i] == NULL)
			return "(ssh true): the image has no sshd at /usr/sbin/sshd or /usr/bin/sshd";
		snprintf(ssh_bin, sizeof ssh_bin, "%s", where[i]);
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
		if (no_new_privs)
			prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0);
		if (ch == 0u && notices[0] != '\0' && write(2, notices, strlen(notices)) < 0) {
			/* its stderr is gone; nothing to tell */
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
	c->credited = ch != 0u;
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
	pid_t agent;

	if (access(init_, X_OK) != 0)
		return "the machine has no init it can run at its (init ...) path";
	agent = fork();
	if (agent < 0)
		return "the agent could not be forked";
	if (agent > 0) {
		char *const argv[] = { init_, NULL };
		sigset_t    none;

		sigemptyset(&none);
		sigprocmask(SIG_SETMASK, &none, NULL);
		signal(SIGCHLD, SIG_DFL);
		signal(SIGPIPE, SIG_DFL);
		execv(init_, argv);
		say("init", strerror(errno));
		for (;;)
			if (wait(NULL) < 0)
				pause();
	}
	handed = 1;
	if (prctl(PR_SET_CHILD_SUBREAPER, 1L, 0L, 0L, 0L) != 0)
		say("subreaper", strerror(errno));
	return NULL;
}

/* The image's sshd, supervised beside the workload: a child off every
 * channel, so its exit is reaped like any orphan and never ends the machine
 * (which only channel 0 does). It reads its host key, authorized_keys and
 * config from the tmpfs place_ssh made, by absolute path. */
static void start_sshd(void)
{
	pid_t child = fork();

	if (child != 0)
		return;
	setsid();
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
	signal_channel(workload(), SIGTERM);
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

/* `(exec (argv ...) (ch n) (cwd p)? (env ...)? (tty ...) (user u)?)`. */
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
	hull_access_find(body, message, "argv", &entry);
	hull_sexpr_child(body, &entry, &item);
	for (more = hull_sexpr_next(body, &item, &item); more && reason == NULL;
	     more = hull_sexpr_next(body, &item, &item)) {
		if (n == MAX_ARGS)
			reason = "the command has too many arguments";
		else if ((argv[n++] = hull_access_text(body, &item)) == NULL)
			reason = "an argument holds a NUL byte";
	}
	if (reason == NULL && n == 0u)
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

static int listen_host(void)
{
	struct sockaddr_vm address;
	int                fd = socket(AF_VSOCK, SOCK_STREAM | SOCK_CLOEXEC, 0);

	memset(&address, 0, sizeof address);
	address.svm_family = AF_VSOCK;
	address.svm_cid    = VMADDR_CID_ANY;
	address.svm_port   = AGENT_PORT;
	if (fd < 0 || bind(fd, (struct sockaddr *)&address, sizeof address) != 0 ||
	    listen(fd, 1) != 0)
		return -1;
	return fd;
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
	send_status(0u, workload()->status);
	/* What the image's init started is its own to end, as it shuts down. */
	if (!handed)
		kill(-1, SIGKILL);
	farewell();
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
	link_devices();
	read_notices();
	network();

	sigemptyset(&mask);
	sigaddset(&mask, SIGCHLD);
	sigprocmask(SIG_BLOCK, &mask, NULL);
	children = signalfd(-1, &mask, SFD_CLOEXEC | SFD_NONBLOCK);
	listener = listen_host();
	if (listener < 0) {
		say("vsock", strerror(errno));
		power_off();
	}
	/* One controller (B.2): the first connection is the host's, and the
	 * listener is closed so that no second can be made. */
	{
		int fd = accept4(listener, NULL, NULL, SOCK_CLOEXEC);

		close(listener);
		listener = -1;
		if (fd < 0 || hull_wire_open(&host, fd, HULL_WIRE_FROM_HOST) != 0)
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
		if (!connected && listener >= 0 && fds[0].revents != 0) {
			int fd = accept4(listener, NULL, NULL, SOCK_CLOEXEC);

			if (fd >= 0 && hull_wire_open(&host, fd, HULL_WIRE_FROM_HOST) == 0) {
				close(listener);
				listener = -1;
				connected = 1;
				believed  = 0;
			} else if (fd >= 0) {
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
