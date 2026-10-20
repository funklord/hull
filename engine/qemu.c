/*
 * qemu.c -- the qemu engine: the vm class with KVM and the emu class with
 * TCG, one driver (project.md sections 5.2 and 5.3), run as the user.
 *
 * A machine is a qemu process, booted on the host's own kernel with an
 * initramfs the engine writes for it: the guest agent as /init
 * (agent/agent.c, Appendix B) and the modules it needs to reach its
 * directory and the host, which are the kernel's 9p, vsock, virtio_net
 * and overlay modules and what modules.dep says they need. The machine's
 * directory is shared over qemu's own 9p server, which needs nothing the
 * user does not have, and read-only under (mutable overlay), which the
 * agent layers in the guest; each volume is a share of its own. /dev/kvm and /dev/vhost-vsock are open
 * to the user on hosts that grant them, and nothing here asks for root.
 *
 * The host is always the initiator (B.1): start() connects to the agent
 * on vsock port 1, sends hello and the machine's spec, waits for ready,
 * sends start and waits for started. A thread then reads what the agent
 * sends -- the workload's output on channel 0, its exit, bye -- until bye
 * or the connection's close, shuts the connection, which the agent waits
 * for before it powers off, waits for qemu, and reports the exit as the
 * agent gave it. A machine whose agent never said so died: its console, kept
 * in a file, says why.
 *
 * stop() asks the agent, which signals the workload and powers off; qemu
 * is killed if that has not happened by the timeout and a margin. qemu
 * dies with the thread that started it, PR_SET_PDEATHSIG, as a chroot
 * machine dies with its core: nothing could manage it afterwards.
 *
 * The kernel is the host's, standing in for Appendix F's bundled one, so
 * vm.kernel.bundled is declared emulated and says so.
 *
 * An exec is a channel of the agent's. Its output flows on B.2's credit:
 * the reader queues what arrives, never waiting, and a thread of the
 * stream's own hands it to on_data, which may wait while the frontend is
 * behind, and gives the agent credit back once it has. So one slow
 * frontend holds back its own command and nothing else, and a stream
 * queues at most the agent's 64 KiB. Its stdin flows on the agent's
 * credit the other way: stream_write waits for credit, which
 * stream_eof can end.
 */

#define _GNU_SOURCE

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/prctl.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <arpa/inet.h>
#include <netinet/in.h>

#include <linux/vm_sockets.h>

#include "access.h"
#include "usage.h"
#include "builtin.h"
#include "wire.h"

#define MAX_VOLUMES  64u
#define MAX_PORTS    64u
#define MAX_MODULES  32u
#define AGENT_PORT   1u
#define BOOT_LIMIT   60   /* seconds for the agent to answer, TCG included */
#define CID_TRIES    4
#define MAX_STREAMS  64u
#define WINDOW       65536u  /* what the agent gives, and is given, each way */
#define NETWORK_FD   3       /* passt's socket, in qemu */
#define FLOW_IDLE    90      /* seconds a udp client's flow is kept unused */

/* The guest's network, which the agent sets to the same addresses. */
#define GUEST_ADDRESS   "10.0.2.15"
#define GATEWAY_ADDRESS "10.0.2.2"
#define DNS_ADDRESS     "10.0.2.3"

/* A piece of an exec's output, queued for its stream's thread. */
typedef struct piece {
	struct piece *next;
	int           fd;
	size_t        length;
	uint8_t       bytes[];
} piece;

struct hull_machine {
	hull_core       *core;
	int              emu;          /* TCG, else KVM */
	char            *root;         /* the dir source, without "dir:" */
	int              readonly_root;
	int              overlay;      /* (mutable overlay): the share is read-only */
	char            *volume_host[MAX_VOLUMES];
	int              volume_readonly[MAX_VOLUMES];
	unsigned         volume_count;
	struct in_addr   address;                /* section 8.1's, the core's */
	char             address_text[16];
	unsigned         port_host[MAX_PORTS];   /* on `address` */
	unsigned         port_guest[MAX_PORTS];
	int              port_udp[MAX_PORTS];
	unsigned         port_count;
	int              listeners[MAX_PORTS];   /* -1 once closed */
	unsigned long    memory_mib;
	unsigned         cpus;
	uint8_t         *spec;         /* the plan's (machine ...), for the agent */
	size_t           spec_length;

	char             dir[4096];    /* the machine's files: initrd, console */
	int              network;      /* qemu's end of passt's socket, or -1 */
	pid_t            passt;        /* passt's daemon, from its pid file, or 0 */
	pid_t            qemu;
	hull_wire        wire;
	int              connected;
	pthread_t        reader;
	int              started;
	pthread_mutex_t  lock;         /* `done`, and signalling qemu */
	pthread_mutex_t  send_lock;    /* one message at a time to the agent */
	pthread_cond_t   done_cond;
	int              done;         /* qemu has been reaped */
	int              exited;       /* the agent said how the workload ended */
	int              code;
	int              signal;
	char             detail[512];
	unsigned char    stats[128];   /* what stats() last gave */

	hull_stream     *streams[MAX_STREAMS];  /* under `lock` */
	uint32_t         next_ch;

	pthread_t        forwarder;    /* accepts on the listeners */
	int              forwarding;
	int              wake;         /* an eventfd that ends the forwarder */
	unsigned         connections;  /* forwarded, under `lock` */
	pthread_cond_t   quiet;        /* `connections` fell */
};

struct hull_stream {
	hull_machine   *machine;
	hull_core      *core;
	uint32_t        ch;
	int             tty;
	pthread_t       deliverer;
	pthread_cond_t  changed;      /* under the machine's lock, as all of this */
	piece          *head;
	piece          *tail;
	int             answered;     /* started or an error has come */
	char            refusal[256]; /* the error, if one did */
	int             exited;       /* the agent said how the command ended */
	int             code;
	int             signal;
	int             lost;         /* the connection went first */
	size_t          credit;       /* stdin the agent will take */
	int             ending;       /* stream_eof() */
	int             input_closed; /* the agent said the command's input closed */
	int             forwarded;    /* a port's connection, whose output goes */
	int             sink;         /* to this socket rather than to on_data */
	int             broken;       /* the socket would not take it */
	int             datagram;     /* a udp client's flow, the forwarder's */
	int             listener;     /* the port's socket, which replies leave by */
	struct sockaddr_in peer;      /* the client */
	time_t          last;         /* when a datagram last passed, monotonic */
};

static const hull_host_ops *host;

static void *forward_ports(void *arg);

static hull_status fail(hull_error *error, hull_status code, const char *detail)
{
	if (error != NULL) {
		error->code           = code;
		error->detail         = detail;
		error->runtime_detail = NULL;
	}
	return code;
}

static void release(hull_machine *m)
{
	free(m->root);
	for (unsigned i = 0u; i < m->volume_count; i++)
		free(m->volume_host[i]);
	free(m->spec);
	pthread_mutex_destroy(&m->lock);
	pthread_mutex_destroy(&m->send_lock);
	pthread_cond_destroy(&m->done_cond);
	pthread_cond_destroy(&m->quiet);
	if (m->wake >= 0)
		close(m->wake);
	free(m);
}

/* --- reading the plan ---------------------------------------------------- */

/* J.1's `memory`, "2G" or "512M" or bytes, in MiB; 0 if it is not one. */
static unsigned long mebibytes(const char *text)
{
	char         *end;
	unsigned long n = strtoul(text, &end, 10);

	if (end == text)
		return 0u;
	if (*end == 'G' || *end == 'g')
		return n * 1024u;
	if (*end == 'M' || *end == 'm')
		return n;
	if (*end == 'K' || *end == 'k')
		return n / 1024u;
	return *end == '\0' ? n / (1024u * 1024u) : 0u;
}

static const char *read_plan(hull_machine *m, const hull_sexpr_doc *doc)
{
	hull_sexpr_form top;
	hull_sexpr_form machine;
	hull_sexpr_form entry;
	hull_sexpr_form item;
	char           *text;
	int             more;

	if (!hull_sexpr_first(doc, &top) || !hull_access_find(doc, &top, "machine", &machine))
		return "the plan holds no machine";
	text = hull_access_value(doc, &top, "class");
	m->emu = text != NULL && strcmp(text, "emu") == 0;
	free(text);

	text = hull_access_value(doc, &machine, "source");
	if (text == NULL || strncmp(text, "dir:", 4u) != 0 || text[4] == '\0') {
		free(text);
		return "the qemu engine runs only a dir source so far";
	}
	m->root = hull_access_path(doc, text + 4);
	free(text);
	if (m->root == NULL)
		return "out of memory";

	if (hull_access_find(doc, &machine, "security", &entry))
		m->readonly_root = hull_access_true(doc, &entry, "readonly_root");
	text = hull_access_value(doc, &machine, "mutable");
	m->overlay = text != NULL && strcmp(text, "overlay") == 0;
	free(text);

	m->memory_mib = 512u;
	if ((text = hull_access_value(doc, &machine, "memory")) != NULL) {
		m->memory_mib = mebibytes(text);
		free(text);
		if (m->memory_mib < 64u)
			return "memory is not a size of 64M or more";
	}
	m->cpus = 1u;
	if ((text = hull_access_value(doc, &machine, "cpus")) != NULL) {
		m->cpus = (unsigned)strtoul(text, NULL, 10);
		free(text);
		if (m->cpus < 1u || m->cpus > 64u)
			return "cpus is not 1 to 64";
	}
	/* emu.cores_scale over-provisions vCPUs against TCG's cost (5.3). */
	if (m->emu && hull_access_find(doc, &machine, "tuning", &entry) &&
	    hull_access_find(doc, &entry, "emu", &item) &&
	    (text = hull_access_value(doc, &item, "cores_scale")) != NULL) {
		double scale = strtod(text, NULL);

		free(text);
		if (scale <= 0.0 || scale > 16.0)
			return "emu's cores_scale is not above 0 and at most 16";
		m->cpus = (unsigned)((double)m->cpus * scale + 0.5);
		if (m->cpus < 1u)
			m->cpus = 1u;
	}

	if (hull_access_find(doc, &machine, "volumes", &entry)) {
		hull_sexpr_child(doc, &entry, &item);
		for (more = hull_sexpr_next(doc, &item, &item); more;
		     more = hull_sexpr_next(doc, &item, &item)) {
			char *host_path = hull_access_value(doc, &item, "host");
			char *mode      = hull_access_value(doc, &item, "mode");

			if (m->volume_count == MAX_VOLUMES) {
				free(host_path);
				free(mode);
				return "there are too many volumes";
			}
			m->volume_readonly[m->volume_count] = mode != NULL && strcmp(mode, "ro") == 0;
			m->volume_host[m->volume_count] =
			    host_path != NULL ? hull_access_path(doc, host_path) : NULL;
			free(host_path);
			free(mode);
			if (m->volume_host[m->volume_count++] == NULL)
				return "a volume is missing its host path";
		}
	}

	/* Ports from the machine's address, which the core gives the plan. */
	if (hull_access_find(doc, &machine, "ports", &entry)) {
		text = hull_access_value(doc, &top, "address");
		if (text == NULL || strlen(text) >= sizeof m->address_text ||
		    inet_pton(AF_INET, text, &m->address) != 1) {
			free(text);
			return "the plan gives its ports no address";
		}
		snprintf(m->address_text, sizeof m->address_text, "%s", text);
		free(text);
		hull_sexpr_child(doc, &entry, &item);
		for (more = hull_sexpr_next(doc, &item, &item); more;
		     more = hull_sexpr_next(doc, &item, &item)) {
			char         *host_port = hull_access_value(doc, &item, "host");
			char         *guest     = hull_access_value(doc, &item, "guest");
			char         *proto     = hull_access_value(doc, &item, "proto");
			unsigned long h = host_port != NULL ? strtoul(host_port, NULL, 10) : 0u;
			unsigned long g = guest != NULL ? strtoul(guest, NULL, 10) : 0u;
			int           udp = proto != NULL && strcmp(proto, "udp") == 0;

			free(host_port);
			free(guest);
			free(proto);
			if (m->port_count == MAX_PORTS)
				return "there are too many ports";
			if (h < 1u || h > 65535u || g < 1u || g > 65535u)
				return "a port is a host and a guest port of 1 to 65535";
			m->port_udp[m->port_count]     = udp;
			m->port_host[m->port_count]    = (unsigned)h;
			m->port_guest[m->port_count++] = (unsigned)g;
		}
	}

	/* The spec the agent applies is the plan's own (machine ...). */
	m->spec_length = machine.extent;
	m->spec = malloc(m->spec_length);
	if (m->spec == NULL)
		return "out of memory";
	memcpy(m->spec, doc->data + machine.offset, m->spec_length);
	return NULL;
}

/* --- the initramfs ------------------------------------------------------- */

typedef struct archive {
	FILE    *file;
	unsigned inode;
	int      failed;
} archive;

static void pad4(archive *a, size_t n)
{
	static const char zeros[4];

	if (n % 4u != 0u && fwrite(zeros, 1u, 4u - n % 4u, a->file) != 4u - n % 4u)
		a->failed = 1;
}

/* One newc entry; the kernel's initramfs reader wants no more than this. */
static void entry(archive *a, const char *name, unsigned mode, unsigned rdev_major,
                  unsigned rdev_minor, const uint8_t *data, size_t length)
{
	size_t name_size = strlen(name) + 1u;

	if (fprintf(a->file,
	            "070701%08X%08X%08X%08X%08X%08X%08zX%08X%08X%08X%08X%08zX%08X",
	            ++a->inode, mode, 0u, 0u, 1u, 0u, length, 0u, 0u, rdev_major, rdev_minor,
	            name_size, 0u) < 0 ||
	    fwrite(name, 1u, name_size, a->file) != name_size)
		a->failed = 1;
	pad4(a, 110u + name_size);
	if (length > 0u && fwrite(data, 1u, length, a->file) != length)
		a->failed = 1;
	pad4(a, length);
}

static uint8_t *slurp(const char *path, size_t *length)
{
	FILE    *file = fopen(path, "rb");
	uint8_t *data = NULL;
	long     size;

	if (file == NULL)
		return NULL;
	if (fseek(file, 0, SEEK_END) == 0 && (size = ftell(file)) >= 0 && fseek(file, 0, SEEK_SET) == 0 &&
	    (data = malloc((size_t)size + 1u)) != NULL) {
		if (fread(data, 1u, (size_t)size, file) != (size_t)size) {
			free(data);
			data = NULL;
		} else {
			*length = (size_t)size;
		}
	}
	fclose(file);
	return data;
}

/* The modules the agent needs, with what modules.dep says each needs,
 * dependencies first: relative paths under /lib/modules/<release>. */
typedef struct module_list {
	char    *path[MAX_MODULES];
	unsigned count;
} module_list;

static int listed(const module_list *list, const char *path)
{
	for (unsigned i = 0u; i < list->count; i++)
		if (strcmp(list->path[i], path) == 0)
			return 1;
	return 0;
}

static int add_module(module_list *list, const char *path)
{
	if (listed(list, path))
		return 1;
	if (list->count == MAX_MODULES || (list->path[list->count] = strdup(path)) == NULL)
		return 0;
	list->count++;
	return 1;
}

static const char *find_modules(const char *release, module_list *list)
{
	static const char *const wanted[] = { "9p", "9pnet_virtio", "vmw_vsock_virtio_transport",
	                                      "virtio_net", "overlay" };
	char        path[512];
	char       *dep;
	char       *lines[8192];
	unsigned    count = 0u;
	size_t      length = 0u;
	const char *reason = NULL;

	snprintf(path, sizeof path, "/lib/modules/%s/modules.dep", release);
	dep = (char *)slurp(path, &length);
	if (dep == NULL)
		return "the host kernel's modules.dep cannot be read";
	dep[length] = '\0';
	for (char *line = strtok(dep, "\n"); line != NULL && count < 8192u; line = strtok(NULL, "\n"))
		lines[count++] = line;
	for (unsigned w = 0u; w < sizeof wanted / sizeof wanted[0] && reason == NULL; w++) {
		unsigned i;

		/* A line is `path: dependency...`; the module's name is the
		 * path's last part up to ".ko". */
		for (i = 0u; i < count; i++) {
			const char *colon = strchr(lines[i], ':');
			const char *name;
			size_t      stem = strlen(wanted[w]);

			if (colon == NULL)
				continue;
			for (name = colon; name > lines[i] && name[-1] != '/'; name--)
				;
			if (strncmp(name, wanted[w], stem) == 0 && strncmp(name + stem, ".ko", 3u) == 0)
				break;
		}
		if (i == count) {
			reason = "the host kernel has no 9p, vsock, virtio_net or overlay module for the guest";
			break;
		}
		{
			char     copy[2048];
			char    *deps[MAX_MODULES];
			char    *colon;
			unsigned n = 0u;

			snprintf(copy, sizeof copy, "%s", lines[i]);
			colon = strchr(copy, ':');
			*colon = '\0';
			for (char *word = strtok(colon + 1, " "); word != NULL && n < MAX_MODULES;
			     word = strtok(NULL, " "))
				deps[n++] = word;
			/* modprobe loads the last listed first. */
			while (n > 0u && reason == NULL)
				if (!add_module(list, deps[--n]))
					reason = "too many modules";
			if (reason == NULL && !add_module(list, copy))
				reason = "too many modules";
		}
	}
	free(dep);
	return reason;
}

/* The agent: hull-agent beside the hull binary that is running. */
static int agent_path(char *out, size_t cap)
{
	char    self[PATH_MAX];
	ssize_t n = readlink("/proc/self/exe", self, sizeof self - 1u);
	char   *slash;

	if (n <= 0)
		return 0;
	self[n] = '\0';
	slash = strrchr(self, '/');
	if (slash == NULL)
		return 0;
	*slash = '\0';
	return snprintf(out, cap, "%s/hull-agent", self) < (int)cap;
}

static const char *write_initramfs(const char *dir, const char *release)
{
	char        path[4200];
	char        agent[PATH_MAX];
	module_list list = { { NULL }, 0u };
	archive     a = { NULL, 0u, 0 };
	char        names[MAX_MODULES * 64u] = "";
	uint8_t    *data;
	size_t      length = 0u;
	const char *reason;

	if (!agent_path(agent, sizeof agent) || (data = slurp(agent, &length)) == NULL)
		return "the guest agent, hull-agent beside hull, cannot be read";
	if ((reason = find_modules(release, &list)) != NULL) {
		free(data);
		return reason;
	}
	snprintf(path, sizeof path, "%s/initrd", dir);
	a.file = fopen(path, "wb");
	if (a.file == NULL) {
		free(data);
		return "the machine's initramfs cannot be written";
	}
	entry(&a, "dev", 040755u, 0u, 0u, NULL, 0u);
	entry(&a, "dev/console", 020600u, 5u, 1u, NULL, 0u);
	entry(&a, "proc", 040555u, 0u, 0u, NULL, 0u);
	entry(&a, "sys", 040555u, 0u, 0u, NULL, 0u);
	entry(&a, "hull", 040755u, 0u, 0u, NULL, 0u);
	entry(&a, "hull/lib", 040755u, 0u, 0u, NULL, 0u);
	entry(&a, "init", 0100755u, 0u, 0u, data, length);
	free(data);
	for (unsigned i = 0u; i < list.count; i++) {
		const char *base = strrchr(list.path[i], '/');
		char        name[300];

		base = base != NULL ? base + 1 : list.path[i];
		snprintf(path, sizeof path, "/lib/modules/%s/%s", release, list.path[i]);
		data = slurp(path, &length);
		if (data == NULL) {
			a.failed = 1;
			break;
		}
		snprintf(name, sizeof name, "hull/lib/%s", base);
		entry(&a, name, 0100644u, 0u, 0u, data, length);
		free(data);
		strncat(names, base, sizeof names - strlen(names) - 2u);
		strcat(names, "\n");
	}
	entry(&a, "hull/modules", 0100644u, 0u, 0u, (const uint8_t *)names, strlen(names));
	entry(&a, "TRAILER!!!", 0u, 0u, 0u, NULL, 0u);
	for (unsigned i = 0u; i < list.count; i++)
		free(list.path[i]);
	if (fclose(a.file) != 0 || a.failed)
		return "the machine's initramfs cannot be written";
	return NULL;
}

/* --- qemu ---------------------------------------------------------------- */

/* qemu's option syntax doubles a comma inside a value. */
static void escaped(char *out, size_t cap, const char *path)
{
	size_t at = 0u;

	for (; *path != '\0' && at + 2u < cap; path++) {
		out[at++] = *path;
		if (*path == ',')
			out[at++] = ',';
	}
	out[at] = '\0';
}

/* The machine's network, out: passt, as the user, on a socket pair whose
 * other end becomes qemu's netdev. It gives the guest the address the
 * agent sets, GUEST_ADDRESS, and the host's DNS at DNS_ADDRESS, and it
 * forwards nothing in. --no-map-gw: the gateway is not the host's
 * loopback, so a machine cannot reach what listens there only for the
 * user. passt's parent exits once it is set up, 0 or with the reason in
 * passt.log, and the daemon it leaves ends when qemu's end closes. */
static void last_line(const hull_machine *m, const char *file, char *out, size_t cap);

static const char *start_network(hull_machine *m)
{
	char        log[4200];
	char        pid_file[4200];
	const char *argv[32];
	unsigned    n = 0u;
	int         pair[2];
	int         status;
	pid_t       pid;

	snprintf(log, sizeof log, "%s/passt.log", m->dir);
	snprintf(pid_file, sizeof pid_file, "%s/passt.pid", m->dir);
	argv[n++] = "passt";
	argv[n++] = "--quiet";
	argv[n++] = "--fd";
	argv[n++] = "3";
	argv[n++] = "--one-off";
	argv[n++] = "--ipv4-only";
	argv[n++] = "--address";
	argv[n++] = GUEST_ADDRESS;
	argv[n++] = "--netmask";
	argv[n++] = "24";
	argv[n++] = "--gateway";
	argv[n++] = GATEWAY_ADDRESS;
	argv[n++] = "--no-map-gw";
	argv[n++] = "--dns-forward";
	argv[n++] = DNS_ADDRESS;
	argv[n++] = "--tcp-ports";
	argv[n++] = "none";
	argv[n++] = "--udp-ports";
	argv[n++] = "none";
	argv[n++] = "--pid";
	argv[n++] = pid_file;
	argv[n] = NULL;

	if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, pair) != 0)
		return "the machine's network socket cannot be made";
	pid = fork();
	if (pid == 0) {
		int null = open("/dev/null", O_RDWR);
		int out  = open(log, O_WRONLY | O_CREAT | O_TRUNC, 0600);

		if (null < 0 || out < 0 || dup2(null, 0) < 0 || dup2(out, 1) < 0 || dup2(out, 2) < 0 ||
		    dup2(pair[1], 3) < 0)
			_exit(126);
		syscall(SYS_close_range, 4u, ~0u, 0u);
		execvp(argv[0], (char *const *)argv);
		fprintf(stderr, "passt, which a qemu machine's network needs: %s\n", strerror(errno));
		_exit(127);
	}
	close(pair[1]);
	if (pid < 0) {
		close(pair[0]);
		return "passt cannot be started";
	}
	while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
		;
	if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
		close(pair[0]);
		last_line(m, "passt.log", m->detail, sizeof m->detail);
		if (m->detail[0] == '\0')
			snprintf(m->detail, sizeof m->detail, "passt could not start the network");
		return m->detail;
	}
	m->network = pair[0];
	/* Written by passt's parent before it exits, so here by now. */
	{
		FILE *f = fopen(pid_file, "re");
		long  pid = 0;

		if (f != NULL) {
			if (fscanf(f, "%ld", &pid) != 1)
				pid = 0;
			fclose(f);
		}
		m->passt = pid > 0 ? (pid_t)pid : 0;
	}
	return NULL;
}

/* Fork qemu for `m`, with `cid` for its vsock device. Its stdout and
 * stderr go to qemu.log. */
static pid_t spawn(hull_machine *m, const char *release, uint32_t cid)
{
	char        kernel[300];
	char        initrd[4200];
	char        console[8600];
	char        log[4200];
	char        memory[32];
	char        cpus[16];
	char        vsock[64];
	char        netdev[64];
	char        root[9000];
	char        fsdev[MAX_VOLUMES][9000];
	char        device[MAX_VOLUMES][96];
	char        path[8500];
	const char *argv[64 + 4u * MAX_VOLUMES];
	unsigned    n = 0u;
	pid_t       pid;

	snprintf(kernel, sizeof kernel, "/boot/vmlinuz-%s", release);
	snprintf(initrd, sizeof initrd, "%s/initrd", m->dir);
	snprintf(log, sizeof log, "%s/qemu.log", m->dir);
	escaped(path, sizeof path, m->dir);
	snprintf(console, sizeof console, "file,id=console,path=%s/console.log", path);
	snprintf(memory, sizeof memory, "%lu", m->memory_mib);
	snprintf(cpus, sizeof cpus, "%u", m->cpus);
	snprintf(vsock, sizeof vsock, "vhost-vsock-pci,guest-cid=%u", cid);
	snprintf(netdev, sizeof netdev, "stream,id=net,addr.type=fd,addr.str=%d", NETWORK_FD);
	escaped(path, sizeof path, m->root);
	snprintf(root, sizeof root, "local,id=root,path=%s,security_model=none%s", path,
	         m->readonly_root || m->overlay ? ",readonly=on" : "");

	argv[n++] = "qemu-system-x86_64";
	argv[n++] = "-nodefaults";
	argv[n++] = "-no-user-config";
	argv[n++] = "-display";
	argv[n++] = "none";
	argv[n++] = "-no-reboot";
	argv[n++] = "-M";
	argv[n++] = "q35";
	argv[n++] = "-accel";
	argv[n++] = m->emu ? "tcg" : "kvm";
	argv[n++] = "-cpu";
	argv[n++] = m->emu ? "max" : "host";
	argv[n++] = "-m";
	argv[n++] = memory;
	argv[n++] = "-smp";
	argv[n++] = cpus;
	argv[n++] = "-kernel";
	argv[n++] = kernel;
	argv[n++] = "-initrd";
	argv[n++] = initrd;
	argv[n++] = "-append";
	argv[n++] = "console=hvc0 quiet panic=-1";
	argv[n++] = "-device";
	argv[n++] = "virtio-serial-pci";
	argv[n++] = "-chardev";
	argv[n++] = console;
	argv[n++] = "-device";
	argv[n++] = "virtconsole,chardev=console";
	argv[n++] = "-fsdev";
	argv[n++] = root;
	argv[n++] = "-device";
	argv[n++] = "virtio-9p-pci,fsdev=root,mount_tag=root";
	for (unsigned i = 0u; i < m->volume_count; i++) {
		escaped(path, sizeof path, m->volume_host[i]);
		snprintf(fsdev[i], sizeof fsdev[i], "local,id=v%u,path=%s,security_model=none%s", i,
		         path, m->volume_readonly[i] ? ",readonly=on" : "");
		snprintf(device[i], sizeof device[i], "virtio-9p-pci,fsdev=v%u,mount_tag=v%u", i, i);
		argv[n++] = "-fsdev";
		argv[n++] = fsdev[i];
		argv[n++] = "-device";
		argv[n++] = device[i];
	}
	argv[n++] = "-device";
	argv[n++] = vsock;
	argv[n++] = "-netdev";
	argv[n++] = netdev;
	argv[n++] = "-device";
	argv[n++] = "virtio-net-pci,netdev=net";
	argv[n] = NULL;

	pid = fork();
	if (pid == 0) {
		int null = open("/dev/null", O_RDWR);
		int out  = open(log, O_WRONLY | O_CREAT | O_TRUNC, 0600);

		prctl(PR_SET_PDEATHSIG, SIGKILL);
		setsid();
		if (null < 0 || out < 0)
			_exit(126);
		dup2(null, 0);
		dup2(out, 1);
		dup2(out, 2);
		/* passt's socket at NETWORK_FD, and nothing of the core's: its
		 * sockets, its lock. dup2() onto itself would keep CLOEXEC. */
		if (m->network == NETWORK_FD)
			fcntl(NETWORK_FD, F_SETFD, 0);
		else if (dup2(m->network, NETWORK_FD) < 0)
			_exit(126);
		syscall(SYS_close_range, NETWORK_FD + 1u, ~0u, 0u);
		execvp(argv[0], (char *const *)argv);
		fprintf(stderr, "qemu-system-x86_64: %s\n", strerror(errno));
		_exit(127);
	}
	return pid;
}

/* Is qemu still there? Not reaped: the reader reaps it. */
static int qemu_gone(const hull_machine *m)
{
	siginfo_t info;

	memset(&info, 0, sizeof info);
	return waitid(P_PID, (id_t)m->qemu, &info, WEXITED | WNOHANG | WNOWAIT) != 0 ||
	       info.si_pid == m->qemu;
}

/* The last line `file` in the machine's directory holds, for a reason. */
static void last_line(const hull_machine *m, const char *file, char *out, size_t cap)
{
	char    path[4200];
	size_t  length = 0u;
	uint8_t *data;
	char    *text;
	char    *line;

	out[0] = '\0';
	snprintf(path, sizeof path, "%s/%s", m->dir, file);
	if ((data = slurp(path, &length)) == NULL)
		return;
	text = (char *)data;
	text[length] = '\0';
	while (length > 0u && (text[length - 1u] == '\n' || text[length - 1u] == '\r'))
		text[--length] = '\0';
	line = strrchr(text, '\n');
	snprintf(out, cap, "%s", line != NULL ? line + 1 : text);
	free(data);
}

/* Connect to the agent, while qemu lives, for BOOT_LIMIT seconds. */
static int dial(hull_machine *m, uint32_t cid)
{
	time_t deadline = time(NULL) + BOOT_LIMIT;

	while (time(NULL) < deadline && !qemu_gone(m)) {
		struct sockaddr_vm address;
		struct timeval     patience = { 0, 200000 };
		int                fd = socket(AF_VSOCK, SOCK_STREAM | SOCK_CLOEXEC, 0);

		if (fd < 0)
			return -1;
		setsockopt(fd, AF_VSOCK, SO_VM_SOCKETS_CONNECT_TIMEOUT, &patience, sizeof patience);
		memset(&address, 0, sizeof address);
		address.svm_family = AF_VSOCK;
		address.svm_cid    = cid;
		address.svm_port   = AGENT_PORT;
		if (connect(fd, (struct sockaddr *)&address, sizeof address) == 0)
			return fd;
		close(fd);
		{
			struct timespec pause = { 0, 20000000L };

			nanosleep(&pause, NULL);
		}
	}
	return -1;
}

static void send_msg(hull_machine *m, hull_msg *msg)
{
	pthread_mutex_lock(&m->send_lock);
	if (m->connected && hull_wire_send(m->wire.fd, msg) != 0)
		m->connected = 0;
	pthread_mutex_unlock(&m->send_lock);
	hull_msg_free(msg);
}

/* The agent's next message, within `seconds`. */
static hull_wire_status hear(hull_machine *m, hull_sexpr_doc *body, int seconds)
{
	time_t deadline = time(NULL) + seconds;

	for (;;) {
		struct pollfd    fd = { m->wire.fd, POLLIN, 0 };
		hull_wire_status status = hull_wire_next(&m->wire, body);
		long             left = (long)(deadline - time(NULL));

		if (status != HULL_WIRE_WAIT)
			return status;
		if (left <= 0 || poll(&fd, 1u, (int)(left * 1000)) <= 0)
			return HULL_WIRE_BROKEN;
		status = hull_wire_fill(&m->wire);
		if (status != HULL_WIRE_OK && status != HULL_WIRE_WAIT)
			return status;
	}
}

/* Wait for a reply of `type`; an error's reason goes into m->detail. */
static int expect(hull_machine *m, const char *type)
{
	hull_sexpr_doc  body;
	hull_sexpr_form value;

	if (hear(m, &body, 30) != HULL_WIRE_OK) {
		snprintf(m->detail, sizeof m->detail, "the guest agent did not answer");
		return 0;
	}
	if (hull_msg_is(&body, type))
		return 1;
	if (hull_msg_is(&body, "error") && hull_msg_find(&body, "reason", &value) &&
	    hull_msg_string(&body, &value, m->detail, sizeof m->detail))
		return 0;
	snprintf(m->detail, sizeof m->detail, "the guest agent answered out of turn");
	return 0;
}

static void reap(hull_machine *m)
{
	siginfo_t info;

	while (waitid(P_PID, (id_t)m->qemu, &info, WEXITED | WNOWAIT) < 0 && errno == EINTR)
		;
	pthread_mutex_lock(&m->lock);
	waitpid(m->qemu, NULL, 0);
	m->done = 1;
	pthread_cond_broadcast(&m->done_cond);
	pthread_mutex_unlock(&m->lock);
}

static hull_stream *stream_on(hull_machine *m, long long ch)
{
	for (unsigned i = 0u; i < MAX_STREAMS; i++)
		if (m->streams[i] != NULL && (long long)m->streams[i]->ch == ch)
			return m->streams[i];
	return NULL;
}

/* A message on an exec's channel, from the reader: queued or noted under
 * the lock, never waiting. */
static void for_stream(hull_machine *m, const hull_sexpr_doc *body, long long ch)
{
	hull_stream    *st;
	hull_sexpr_form value;
	long long       number;

	pthread_mutex_lock(&m->lock);
	st = stream_on(m, ch);
	if (st == NULL) {
		pthread_mutex_unlock(&m->lock);
		return;
	}
	if (hull_msg_is(body, "data") && st->datagram) {
		hull_sexpr_form buf;
		struct timespec now;

		/* A reply, to the client, whole or not at all, as udp is. */
		if (hull_msg_find(body, "buf", &buf)) {
			sendto(st->listener, body->data + buf.content, buf.content_length,
			       MSG_DONTWAIT | MSG_NOSIGNAL, (struct sockaddr *)&st->peer, sizeof st->peer);
			clock_gettime(CLOCK_MONOTONIC, &now);
			st->last = now.tv_sec;
		}
	} else if (hull_msg_is(body, "data")) {
		hull_sexpr_form buf;
		hull_sexpr_form fd;
		piece          *p;

		if (hull_msg_find(body, "buf", &buf) && hull_msg_find(body, "fd", &fd) &&
		    (p = malloc(sizeof *p + buf.content_length)) != NULL) {
			p->next   = NULL;
			p->fd     = hull_sexpr_is(body, &fd, "2") ? 2 : 1;
			p->length = buf.content_length;
			memcpy(p->bytes, body->data + buf.content, buf.content_length);
			if (st->tail != NULL)
				st->tail->next = p;
			else
				st->head = p;
			st->tail = p;
		}
	} else if (hull_msg_is(body, "started")) {
		st->answered = 1;
	} else if (hull_msg_is(body, "error")) {
		if (!st->answered) {
			st->answered = 1;
			st->lost     = 1;
			if (!hull_msg_find(body, "reason", &value) ||
			    !hull_msg_string(body, &value, st->refusal, sizeof st->refusal))
				snprintf(st->refusal, sizeof st->refusal, "the agent refused the exec");
		}
	} else if (hull_msg_is(body, "exited")) {
		st->exited = 1;
		if (hull_msg_find(body, "code", &value) && hull_msg_long(body, &value, &number))
			st->code = (int)number;
		if (hull_msg_find(body, "signal", &value) && hull_msg_long(body, &value, &number))
			st->signal = (int)number;
	} else if (hull_msg_is(body, "credit")) {
		if (hull_msg_find(body, "bytes", &value) && hull_msg_long(body, &value, &number) &&
		    number > 0)
			st->credit += (size_t)number;
	} else if (hull_msg_is(body, "eof")) {
		st->input_closed = 1;
	}
	pthread_cond_broadcast(&st->changed);
	pthread_mutex_unlock(&m->lock);
}

static void *read_agent(void *arg)
{
	hull_machine *m = arg;

	for (;;) {
		hull_sexpr_doc   body;
		hull_sexpr_form  value;
		hull_wire_status status = hull_wire_read(&m->wire, &body);
		long long        number;

		if (status == HULL_WIRE_WAIT)
			continue;
		if (status != HULL_WIRE_OK)
			break;
		if (hull_msg_find(&body, "ch", &value) && hull_msg_long(&body, &value, &number) &&
		    number != 0) {
			for_stream(m, &body, number);
		} else if (hull_msg_is(&body, "data")) {
			hull_sexpr_form buf;
			hull_sexpr_form fd;

			if (hull_msg_find(&body, "buf", &buf) && hull_msg_find(&body, "fd", &fd))
				host->on_data(m->core, NULL, hull_sexpr_is(&body, &fd, "2") ? 2 : 1,
				              body.data + buf.content, buf.content_length);
		} else if (hull_msg_is(&body, "exited")) {
			m->exited = 1;
			if (hull_msg_find(&body, "code", &value) && hull_msg_long(&body, &value, &number))
				m->code = (int)number;
			if (hull_msg_find(&body, "signal", &value) && hull_msg_long(&body, &value, &number))
				m->signal = (int)number;
		} else if (hull_msg_is(&body, "bye")) {
			break;
		}
	}
	/* Closed on bye, which the agent waits for before it powers off:
	 * what it sent before bye has all been read. */
	pthread_mutex_lock(&m->send_lock);
	m->connected = 0;
	shutdown(m->wire.fd, SHUT_RDWR);
	pthread_mutex_unlock(&m->send_lock);
	/* The agent has gone: every exec it had not reported ended with the
	 * machine, as a SIGKILL, and every connection ends. Its socket is
	 * shut, so that a deliver() blocked on a client that does not read
	 * ends too; it is closed only once its stream is dropped. */
	pthread_mutex_lock(&m->lock);
	for (unsigned i = 0u; i < MAX_STREAMS; i++) {
		if (m->streams[i] != NULL) {
			m->streams[i]->lost = 1;
			if (m->streams[i]->forwarded)
				shutdown(m->streams[i]->sink, SHUT_RDWR);
			pthread_cond_broadcast(&m->streams[i]->changed);
		}
	}
	pthread_mutex_unlock(&m->lock);
	reap(m);
	if (m->wake >= 0 && eventfd_write(m->wake, 1u) != 0)
		host->log(m->core, 3, "the machine's forwarder could not be woken");
	if (m->exited && m->signal != 0) {
		host->on_exit(m->core, m, NULL, -1, m->signal);
	} else if (m->exited) {
		host->on_exit(m->core, m, NULL, m->code, 0);
	} else {
		char line[400];
		char message[512];

		last_line(m, "console.log", line, sizeof line);
		snprintf(message, sizeof message, "machine died without its agent's word: %s",
		         line[0] != '\0' ? line : "the console is empty");
		host->log(m->core, 3, message);
		host->on_exit(m->core, m, NULL, -1, SIGKILL);
	}
	host->on_state(m->core, m, "stopped");
	return NULL;
}

/* --- operations ---------------------------------------------------------- */

static hull_status qemu_create(hull_core *core, hull_buf plan, hull_machine **out,
                               hull_error *error)
{
	hull_sexpr_doc doc;
	hull_machine  *m;
	const char    *reason;

	if (hull_sexpr_open(&doc, plan.data, plan.len, HULL_SEXPR_CANONICAL, NULL) != HULL_SEXPR_OK)
		return fail(error, HULL_ERR_UNSUPPORTED, "the plan is not canonical");
	m = calloc(1u, sizeof *m);
	if (m == NULL)
		return fail(error, HULL_ERR_INTERNAL, "out of memory");
	m->core = core;
	m->wire.fd = -1;
	m->network = -1;
	m->wake = -1;
	for (unsigned i = 0u; i < MAX_PORTS; i++)
		m->listeners[i] = -1;
	pthread_mutex_init(&m->lock, NULL);
	pthread_mutex_init(&m->send_lock, NULL);
	pthread_cond_init(&m->done_cond, NULL);
	pthread_cond_init(&m->quiet, NULL);
	if ((reason = read_plan(m, &doc)) != NULL) {
		release(m);
		return fail(error, HULL_ERR_UNSUPPORTED, reason);
	}
	*out = m;
	host->on_state(core, m, "created");
	return HULL_OK;
}

static void remove_files(const char *dir)
{
	static const char *const files[] = { "initrd", "console.log", "qemu.log", "passt.log",
	                                     "passt.pid" };
	char path[4200];

	for (unsigned i = 0u; i < sizeof files / sizeof files[0]; i++) {
		snprintf(path, sizeof path, "%s/%s", dir, files[i]);
		unlink(path);
	}
	rmdir(dir);
}

/* A core killed outright leaves its machines' directories. One core runs
 * per user, holding core.lock, so a directory named for a pid that is not
 * running was a dead core's: its files are removed, by name. */
static void sweep(const char *parent)
{
	DIR           *list = opendir(parent);
	struct dirent *item;

	while (list != NULL && (item = readdir(list)) != NULL) {
		char path[4200];
		long pid;
		char *end;

		if (strncmp(item->d_name, "qemu-", 5u) != 0)
			continue;
		pid = strtol(item->d_name + 5, &end, 10);
		if (end == item->d_name + 5 || *end != '-' || pid <= 0 || pid == (long)getpid() ||
		    kill((pid_t)pid, 0) == 0 || errno != ESRCH)
			continue;
		snprintf(path, sizeof path, "%s/%s", parent, item->d_name);
		remove_files(path);
	}
	if (list != NULL)
		closedir(list);
}

/* The machine's directory, under the user's runtime directory. */
static const char *make_dir(hull_machine *m)
{
	const char *runtime = getenv("XDG_RUNTIME_DIR");
	static unsigned counter;
	char        parent[3900];

	if (runtime == NULL || runtime[0] != '/')
		return "XDG_RUNTIME_DIR is not set";
	snprintf(parent, sizeof parent, "%s/hull", runtime);
	mkdir(parent, 0700);
	sweep(parent);
	snprintf(m->dir, sizeof m->dir, "%s/qemu-%ld-%u", parent, (long)getpid(),
	         __atomic_add_fetch(&counter, 1u, __ATOMIC_RELAXED));
	if (mkdir(m->dir, 0700) != 0) {
		m->dir[0] = '\0';
		return "the machine's directory cannot be made";
	}
	return NULL;
}

static void close_listeners(hull_machine *m)
{
	for (unsigned i = 0u; i < m->port_count; i++) {
		if (m->listeners[i] >= 0)
			close(m->listeners[i]);
		m->listeners[i] = -1;
	}
}

/* The machine's directory, and its listeners, which a failed start and
 * destroy() both end with. */
static void remove_dir(hull_machine *m)
{
	close_listeners(m);
	if (m->dir[0] != '\0')
		remove_files(m->dir);
	m->dir[0] = '\0';
}

/* Each port's listener on the machine's address, bound before the machine boots, so a
 * port that is taken refuses the start. A reason, or NULL. */
static const char *open_listeners(hull_machine *m)
{
	for (unsigned i = 0u; i < m->port_count; i++) {
		struct sockaddr_in at;
		int                one = 1;
		int                fd = socket(AF_INET, (m->port_udp[i] ? SOCK_DGRAM : SOCK_STREAM) |
		                               SOCK_CLOEXEC | SOCK_NONBLOCK, 0);

		memset(&at, 0, sizeof at);
		at.sin_family      = AF_INET;
		at.sin_port        = htons((uint16_t)m->port_host[i]);
		at.sin_addr        = m->address;
		if (fd < 0 || setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one) != 0 ||
		    bind(fd, (struct sockaddr *)&at, sizeof at) != 0 ||
		    (!m->port_udp[i] && listen(fd, 64) != 0)) {
			snprintf(m->detail, sizeof m->detail, "port %u on %s: %s", m->port_host[i],
			         m->address_text, strerror(errno));
			if (fd >= 0)
				close(fd);
			close_listeners(m);
			return m->detail;
		}
		m->listeners[i] = fd;
	}
	return NULL;
}

/* Kill qemu, unless it has been reaped and its pid freed. */
static void kill_qemu(hull_machine *m)
{
	pthread_mutex_lock(&m->lock);
	if (!m->done && m->qemu > 0)
		kill(m->qemu, SIGKILL);
	pthread_mutex_unlock(&m->lock);
}

static hull_status qemu_start(hull_core *core, hull_machine *m, hull_error *error)
{
	struct utsname name;
	const char    *reason;
	int            fd = -1;
	hull_msg       msg = { 0 };

	if (m->started || m->qemu > 0)
		return fail(error, HULL_ERR_BUSY, "the machine has been started already");
	if (uname(&name) != 0 || strcmp(name.machine, "x86_64") != 0)
		return fail(error, HULL_ERR_RUNTIME, "the qemu engine boots on x86_64 hosts only so far");
	if (!m->emu && access("/dev/kvm", R_OK | W_OK) != 0)
		return fail(error, HULL_ERR_RUNTIME, "/dev/kvm is not open to this user");
	if (access("/dev/vhost-vsock", R_OK | W_OK) != 0)
		return fail(error, HULL_ERR_RUNTIME, "/dev/vhost-vsock is not open to this user");
	if ((reason = make_dir(m)) != NULL || (reason = write_initramfs(m->dir, name.release)) != NULL ||
	    (reason = open_listeners(m)) != NULL) {
		if (reason != m->detail)
			snprintf(m->detail, sizeof m->detail, "%s", reason);
		remove_dir(m);
		return fail(error, HULL_ERR_RUNTIME, m->detail);
	}

	/* A context id is the host's to share among every VM: one taken
	 * makes qemu refuse it, and another is tried. */
	for (int attempt = 0; attempt < CID_TRIES && fd < 0; attempt++) {
		uint32_t cid = 0u;

		if (getrandom(&cid, sizeof cid, 0) != (ssize_t)sizeof cid)
			cid = (uint32_t)getpid() ^ (uint32_t)time(NULL);
		cid = 3u + cid % 0x7ffffff0u;
		m->done = 0;
		/* A passt each time: the last ended with its qemu. */
		if ((reason = start_network(m)) != NULL) {
			if (reason != m->detail)
				snprintf(m->detail, sizeof m->detail, "%s", reason);
			break;
		}
		m->qemu = spawn(m, name.release, cid);
		close(m->network);
		m->network = -1;
		if (m->qemu < 0)
			break;
		fd = dial(m, cid);
		if (fd < 0) {
			kill_qemu(m);
			waitpid(m->qemu, NULL, 0);
			m->done = 1;
			last_line(m, "qemu.log", m->detail, sizeof m->detail);
			if (strstr(m->detail, "guest cid") == NULL &&
			    strstr(m->detail, "Address already in use") == NULL)
				break;
		}
	}
	if (fd < 0) {
		if (m->detail[0] == '\0')
			snprintf(m->detail, sizeof m->detail, "the guest agent never answered");
		remove_dir(m);
		return fail(error, HULL_ERR_RUNTIME, m->detail);
	}
	if (hull_wire_open(&m->wire, fd, HULL_WIRE_FROM_AGENT) != 0) {
		close(fd);
		kill_qemu(m);
		waitpid(m->qemu, NULL, 0);
		m->done = 1;
		remove_dir(m);
		return fail(error, HULL_ERR_INTERNAL, "out of memory");
	}
	m->connected = 1;
	/* The reader wakes the forwarder when the machine ends, which it
	 * may do as soon as it runs, so `wake` is made before it. */
	if (m->port_count > 0u)
		m->wake = eventfd(0u, EFD_CLOEXEC);

	hull_msg_raw(&msg, "(5:hello(2:ch1:0)(8:features)(5:proto1:1))");
	send_msg(m, &msg);
	if (expect(m, "hello")) {
		hull_msg_raw(&msg, "(4:spec(2:ch1:0)");
		hull_msg_bytes(&msg, m->spec, m->spec_length);
		hull_msg_raw(&msg, ")");
		send_msg(m, &msg);
		if (expect(m, "ready")) {
			hull_msg_raw(&msg, "(5:start(2:ch1:0))");
			send_msg(m, &msg);
			if (expect(m, "started") &&
			    pthread_create(&m->reader, NULL, read_agent, m) == 0) {
				m->started = 1;
				/* Without a forwarder a port's connections wait in
				 * its backlog, unanswered; the machine runs. */
				if (m->port_count > 0u && m->wake >= 0 &&
				    pthread_create(&m->forwarder, NULL, forward_ports, m) == 0)
					m->forwarding = 1;
				else if (m->port_count > 0u)
					host->log(core, 3, "the machine's ports cannot be forwarded");
				host->on_state(core, m, "running");
				return HULL_OK;
			}
		}
	}
	kill_qemu(m);
	waitpid(m->qemu, NULL, 0);
	m->done = 1;
	hull_wire_close(&m->wire);
	remove_dir(m);
	return fail(error, HULL_ERR_RUNTIME, m->detail);
}

static int wait_done(hull_machine *m, int timeout_ms)
{
	struct timespec deadline;
	int             done;

	clock_gettime(CLOCK_REALTIME, &deadline);
	deadline.tv_sec  += timeout_ms / 1000;
	deadline.tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
	if (deadline.tv_nsec >= 1000000000L) {
		deadline.tv_sec++;
		deadline.tv_nsec -= 1000000000L;
	}
	pthread_mutex_lock(&m->lock);
	while (!m->done && pthread_cond_timedwait(&m->done_cond, &m->lock, &deadline) == 0)
		;
	done = m->done;
	pthread_mutex_unlock(&m->lock);
	return done;
}

/* The agent signals the workload, waits `timeout_ms` and kills it, and
 * powers off; qemu is killed if that is not done in five seconds more. */
static hull_status qemu_stop(hull_core *core, hull_machine *m, int timeout_ms, hull_error *error)
{
	hull_msg msg = { 0 };

	(void)error;
	if (!m->started || wait_done(m, 0))
		return HULL_OK;
	host->on_state(core, m, "stopping");
	hull_msg_raw(&msg, "(4:stop(2:ch1:0)(10:timeout_ms");
	hull_msg_number(&msg, timeout_ms);
	hull_msg_raw(&msg, "))");
	send_msg(m, &msg);
	if (!wait_done(m, timeout_ms + 5000)) {
		kill_qemu(m);
		wait_done(m, 60000);
	}
	return HULL_OK;
}

static hull_status qemu_destroy(hull_core *core, hull_machine *m, hull_error *error)
{
	if (m->started) {
		qemu_stop(core, m, 0, error);
		pthread_join(m->reader, NULL);
		/* The reader has woken the forwarder, and every connection's
		 * stream is lost: each ends, and none starts after. */
		if (m->forwarding)
			pthread_join(m->forwarder, NULL);
		pthread_mutex_lock(&m->lock);
		while (m->connections > 0u)
			pthread_cond_wait(&m->quiet, &m->lock);
		pthread_mutex_unlock(&m->lock);
		hull_wire_close(&m->wire);
	}
	remove_dir(m);
	release(m);
	return HULL_OK;
}

static const struct {
	int         number;
	const char *name;
} signal_names[] = {
	{ SIGHUP, "SIGHUP" }, { SIGINT, "SIGINT" }, { SIGQUIT, "SIGQUIT" }, { SIGTERM, "SIGTERM" },
	{ SIGUSR1, "SIGUSR1" }, { SIGUSR2, "SIGUSR2" }, { SIGKILL, "SIGKILL" },
};

/* A signal for the workload, which the agent sends it (B.3, channel 0). */
static hull_status qemu_signal(hull_core *core, hull_machine *m, hull_stream *stream, int signum,
                               hull_error *error)
{
	hull_msg msg = { 0 };

	(void)core;
	if (!m->started || wait_done(m, 0))
		return fail(error, HULL_ERR_GONE, "the machine is not running");
	for (unsigned i = 0u; i < sizeof signal_names / sizeof signal_names[0]; i++) {
		if (signal_names[i].number == signum) {
			hull_msg_raw(&msg, "(6:signal");
			hull_msg_channel(&msg, stream != NULL ? stream->ch : 0u);
			hull_msg_raw(&msg, "(6:signal");
			hull_msg_text(&msg, signal_names[i].name);
			hull_msg_raw(&msg, "))");
			send_msg(m, &msg);
			return HULL_OK;
		}
	}
	return fail(error, HULL_ERR_UNSUPPORTED, "not a signal the agent forwards");
}

/* --- exec --------------------------------------------------------------- */

static void send_credit(hull_machine *m, uint32_t ch, size_t bytes)
{
	hull_msg msg = { 0 };

	hull_msg_raw(&msg, "(6:credit(5:bytes");
	hull_msg_number(&msg, (long long)bytes);
	hull_msg_raw(&msg, ")");
	hull_msg_channel(&msg, ch);
	hull_msg_raw(&msg, ")");
	send_msg(m, &msg);
}

/* Have the agent end the stream's command, or its connection. */
static void kill_stream(hull_machine *m, hull_stream *st)
{
	hull_msg msg = { 0 };

	hull_msg_raw(&msg, "(6:signal");
	hull_msg_channel(&msg, st->ch);
	hull_msg_raw(&msg, "(6:signal7:SIGKILL))");
	send_msg(m, &msg);
}

/* All of `length` to a connection's socket; 0 if it will not take it. */
static int send_all(int fd, const uint8_t *bytes, size_t length)
{
	while (length > 0u) {
		ssize_t n = send(fd, bytes, length, MSG_NOSIGNAL);

		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0)
			return 0;
		bytes += n;
		length -= (size_t)n;
	}
	return 1;
}

/* The stream's own thread: its output to on_data, or to a connection's
 * socket, credit back to the agent once each piece has gone, then its
 * exit, once. A connection's end shuts its socket, which ends its pump. */
static void *deliver(void *arg)
{
	hull_stream  *st = arg;
	hull_machine *m  = st->machine;

	for (;;) {
		piece *p;

		pthread_mutex_lock(&m->lock);
		while (st->head == NULL && !st->exited && !st->lost)
			pthread_cond_wait(&st->changed, &m->lock);
		p = st->head;
		if (p != NULL) {
			st->head = p->next;
			if (st->head == NULL)
				st->tail = NULL;
		}
		pthread_mutex_unlock(&m->lock);
		if (p == NULL)
			break;
		if (!st->forwarded) {
			host->on_data(st->core, st, p->fd, p->bytes, p->length);
		} else if (!st->broken && !send_all(st->sink, p->bytes, p->length)) {
			/* The client has gone: the connection ends, and what
			 * the machine still sends is taken and dropped. */
			st->broken = 1;
			kill_stream(m, st);
		}
		send_credit(m, st->ch, p->length);
		free(p);
	}
	if (st->forwarded) {
		shutdown(st->sink, SHUT_RDWR);
		return NULL;
	}
	if (st->exited && st->signal != 0)
		host->on_exit(st->core, m, st, -1, st->signal);
	else if (st->exited)
		host->on_exit(st->core, m, st, st->code, 0);
	else if (st->answered)
		host->on_exit(st->core, m, st, -1, SIGKILL);
	return NULL;
}

static void free_stream(hull_stream *st)
{
	while (st->head != NULL) {
		piece *next = st->head->next;

		free(st->head);
		st->head = next;
	}
	pthread_cond_destroy(&st->changed);
	free(st);
}

/* A stream on a channel of the machine's own, or NULL with `why` set. */
static hull_stream *new_stream(hull_machine *m, hull_core *core, hull_status *why)
{
	hull_stream *st = calloc(1u, sizeof *st);
	unsigned     slot = MAX_STREAMS;

	*why = HULL_ERR_INTERNAL;
	if (st == NULL)
		return NULL;
	st->machine = m;
	st->core    = core;
	pthread_cond_init(&st->changed, NULL);
	pthread_mutex_lock(&m->lock);
	for (unsigned i = 0u; i < MAX_STREAMS && slot == MAX_STREAMS; i++)
		if (m->streams[i] == NULL)
			slot = i;
	if (slot < MAX_STREAMS) {
		st->ch = ++m->next_ch;
		m->streams[slot] = st;
	}
	pthread_mutex_unlock(&m->lock);
	if (slot == MAX_STREAMS) {
		*why = HULL_ERR_BUSY;
		free_stream(st);
		return NULL;
	}
	return st;
}

static void drop_stream(hull_machine *m, hull_stream *st)
{
	pthread_mutex_lock(&m->lock);
	for (unsigned i = 0u; i < MAX_STREAMS; i++)
		if (m->streams[i] == st)
			m->streams[i] = NULL;
	pthread_mutex_unlock(&m->lock);
}

/* `started`, within ten seconds; else the agent's reason, or that it did
 * not answer, into `reason`. */
static int answered(hull_machine *m, hull_stream *st, char *reason, size_t cap)
{
	struct timespec deadline;
	int             ok;

	clock_gettime(CLOCK_REALTIME, &deadline);
	deadline.tv_sec += 10;
	pthread_mutex_lock(&m->lock);
	while (!st->answered && !st->lost &&
	       pthread_cond_timedwait(&st->changed, &m->lock, &deadline) == 0)
		;
	ok = st->answered && st->refusal[0] == '\0';
	if (!ok)
		snprintf(reason, cap, "%s",
		         st->refusal[0] != '\0' ? st->refusal : "the agent did not answer");
	pthread_mutex_unlock(&m->lock);
	return ok;
}

/* The core's request, `(exec (argv ...) (ch n) (machine id) (tty ...))`,
 * as the agent's: the argv, a channel of this machine's, the terminal. */
static hull_status qemu_exec(hull_core *core, hull_machine *m, hull_buf spec, hull_stream **out,
                             hull_error *error)
{
	hull_sexpr_doc  doc;
	hull_sexpr_form message;
	hull_sexpr_form argv;
	hull_stream    *st;
	hull_msg        msg = { 0 };
	hull_status     why;
	int             cols = 0;
	int             rows = 0;

	if (!m->started || wait_done(m, 0))
		return fail(error, HULL_ERR_GONE, "the machine is not running");
	if (hull_sexpr_open(&doc, spec.data, spec.len, HULL_SEXPR_CANONICAL, NULL) != HULL_SEXPR_OK ||
	    !hull_sexpr_first(&doc, &message) || !hull_access_find(&doc, &message, "argv", &argv))
		return fail(error, HULL_ERR_UNSUPPORTED, "an exec names a command");
	if ((st = new_stream(m, core, &why)) == NULL)
		return fail(error, why, why == HULL_ERR_BUSY ?
		            "the machine runs as many commands as it can" : "out of memory");
	st->tty = hull_access_terminal(&doc, &message, &cols, &rows);

	hull_msg_raw(&msg, "(4:exec");
	hull_msg_bytes(&msg, doc.data + argv.offset, argv.extent);
	hull_msg_channel(&msg, st->ch);
	if (st->tty) {
		hull_msg_raw(&msg, "(3:tty(4:cols");
		hull_msg_number(&msg, cols);
		hull_msg_raw(&msg, ")(4:rows");
		hull_msg_number(&msg, rows);
		hull_msg_raw(&msg, ")))");
	} else {
		hull_msg_raw(&msg, "(3:tty5:false))");
	}
	send_msg(m, &msg);

	if (!answered(m, st, m->detail, sizeof m->detail)) {
		drop_stream(m, st);
		free_stream(st);
		return fail(error, HULL_ERR_RUNTIME, m->detail);
	}
	if (pthread_create(&st->deliverer, NULL, deliver, st) != 0) {
		/* Nothing will deliver it: the command is ended, unseen. */
		drop_stream(m, st);
		kill_stream(m, st);
		free_stream(st);
		return fail(error, HULL_ERR_INTERNAL, "the stream's thread could not start");
	}
	*out = st;
	return HULL_OK;
}

/* One of the machine's ports, declared or not, as a stream: the agent's
 * connect, as a forwarded port's connection is, but with its bytes for
 * on_data rather than a socket of the engine's. */
static hull_status qemu_dial(hull_core *core, hull_machine *m, uint16_t port, hull_stream **out,
                             hull_error *error)
{
	hull_stream *st;
	hull_msg     msg = { 0 };
	hull_status  why;
	char         reason[300];

	if (!m->started || wait_done(m, 0))
		return fail(error, HULL_ERR_GONE, "the machine is not running");
	if ((st = new_stream(m, core, &why)) == NULL)
		return fail(error, why, why == HULL_ERR_BUSY ?
		            "the machine runs as many commands as it can" : "out of memory");
	hull_msg_raw(&msg, "(7:connect");
	hull_msg_channel(&msg, st->ch);
	hull_msg_raw(&msg, "(4:port");
	hull_msg_number(&msg, (long long)port);
	hull_msg_raw(&msg, "))");
	send_msg(m, &msg);
	if (!answered(m, st, reason, sizeof reason)) {
		drop_stream(m, st);
		free_stream(st);
		snprintf(m->detail, sizeof m->detail, "port %u: %s", (unsigned)port, reason);
		return fail(error, HULL_ERR_RUNTIME, m->detail);
	}
	if (pthread_create(&st->deliverer, NULL, deliver, st) != 0) {
		drop_stream(m, st);
		kill_stream(m, st);
		free_stream(st);
		return fail(error, HULL_ERR_INTERNAL, "the stream's thread could not start");
	}
	*out = st;
	return HULL_OK;
}

/* stdin, written whole however long the command takes, on the agent's
 * credit; stream_eof() from another thread ends the wait. */
static hull_status qemu_stream_write(hull_core *core, hull_stream *st, const uint8_t *data,
                                     size_t length, hull_error *error)
{
	hull_machine *m = st->machine;

	(void)core;
	while (length > 0u) {
		hull_msg msg = { 0 };
		size_t   n;

		pthread_mutex_lock(&m->lock);
		while (st->credit == 0u && !st->ending && !st->input_closed && !st->lost &&
		       !st->exited)
			pthread_cond_wait(&st->changed, &m->lock);
		if (st->ending || st->input_closed || st->lost || st->exited) {
			pthread_mutex_unlock(&m->lock);
			return fail(error, HULL_ERR_GONE, "the command's input has closed");
		}
		n = length < st->credit ? length : st->credit;
		st->credit -= n;
		pthread_mutex_unlock(&m->lock);
		hull_msg_raw(&msg, "(4:data(3:buf");
		hull_msg_atom(&msg, data, n);
		hull_msg_raw(&msg, ")");
		hull_msg_channel(&msg, st->ch);
		hull_msg_raw(&msg, "(2:fd1:0))");
		send_msg(m, &msg);
		data += n;
		length -= n;
	}
	return HULL_OK;
}

static hull_status qemu_stream_eof(hull_core *core, hull_stream *st, hull_error *error)
{
	hull_machine *m = st->machine;
	hull_msg      msg = { 0 };
	int           first;

	(void)core;
	(void)error;
	pthread_mutex_lock(&m->lock);
	first = !st->ending;
	st->ending = 1;
	pthread_cond_broadcast(&st->changed);
	pthread_mutex_unlock(&m->lock);
	if (first) {
		hull_msg_raw(&msg, "(3:eof");
		hull_msg_channel(&msg, st->ch);
		hull_msg_raw(&msg, ")");
		send_msg(m, &msg);
	}
	return HULL_OK;
}

static hull_status qemu_resize(hull_core *core, hull_stream *st, int rows, int cols,
                               hull_error *error)
{
	hull_msg msg = { 0 };

	(void)core;
	if (st == NULL)
		return fail(error, HULL_ERR_UNSUPPORTED, "a machine's workload has no terminal");
	if (!st->tty)
		return fail(error, HULL_ERR_UNSUPPORTED, "the command has no terminal");
	hull_msg_raw(&msg, "(5:winch");
	hull_msg_channel(&msg, st->ch);
	hull_msg_raw(&msg, "(4:cols");
	hull_msg_number(&msg, cols);
	hull_msg_raw(&msg, ")(4:rows");
	hull_msg_number(&msg, rows);
	hull_msg_raw(&msg, "))");
	send_msg(st->machine, &msg);
	return HULL_OK;
}

/* Ends the command, if it has not ended, and frees the stream; its exit
 * has been reported by then. */
static hull_status qemu_stream_close(hull_core *core, hull_stream *st, hull_error *error)
{
	hull_machine *m = st->machine;
	int           over;

	qemu_stream_eof(core, st, error);
	pthread_mutex_lock(&m->lock);
	over = st->exited || st->lost;
	pthread_mutex_unlock(&m->lock);
	if (!over)
		kill_stream(m, st);
	pthread_join(st->deliverer, NULL);
	drop_stream(m, st);
	free_stream(st);
	return HULL_OK;
}

/* --- ports --------------------------------------------------------------- */

typedef struct connection {
	hull_machine *machine;
	int           socket;
	unsigned      port;      /* the guest's */
} connection;

/* One connection to a port: a channel of the agent's, `connect`ed to the
 * guest's port. The client's bytes go in on the agent's credit, as an
 * exec's stdin does, and the machine's come back through deliver(). The
 * client's end of writing is the agent's eof, so a client may half-close;
 * a client that goes, or that resets, ends the connection. */
/* passt's figures, if `m->passt` is still a passt: it is not the
 * engine's child, so its pid may have been freed and taken by another
 * process once it has gone. */
static void add_passt(const hull_machine *m, hull_usage *usage)
{
	char  path[64];
	char  name[32] = "";
	FILE *f;

	if (m->passt <= 0)
		return;
	snprintf(path, sizeof path, "/proc/%ld/comm", (long)m->passt);
	if ((f = fopen(path, "re")) == NULL)
		return;
	if (fgets(name, sizeof name, f) != NULL && strncmp(name, "passt", 5u) == 0)
		hull_usage_add(m->passt, usage);
	fclose(f);
}

/* What the host spends on the machine: qemu, which holds the guest's
 * memory and runs its CPUs, and its passt. Read under the lock, so that
 * qemu's pid is still qemu's. */
static hull_status qemu_stats(hull_core *core, hull_machine *m, hull_buf *out, hull_error *error)
{
	hull_usage usage = { 0u, 0u, 0u };
	int        read_all;

	(void)core;
	pthread_mutex_lock(&m->lock);
	read_all = m->started && !m->done && hull_usage_add(m->qemu, &usage);
	pthread_mutex_unlock(&m->lock);
	if (!read_all)
		return fail(error, HULL_ERR_GONE, "the machine is not running");
	add_passt(m, &usage);
	out->data = m->stats;
	out->len = hull_usage_encode(&usage, m->stats, sizeof m->stats);
	return out->len == 0u ? fail(error, HULL_ERR_INTERNAL, "stats do not fit") : HULL_OK;
}

static void *forward_one(void *arg)
{
	connection   *k = arg;
	hull_machine *m = k->machine;
	hull_stream  *st;
	hull_status   why;
	hull_msg      msg = { 0 };
	char          reason[300];

	if ((st = new_stream(m, NULL, &why)) == NULL) {
		host->log(m->core, 2, "a connection to a port finds the machine out of channels");
		goto out;
	}
	st->forwarded = 1;
	st->sink      = k->socket;
	hull_msg_raw(&msg, "(7:connect");
	hull_msg_channel(&msg, st->ch);
	hull_msg_raw(&msg, "(4:port");
	hull_msg_number(&msg, (long long)k->port);
	hull_msg_raw(&msg, "))");
	send_msg(m, &msg);
	if (!answered(m, st, reason, sizeof reason)) {
		char line[400];

		snprintf(line, sizeof line, "a connection to port %u: %s", k->port, reason);
		host->log(m->core, 1, line);
		drop_stream(m, st);
		free_stream(st);
		goto out;
	}
	if (pthread_create(&st->deliverer, NULL, deliver, st) != 0) {
		drop_stream(m, st);
		kill_stream(m, st);
		free_stream(st);
		goto out;
	}
	for (;;) {
		uint8_t buffer[16384];
		ssize_t n = recv(k->socket, buffer, sizeof buffer, 0);

		if (n < 0 && errno == EINTR)
			continue;
		if (n > 0 && qemu_stream_write(NULL, st, buffer, (size_t)n, NULL) == HULL_OK)
			continue;
		if (n == 0)
			qemu_stream_eof(NULL, st, NULL);
		else
			kill_stream(m, st);
		break;
	}
	pthread_join(st->deliverer, NULL);
	drop_stream(m, st);
	free_stream(st);
out:
	close(k->socket);
	pthread_mutex_lock(&m->lock);
	m->connections--;
	pthread_cond_broadcast(&m->quiet);
	pthread_mutex_unlock(&m->lock);
	free(k);
	return NULL;
}

/* A client's datagram to a udp port: on its flow, a channel of the
 * agent's opened for its address, or on a new one. Sent without credit,
 * and dropped, as udp is, where the machine has no channel left. */
static void forward_datagram(hull_machine *m, unsigned port)
{
	static uint8_t     datagram[65536];
	struct sockaddr_in from;
	socklen_t          length = sizeof from;
	ssize_t            n = recvfrom(m->listeners[port], datagram, sizeof datagram, MSG_DONTWAIT,
	                                (struct sockaddr *)&from, &length);
	hull_stream       *st = NULL;
	hull_msg           msg = { 0 };
	struct timespec    now;
	hull_status        why;

	if (n < 0 || length != sizeof from || from.sin_family != AF_INET)
		return;
	clock_gettime(CLOCK_MONOTONIC, &now);
	pthread_mutex_lock(&m->lock);
	for (unsigned i = 0u; i < MAX_STREAMS && st == NULL; i++)
		if (m->streams[i] != NULL && m->streams[i]->datagram && !m->streams[i]->lost &&
		    m->streams[i]->listener == m->listeners[port] &&
		    m->streams[i]->peer.sin_port == from.sin_port &&
		    m->streams[i]->peer.sin_addr.s_addr == from.sin_addr.s_addr)
			st = m->streams[i];
	if (st != NULL)
		st->last = now.tv_sec;
	pthread_mutex_unlock(&m->lock);
	if (st == NULL) {
		if ((st = new_stream(m, NULL, &why)) == NULL) {
			host->log(m->core, 2, "a datagram to a port finds the machine out of channels");
			return;
		}
		pthread_mutex_lock(&m->lock);
		st->datagram = 1;
		st->listener = m->listeners[port];
		st->peer     = from;
		st->last     = now.tv_sec;
		pthread_mutex_unlock(&m->lock);
		hull_msg_raw(&msg, "(7:connect");
		hull_msg_channel(&msg, st->ch);
		hull_msg_raw(&msg, "(4:port");
		hull_msg_number(&msg, (long long)m->port_guest[port]);
		hull_msg_raw(&msg, ")(5:proto3:udp))");
		send_msg(m, &msg);
		msg = (hull_msg){ 0 };
	}
	hull_msg_raw(&msg, "(4:data(3:buf");
	hull_msg_atom(&msg, datagram, (size_t)n);
	hull_msg_raw(&msg, ")");
	hull_msg_channel(&msg, st->ch);
	hull_msg_raw(&msg, "(2:fd1:0))");
	send_msg(m, &msg);
}

/* The flows that have ended, been refused, or gone FLOW_IDLE seconds
 * unused, or every flow once the machine has ended: dropped, and the
 * agent told to close those it holds. Whether any is left. */
static int sweep_flows(hull_machine *m, int all)
{
	hull_stream    *ended[MAX_STREAMS];
	unsigned        count = 0u;
	int             left = 0;
	struct timespec now;

	clock_gettime(CLOCK_MONOTONIC, &now);
	pthread_mutex_lock(&m->lock);
	for (unsigned i = 0u; i < MAX_STREAMS; i++) {
		hull_stream *st = m->streams[i];

		if (st == NULL || !st->datagram)
			continue;
		if (all || st->lost || st->exited || now.tv_sec - st->last >= FLOW_IDLE) {
			ended[count++] = st;
			m->streams[i] = NULL;
		} else {
			left = 1;
		}
	}
	pthread_mutex_unlock(&m->lock);
	for (unsigned i = 0u; i < count; i++) {
		if (!ended[i]->lost && !ended[i]->exited)
			kill_stream(m, ended[i]);
		free_stream(ended[i]);
	}
	return left;
}

/* The machine's forwarder: each connection to a listener gets a thread of
 * its own, and each datagram goes on its client's flow, until the machine
 * ends and the reader wakes it. Flows are swept each second there are
 * any. */
static void *forward_ports(void *arg)
{
	hull_machine *m = arg;
	int           flows = 0;

	for (;;) {
		struct pollfd fds[1u + MAX_PORTS];

		fds[0] = (struct pollfd){ m->wake, POLLIN, 0 };
		for (unsigned i = 0u; i < m->port_count; i++)
			fds[1u + i] = (struct pollfd){ m->listeners[i], POLLIN, 0 };
		if (poll(fds, 1u + m->port_count, flows ? 1000 : -1) < 0 && errno != EINTR)
			break;
		if (fds[0].revents != 0)
			break;
		for (unsigned i = 0u; i < m->port_count; i++) {
			connection    *k;
			pthread_t      thread;
			pthread_attr_t detached;
			int            fd;

			if (fds[1u + i].revents != 0 && m->port_udp[i]) {
				forward_datagram(m, i);
				continue;
			}
			if (fds[1u + i].revents == 0 ||
			    (fd = accept4(m->listeners[i], NULL, NULL, SOCK_CLOEXEC)) < 0)
				continue;
			if ((k = malloc(sizeof *k)) == NULL) {
				close(fd);
				continue;
			}
			k->machine = m;
			k->socket  = fd;
			k->port    = m->port_guest[i];
			pthread_mutex_lock(&m->lock);
			m->connections++;
			pthread_mutex_unlock(&m->lock);
			pthread_attr_init(&detached);
			pthread_attr_setdetachstate(&detached, PTHREAD_CREATE_DETACHED);
			if (pthread_create(&thread, &detached, forward_one, k) != 0) {
				close(fd);
				free(k);
				pthread_mutex_lock(&m->lock);
				m->connections--;
				pthread_cond_broadcast(&m->quiet);
				pthread_mutex_unlock(&m->lock);
			}
			pthread_attr_destroy(&detached);
		}
		flows = sweep_flows(m, 0);
	}
	sweep_flows(m, 1);
	return NULL;
}

/* --- the declaration ----------------------------------------------------- */

static const hull_cap_entry entries[] = {
	/* The dir over 9p, written in place; or read-only beneath an
	 * overlayfs in the guest, whose upper layer is a tmpfs there rather
	 * than section 3.7's upper/ on the host. */
	{ "source.dir.direct",  HULL_SUP_NATIVE,   NULL, NULL },
	{ "source.dir.overlay", HULL_SUP_EMULATED,
	  "an overlayfs in the guest; writes are held in the machine's memory and go with it",
	  NULL },

	/* Applied by the guest agent. */
	{ "command",            HULL_SUP_NATIVE,   NULL, NULL },
	{ "workdir",            HULL_SUP_NATIVE,   NULL, NULL },
	{ "env",                HULL_SUP_NATIVE,   NULL, NULL },
	{ "hostname",           HULL_SUP_NATIVE,   NULL, NULL },
	{ "volumes",            HULL_SUP_NATIVE,   "a 9p share each", NULL },
	{ "cpus",               HULL_SUP_NATIVE,   NULL, NULL },
	{ "memory",             HULL_SUP_NATIVE,   "a hard allocation", NULL },
	/* Section 6's host forwarder: the engine's, over vsock to the agent. */
	{ "ports",              HULL_SUP_EMULATED,
	  "tcp and udp, forwarded over vsock from the machine's address", NULL },

	{ "security.readonly_root", HULL_SUP_NATIVE, NULL, NULL },
	{ "security.no_new_privs",  HULL_SUP_NATIVE, "set by the agent on the workload", NULL },

	/* 9p, like virtiofs, misses changes made on the host (6). */
	{ "fidelity.inotify",   HULL_SUP_REJECTED, "9p does not see changes made on the host",
	  "vm:inotify-poll" },

	{ "projection.passthrough", HULL_SUP_NATIVE, NULL, NULL },
	{ "writeback.none",         HULL_SUP_NATIVE, NULL, NULL },

	/* Kept while the core runs; qemu dies with it, as a chroot machine
	 * does, until adopt() is written. */
	{ "on_disconnect.stop", HULL_SUP_NATIVE,   NULL, NULL },
	{ "on_disconnect.keep", HULL_SUP_NATIVE,   NULL, NULL },

	/* J.5. A q35 machine on the host's own kernel stands in for the
	 * micro profile and Appendix F's bundled kernel. */
	{ "vm.profile.micro",    HULL_SUP_EMULATED,
	  "a q35 machine on the host's kernel, which boots in about two seconds", NULL },
	{ "vm.kernel.bundled",   HULL_SUP_EMULATED,
	  "the host's own kernel and modules, until Appendix F ships one", NULL },
	{ "vm.cpu_profile.host", HULL_SUP_NATIVE,  NULL, NULL },
	{ "qemu.accel.auto",     HULL_SUP_NATIVE,  NULL, NULL },
	{ "emu.cores_scale",     HULL_SUP_NATIVE,  NULL, NULL },

	{ NULL,                 HULL_SUP_NATIVE,   NULL, NULL },
};

static const char *const classes[] = { "vm", "emu", NULL };

static const hull_capabilities caps = {
	.struct_size    = sizeof (hull_capabilities),
	.engine_name    = "qemu",
	.engine_version = "0",
	.classes        = classes,
	.entries        = entries,
	.migration      = HULL_MIG_LIVE,  /* H.3 */
	.arches         = NULL,           /* the host's own, on the host's kernel */
};

static const hull_engine_ops ops = {
	.struct_size = sizeof (hull_engine_ops),
	.create      = qemu_create,
	.start       = qemu_start,
	.stop        = qemu_stop,
	.destroy     = qemu_destroy,
	.exec         = qemu_exec,
	.stream_write = qemu_stream_write,
	.stream_eof   = qemu_stream_eof,
	.stream_close = qemu_stream_close,
	.resize       = qemu_resize,
	.signal       = qemu_signal,
	.stats        = qemu_stats,
	.dial         = qemu_dial,
};

static const hull_engine engine = {
	.struct_size = sizeof (hull_engine),
	.abi_major   = HULL_ABI_MAJOR,
	.abi_minor   = HULL_ABI_MINOR,
	.caps        = &caps,
	.ops         = &ops,
};

const hull_engine *hull_engine_qemu(const hull_host_ops *host_ops)
{
	host = host_ops;
	return &engine;
}
