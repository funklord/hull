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
#include <grp.h>
#include <stddef.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
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
#include "stack.h"
#include "builtin.h"
#include "wire.h"
#include "idmap.h"

#define MAX_VOLUMES  64u
#define MAX_PORTS    64u
#define MAX_MODULES  32u
#define AGENT_PORT   1u
#define BOOT_LIMIT   60   /* seconds for the agent to answer, TCG included */
#define CID_TRIES    4
#define MAX_STREAMS  64u
#define WINDOW       65536u  /* what the agent gives, and is given, each way */
#define NETWORK_FD   3       /* passt's socket, in qemu */
#define SEGMENT_FD   4       /* the first segment's port, in qemu */
#define MAX_SEGMENTS 8u
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
	char            *layer_path[MAX_VOLUMES];  /* (layers ...): read-only, each above the last */
	unsigned         layer_count;
	char            *upper;        /* (upper PATH): the overlay's writes, kept */
	char            *upper_work;   /* overlayfs's work directory beside it, hidden */
	int              host_overlay; /* layers or an upper: the overlay is qemu's, the share it */
	char            *owners;       /* (owners PATH): a stored tree's owners, given back */
	int              many;         /* the user's subordinate range mapped, not one uid */
	char            *kernel_path;  /* (kernel "path:FILE"): bring your own, else the host's */
	int              kernel_image; /* (kernel image): the image's own /hull/kernel */
	char            *volume_host[MAX_VOLUMES];
	int              volume_readonly[MAX_VOLUMES];
	hull_stack       volume_stack[MAX_VOLUMES];  /* a volume of (layers ...), or count 0 */
	int              staged;     /* a volume is a stack: mounted host-side, as the root's */
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
	hull_msg         secrets;      /* `(secrets (NAME VALUE) ...)` for it, or empty */
	hull_msg         ssh;          /* `(ssh (host_key V)(authorized_keys V)(config V))`, or empty */
	uint8_t         *names;        /* the plan's `(names ...)`, for the agent (Appendix D), or NULL */
	size_t           names_length;

	char             dir[4096];    /* the machine's files: initrd, console */
	int              network;      /* qemu's end of passt's socket, or -1 */
	int              no_network;   /* (network none): no NIC, no passt */
	char            *segment[MAX_SEGMENTS];     /* Appendix D's, by name */
	int              segment_fd[MAX_SEGMENTS];  /* their ports, until qemu has them */
	unsigned         segment_count;
	pid_t            passt;        /* passt's daemon, by the socket it holds, or 0 */
	pid_t            qemu;
	int              keep;         /* (on_disconnect keep): qemu outlives the core */
	int              adopted;      /* taken back: qemu is not this process's child */
	int              pidfd;        /* qemu's, when adopted, else -1 */
	uint32_t         cid;          /* its vsock context id */
	uint8_t          token[32];    /* what a host after this one resumes with */
	char             persisted[4800];  /* what persist() last gave */
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
	pthread_cond_t   guest_cond;   /* the agent's stats reply arrived */
	uint32_t         guest_ch;     /* the channel a guest-stats request waits on, 0 if none */
	int              guest_ready;  /* the reply is in `guest_usage` */
	hull_usage       guest_usage;  /* the guest's own view, from its (sub (what stats)) */

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

/* The secrets' values, once they are in the spec, or the machine goes:
 * zeroed first, as the spec that held them is once sent. The wire's own
 * frame and the allocator's earlier copies are not. */
static void forget_secrets(hull_machine *m)
{
	if (m->secrets.data != NULL)
		explicit_bzero(m->secrets.data, m->secrets.cap);
	hull_msg_free(&m->secrets);
}

/* The ssh material (the host key is private), on the same terms as secrets:
 * zeroed once in the spec or once the machine goes. */
static void forget_ssh(hull_machine *m)
{
	if (m->ssh.data != NULL)
		explicit_bzero(m->ssh.data, m->ssh.cap);
	hull_msg_free(&m->ssh);
}

/*
 * (ssh true)'s material, for the agent to place in the guest: read from the
 * three files the core wrote in the plan's `(ssh_dir DIR)` -- host_key,
 * authorized_keys and config -- into `(ssh (host_key V)(authorized_keys
 * V)(config V))`, which goes after the machine in the spec. A machine with
 * no `(ssh true)` has no `(ssh_dir ...)` and this is a no-op. Returns a
 * reason, or NULL.
 */
static const char *read_ssh(hull_machine *m, const hull_sexpr_doc *doc)
{
	static const char *const names[] = { "host_key", "authorized_keys", "config", NULL };
	static uint8_t           value[65537];
	hull_sexpr_form          top;
	char                    *dir;
	const char              *reason = NULL;

	hull_sexpr_first(doc, &top);
	if ((dir = hull_access_value(doc, &top, "ssh_dir")) == NULL)
		return NULL;
	hull_msg_raw(&m->ssh, "(3:ssh");
	for (unsigned i = 0u; names[i] != NULL && reason == NULL; i++) {
		char    path[4400];
		size_t  n = 0u;
		ssize_t got = 0;
		int     fd = -1;

		if ((size_t)snprintf(path, sizeof path, "%s/%s", dir, names[i]) < sizeof path)
			fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
		while (fd >= 0 && n < sizeof value && (got = read(fd, value + n, sizeof value - n)) > 0)
			n += (size_t)got;
		if (fd < 0 || got < 0 || n == sizeof value) {
			reason = "an ssh material file cannot be read";
		} else {
			hull_msg_raw(&m->ssh, "(");
			hull_msg_text(&m->ssh, names[i]);
			hull_msg_atom(&m->ssh, value, n);
			hull_msg_raw(&m->ssh, ")");
		}
		explicit_bzero(value, n);
		if (fd >= 0)
			close(fd);
	}
	hull_msg_raw(&m->ssh, ")");
	free(dir);
	if (reason == NULL && m->ssh.failed)
		reason = "out of memory";
	return reason;
}

/*
 * J.3's secrets, for the agent to place in the guest: read from the files
 * the core wrote in the plan's `(secrets DIR)`, by the names the plan's
 * machine gives, into `(secrets (NAME VALUE) ...)`, which goes after the
 * machine in the spec. Returns a reason, or NULL.
 */
static const char *read_secrets(hull_machine *m, const hull_sexpr_doc *doc,
                                const hull_sexpr_form *machine)
{
	static uint8_t  value[65537];
	hull_sexpr_form top;
	hull_sexpr_form entry;
	hull_sexpr_form item;
	hull_sexpr_form name_form;
	char           *dir;
	int             more;
	const char     *reason = NULL;

	if (!hull_access_find(doc, machine, "secrets", &entry))
		return NULL;
	hull_sexpr_first(doc, &top);
	if ((dir = hull_access_value(doc, &top, "secrets")) == NULL)
		return "the plan's secrets were given no directory";
	hull_msg_raw(&m->secrets, "(7:secrets");
	hull_sexpr_child(doc, &entry, &item);
	for (more = hull_sexpr_next(doc, &item, &item); more && reason == NULL;
	     more = hull_sexpr_next(doc, &item, &item)) {
		char    path[4400];
		char   *name = hull_sexpr_child(doc, &item, &name_form) ?
		               hull_access_text(doc, &name_form) : NULL;
		size_t  n = 0u;
		ssize_t got = 0;
		int     fd = -1;

		if (name != NULL && (size_t)snprintf(path, sizeof path, "%s/%s", dir, name) < sizeof path)
			fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
		while (fd >= 0 && n < sizeof value && (got = read(fd, value + n, sizeof value - n)) > 0)
			n += (size_t)got;
		if (fd < 0 || got < 0 || n == sizeof value) {
			reason = "a secret cannot be read";
		} else {
			hull_msg_raw(&m->secrets, "(");
			hull_msg_text(&m->secrets, name);
			hull_msg_atom(&m->secrets, value, n);
			hull_msg_raw(&m->secrets, ")");
		}
		explicit_bzero(value, n);
		if (fd >= 0)
			close(fd);
		free(name);
	}
	hull_msg_raw(&m->secrets, ")");
	free(dir);
	if (reason == NULL && m->secrets.failed)
		reason = "out of memory";
	return reason;
}

static void release(hull_machine *m)
{
	free(m->root);
	for (unsigned i = 0u; i < m->volume_count; i++)
		free(m->volume_host[i]);
	for (unsigned i = 0u; i < m->volume_count; i++)
		hull_stack_release(&m->volume_stack[i]);
	for (unsigned i = 0u; i < m->segment_count; i++)
		free(m->segment[i]);
	for (unsigned i = 0u; i < m->layer_count; i++)
		free(m->layer_path[i]);
	free(m->upper);
	free(m->upper_work);
	free(m->owners);
	free(m->kernel_path);
	free(m->spec);
	free(m->names);
	forget_secrets(m);
	forget_ssh(m);
	if (m->pidfd >= 0)
		close(m->pidfd);
	pthread_mutex_destroy(&m->lock);
	pthread_mutex_destroy(&m->send_lock);
	pthread_cond_destroy(&m->done_cond);
	pthread_cond_destroy(&m->quiet);
	pthread_cond_destroy(&m->guest_cond);
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

	/* (layers ...) and (upper PATH): the overlay mounted host-side, in the
	 * user and mount namespace qemu runs in, and the merged tree the
	 * share, since the guest's overlayfs cannot take a 9p share as its
	 * upper (section 3.7). */
	if (hull_access_find(doc, &machine, "layers", &entry)) {
		hull_sexpr_child(doc, &entry, &item);
		for (more = hull_sexpr_next(doc, &item, &item); more;
		     more = hull_sexpr_next(doc, &item, &item)) {
			char *layer = hull_access_text(doc, &item);

			if (m->layer_count == MAX_VOLUMES || layer == NULL ||
			    strncmp(layer, "dir:", 4u) != 0) {
				free(layer);
				return "a layer is a dir: source, and there are at most 64";
			}
			m->layer_path[m->layer_count] = hull_access_path(doc, layer + 4);
			free(layer);
			if (m->layer_path[m->layer_count++] == NULL)
				return "a layer's path cannot be read";
		}
	}
	if ((text = hull_access_value(doc, &machine, "upper")) != NULL) {
		const char *slash;

		m->upper = hull_access_path(doc, text);
		free(text);
		if (m->upper == NULL || (m->upper_work = malloc(strlen(m->upper) + 16u)) == NULL)
			return "the upper's path cannot be read";
		slash = strrchr(m->upper, '/');
		if (slash == NULL)
			sprintf(m->upper_work, ".%s.hull-work", m->upper);
		else
			sprintf(m->upper_work, "%.*s/.%s.hull-work", (int)(slash - m->upper), m->upper,
			        slash + 1);
	}
	m->host_overlay = m->layer_count != 0u || m->upper != NULL;
	if (m->host_overlay && (!m->overlay || m->readonly_root))
		return "layers and an upper lie in an overlay: they need (mutable overlay) and a root "
		       "that is not read-only";

	/* (owners PATH): a stored tree's owners, given back in the merged tree
	 * host-side before qemu starts, which copies each file so given up
	 * into the overlay's tmpfs. A share in place, or the guest's own
	 * overlay over a read-only share, has no host-side tree to chown: so
	 * owners need the overlay qemu mounts, layers or an upper (section
	 * 4.11). The range those ids need is mapped from outside (below). */
	if ((text = hull_access_value(doc, &machine, "owners")) != NULL) {
		m->owners = hull_access_path(doc, text);
		free(text);
		if (m->owners == NULL)
			return "the owners' path cannot be read";
		if (!m->overlay || m->readonly_root || m->upper != NULL)
			return "owners are given in the overlay's tmpfs: (owners ...) needs (mutable "
			       "overlay), a root that is not read-only, and no kept (upper ...), where an "
			       "owner would be one on the user's disk";
		/* Chowned in the overlay qemu mounts host-side, so the overlay is
		 * host-side whenever owners are given, as it is with layers or an
		 * upper: a share in place, or the guest's own overlay over a
		 * read-only share, has no host tree to chown. */
		m->host_overlay = 1;
	}

	/* (tuning (vm (kernel ...))): the kernel the vm boots, in place of the
	 * host's. vm's tuning applies under emu too (section 4), so the (vm ...)
	 * section carries it for both classes. "path:FILE" is a bring-your-own
	 * kernel on the user's disk; "image" is the image's own /hull/kernel
	 * (Appendix F), read at boot since the source is known only here where
	 * m->root is; "bundled" is the host's (below) and names no kernel. */
	if (hull_access_find(doc, &machine, "tuning", &entry) &&
	    hull_access_find(doc, &entry, "vm", &item) &&
	    (text = hull_access_value(doc, &item, "kernel")) != NULL) {
		if (strncmp(text, "path:", 5u) == 0 && text[5] != '\0') {
			m->kernel_path = hull_access_path(doc, text + 5);
			free(text);
			if (m->kernel_path == NULL)
				return "the kernel's path cannot be read";
		} else {
			m->kernel_image = strcmp(text, "image") == 0;
			free(text);
		}
	}

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
			char       *host_path = hull_access_value(doc, &item, "host");
			char       *mode      = hull_access_value(doc, &item, "mode");
			const char *why;

			if (m->volume_count == MAX_VOLUMES) {
				free(host_path);
				free(mode);
				return "there are too many volumes";
			}
			unsigned    i = m->volume_count++;

			m->volume_readonly[i] = mode != NULL && strcmp(mode, "ro") == 0;
			free(mode);
			if ((why = hull_access_stack(doc, &item, m->volume_readonly[i],
			                             &m->volume_stack[i])) != NULL) {
				free(host_path);
				return why;
			}
			/* A stack is shared from where it is staged, in the machine's
			 * directory, which spawn() names. */
			if (m->volume_stack[i].count > 0u) {
				m->staged = 1;
				free(host_path);
				continue;
			}
			m->volume_host[i] = host_path != NULL ? hull_access_path(doc, host_path) : NULL;
			free(host_path);
			if (m->volume_host[i] == NULL)
				return "a volume is missing its host path";
		}
	}

	/* (network none) is a machine with no NIC, lo alone in the guest; a
	 * vm shares no host network to give (network host). */
	text = hull_access_value(doc, &machine, "network");
	m->no_network = text != NULL && strcmp(text, "none") == 0;
	free(text);
	if (m->no_network && hull_access_find(doc, &machine, "ports", &entry))
		return "ports need the machine's network, which (network none) has not";

	/* A NIC each, after passt's, on the core's switch. */
	if (hull_access_find(doc, &machine, "segments", &entry)) {
		hull_sexpr_child(doc, &entry, &item);
		for (more = hull_sexpr_next(doc, &item, &item); more;
		     more = hull_sexpr_next(doc, &item, &item)) {
			if (m->segment_count == MAX_SEGMENTS)
				return "there are too many segments";
			if ((m->segment[m->segment_count++] = hull_access_segment_name(doc, &item)) == NULL)
				return "a segment's name is not text";
		}
	}

	/* The machine's address, which the core gives every plan: its ports
	 * are on it, and its segment NICs' MACs are drawn from it. */
	text = hull_access_value(doc, &top, "address");
	if (text != NULL && strlen(text) < sizeof m->address_text &&
	    inet_pton(AF_INET, text, &m->address) == 1)
		snprintf(m->address_text, sizeof m->address_text, "%s", text);
	free(text);
	if (m->segment_count != 0u && m->address_text[0] == '\0')
		return "the plan gives its segments no address to draw a MAC from";
	if (hull_access_find(doc, &machine, "ports", &entry)) {
		if (m->address_text[0] == '\0')
			return "the plan gives its ports no address";
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

	{
		char *disconnect = hull_access_value(doc, &machine, "on_disconnect");

		m->keep = disconnect != NULL && strcmp(disconnect, "keep") == 0;
		free(disconnect);
	}
	/* A segment is the core's switch, which goes with the core. */
	if (m->keep && m->segment_count != 0u)
		return "a machine on a segment cannot outlive the core, whose switch the segment is: "
		       "(on_disconnect keep) and (segments ...) do not go together";

	/* The spec the agent applies is the plan's own (machine ...). */
	m->spec_length = machine.extent;
	m->spec = malloc(m->spec_length);
	if (m->spec == NULL)
		return "out of memory";
	memcpy(m->spec, doc->data + machine.offset, m->spec_length);
	/* The segments hull addresses, and their names, which the agent
	 * places in the guest as the engine is given them. */
	if (hull_access_find(doc, &top, "names", &entry)) {
		m->names_length = entry.extent;
		if ((m->names = malloc(m->names_length)) == NULL)
			return "out of memory";
		memcpy(m->names, doc->data + entry.offset, m->names_length);
	}
	{
		const char *reason = read_secrets(m, doc, &machine);

		if (reason != NULL)
			return reason;
	}
	return read_ssh(m, doc);
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

/* The release a bzImage carries, for finding its modules on the host. The
 * boot protocol keeps a pointer to the version string at offset 0x20E, into
 * the setup area at that pointer plus 0x200, a NUL-terminated
 * "<release> (builder) #n ..." whose first token is the release. Returns 0
 * where the file carries none, in which case the kernel is taken to have its
 * guest drivers built in (Appendix F) and no modules are loaded. */
static int kernel_release(const char *image, char *out, size_t cap)
{
	uint8_t  head[0x210];
	FILE    *file = fopen(image, "rb");
	unsigned ptr;
	char     line[256];
	size_t   i;

	if (file == NULL)
		return 0;
	if (fread(head, 1u, sizeof head, file) != sizeof head) {
		fclose(file);
		return 0;
	}
	ptr = (unsigned)head[0x20E] | ((unsigned)head[0x20F] << 8);
	if (ptr == 0u || fseek(file, (long)ptr + 0x200, SEEK_SET) != 0 ||
	    fgets(line, sizeof line, file) == NULL) {
		fclose(file);
		return 0;
	}
	fclose(file);
	for (i = 0u; line[i] != '\0' && line[i] != ' ' && line[i] != '\n' && i + 1u < cap; i++)
		out[i] = line[i];
	out[i] = '\0';
	return i > 0u;
}

static const char *write_initramfs(const char *dir, const char *release, int load_modules)
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
	/* A path kernel whose modules the host does not have is taken to have
	 * its guest drivers built in (Appendix F): no module list, no share. */
	if (load_modules && (reason = find_modules(release, &list)) != NULL) {
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

/*
 * passt's daemon: the process holding the socket's other end as its fd 3,
 * which it was given and keeps. Found in /proc, not from a pid file,
 * which passt cannot write in the runtime directory: its AppArmor profile,
 * Debian's, lets it write only under /tmp and the home directory, so with
 * $XDG_RUNTIME_DIR at /run/user/UID no machine started. 0 if none is
 * seen, which costs only its share of `hull stats`.
 */
static pid_t holder_of(ino_t inode)
{
	char           want[48];
	DIR           *proc = opendir("/proc");
	struct dirent *entry;
	pid_t          found = 0;

	snprintf(want, sizeof want, "socket:[%lu]", (unsigned long)inode);
	while (proc != NULL && found == 0 && (entry = readdir(proc)) != NULL) {
		char    path[64];
		char    link[48];
		ssize_t n;

		if (entry->d_name[0] < '1' || entry->d_name[0] > '9')
			continue;
		snprintf(path, sizeof path, "/proc/%.20s/fd/3", entry->d_name);
		n = readlink(path, link, sizeof link - 1u);
		if (n > 0) {
			link[n] = '\0';
			if (strcmp(link, want) == 0)
				found = (pid_t)strtol(entry->d_name, NULL, 10);
		}
	}
	if (proc != NULL)
		closedir(proc);
	return found;
}

static const char *start_network(hull_machine *m)
{
	char        log[4200];
	const char *argv[32];
	struct stat socket_end;
	unsigned    n = 0u;
	int         pair[2];
	int         status;
	pid_t       pid;

	if (m->no_network) {
		m->network = -1;
		m->passt = 0;
		return NULL;
	}

	snprintf(log, sizeof log, "%s/passt.log", m->dir);
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
	if (fstat(pair[1], &socket_end) != 0)
		socket_end.st_ino = 0;
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
	m->passt   = socket_end.st_ino != 0 ? holder_of(socket_end.st_ino) : 0;
	return NULL;
}

/* A port on each of the machine's segments, from the core's switch, for
 * one qemu: each attempt's qemu has its own, and the port goes with the
 * qemu that held it. A reason, or NULL. */
static const char *open_segments(hull_machine *m)
{
	for (unsigned i = 0u; i < m->segment_count; i++) {
		if (host->struct_size < offsetof(hull_host_ops, segment) + sizeof host->segment ||
		    host->segment == NULL)
			return "the core has no switch for a segment";
		m->segment_fd[i] = host->segment(m->core, m->segment[i], m->detail, sizeof m->detail);
		if (m->segment_fd[i] < 0)
			return m->detail;
	}
	return NULL;
}

/* This process's ends of them, once qemu has its own. */
static void close_segments(hull_machine *m)
{
	for (unsigned i = 0u; i < MAX_SEGMENTS; i++) {
		if (m->segment_fd[i] >= 0)
			close(m->segment_fd[i]);
		m->segment_fd[i] = -1;
	}
}

/* A segment NIC's address, as hull_access_segment_mac() draws it. */
static void segment_mac(const hull_machine *m, unsigned i, char *out, size_t cap)
{
	unsigned char mac[6];

	hull_access_segment_mac(m->address_text, m->segment[i], i, mac);
	snprintf(out, cap, "%02x:%02x:%02x:%02x:%02x:%02x", mac[0], mac[1], mac[2], mac[3], mac[4],
	         mac[5]);
}

/*
 * In qemu's child, in its own user and mount namespace: the overlay of the
 * machine's directory, its layers, each above the last, and its upper, or
 * a tmpfs where it keeps none, mounted at merged/ in its runtime
 * directory, as the chroot engine mounts its own (engine/chroot.c). The
 * upper's directory is cloned once with the work directory beside it,
 * which overlayfs wants on one mount, and that work directory is made the
 * user's own group and not setgid, overlayfs refusing one of a group the
 * namespace does not map. The step that failed, or NULL.
 */
static const char *stage_overlay(const hull_machine *m)
{
	char   options[8800u + 16u * MAX_VOLUMES];
	char   upper_dir[4200] = "mem/upper";
	char   work_dir[4200] = "mem/work";
	size_t at;
	int    tree;

	if (chdir(m->dir) != 0 || mkdir("lower", 0700) != 0 || mkdir("merged", 0700) != 0)
		return "its staging";
	if ((tree = open_tree(AT_FDCWD, m->root, OPEN_TREE_CLONE | OPEN_TREE_CLOEXEC)) < 0 ||
	    move_mount(tree, "", AT_FDCWD, "lower", MOVE_MOUNT_F_EMPTY_PATH) != 0)
		return "the lower layer";
	close(tree);
	for (unsigned i = 0u; i < m->layer_count; i++) {
		char name[32];

		snprintf(name, sizeof name, "lower%u", i + 1u);
		if (mkdir(name, 0700) != 0 ||
		    (tree = open_tree(AT_FDCWD, m->layer_path[i], OPEN_TREE_CLONE | OPEN_TREE_CLOEXEC)) < 0 ||
		    move_mount(tree, "", AT_FDCWD, name, MOVE_MOUNT_F_EMPTY_PATH) != 0)
			return "a layer";
		close(tree);
	}
	if (m->upper != NULL) {
		char        parent[4200];
		const char *slash = strrchr(m->upper, '/');
		const char *work = strrchr(m->upper_work, '/');

		snprintf(parent, sizeof parent, "%.*s", slash != NULL && slash != m->upper ?
		         (int)(slash - m->upper) : 1, slash != NULL ? m->upper : ".");
		snprintf(upper_dir, sizeof upper_dir, "kept/%s", slash != NULL ? slash + 1 : m->upper);
		snprintf(work_dir, sizeof work_dir, "kept/%s", work != NULL ? work + 1 : m->upper_work);
		if (strpbrk(upper_dir, ",:") != NULL || (mkdir(m->upper, 0755) != 0 && errno != EEXIST) ||
		    (mkdir(m->upper_work, 0700) != 0 && errno != EEXIST) ||
		    chown(m->upper_work, (uid_t)-1, 0) != 0 || chmod(m->upper_work, 0700) != 0 ||
		    mkdir("kept", 0700) != 0 ||
		    (tree = open_tree(AT_FDCWD, parent, OPEN_TREE_CLONE | OPEN_TREE_CLOEXEC)) < 0 ||
		    move_mount(tree, "", AT_FDCWD, "kept", MOVE_MOUNT_F_EMPTY_PATH) != 0)
			return "the upper, kept";
		close(tree);
	} else if (mkdir("mem", 0700) != 0 || mount("tmpfs", "mem", "tmpfs", 0u, "mode=0700") != 0 ||
	           mkdir("mem/upper", 0755) != 0 || mkdir("mem/work", 0700) != 0) {
		return "the upper, in memory";
	}
	at = (size_t)snprintf(options, sizeof options, "lowerdir=");
	for (unsigned i = m->layer_count; i > 0u; i--)
		at += (size_t)snprintf(options + at, sizeof options - at, "lower%u:", i);
	snprintf(options + at, sizeof options - at, "lower,upperdir=%s,workdir=%s,userxattr",
	         upper_dir, work_dir);
	if (mount("overlay", "merged", "overlay", 0u, options) != 0)
		return "mounting it";
	return NULL;
}

/* `format` with `id`, written whole to `path`, as a child between fork and
 * exec may: no allocation, no stdio. */
static int write_map(const char *path, const char *format, unsigned id)
{
	char    text[64];
	int     n = snprintf(text, sizeof text, format, id);
	int     fd = open(path, O_WRONLY | O_CLOEXEC);
	ssize_t written;

	if (fd < 0 || n < 0)
		return 0;
	written = write(fd, text, (size_t)n);
	close(fd);
	return written == (ssize_t)n;
}

/* Fork qemu for `m`, with `cid` for its vsock device. Its stdout and
 * stderr go to qemu.log. */
static pid_t spawn(hull_machine *m, const char *kernel, const char *release, int load_modules,
                   uint32_t cid)
{
	uid_t       uid = getuid();   /* read here: unmapped once unshared */
	gid_t       gid = getgid();
	int         ready[2];         /* the child says its namespace is there */
	int         decide[2];        /* the parent says how its ids were mapped */
	char        initrd[4200];
	char        console[8600];
	char        log[4200];
	char        memory[32];
	char        cpus[16];
	char        vsock[64];
	char        netdev[64];
	char        root[9000];
	char        modules[9000];
	char        modules_at[4200];
	char        segment_netdev[MAX_SEGMENTS][64];
	char        segment_device[MAX_SEGMENTS][96];
	char        fsdev[MAX_VOLUMES][9000];
	char        device[MAX_VOLUMES][96];
	char        path[8500];
	const char *argv[64 + 4u * MAX_VOLUMES + 4u * MAX_SEGMENTS];
	unsigned    n = 0u;
	pid_t       pid;

	snprintf(initrd, sizeof initrd, "%s/initrd", m->dir);
	snprintf(log, sizeof log, "%s/qemu.log", m->dir);
	escaped(path, sizeof path, m->dir);
	snprintf(console, sizeof console, "file,id=console,path=%s/console.log", path);
	snprintf(memory, sizeof memory, "%lu", m->memory_mib);
	snprintf(cpus, sizeof cpus, "%u", m->cpus);
	snprintf(vsock, sizeof vsock, "vhost-vsock-pci,guest-cid=%u", cid);
	snprintf(netdev, sizeof netdev, "stream,id=net,addr.type=fd,addr.str=%d", NETWORK_FD);
	/* The share is the directory, or with layers, an upper or owners the
	 * overlay qemu's child mounts in its runtime directory, its merged tree.
	 * It is read-only under (mutable overlay) whenever the guest lays its
	 * own overlay over it (a tmpfs upper, fast writes in its memory), which
	 * is every case but a kept (upper PATH): there the guest writes through
	 * the share to the host-side overlay's kept upper, so it is read-write. */
	if (m->host_overlay) {
		char merged[4200];

		snprintf(merged, sizeof merged, "%s/merged", m->dir);
		escaped(path, sizeof path, merged);
	} else {
		escaped(path, sizeof path, m->root);
	}
	snprintf(root, sizeof root, "local,id=root,path=%s,security_model=none%s", path,
	         m->readonly_root || (m->overlay && m->upper == NULL) ? ",readonly=on" : "");

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
	/* hull.passt says eth0 is passt's, so the agent gives it passt's
	 * address: under (network none) eth0 is a segment's (Appendix D). */
	argv[n++] = m->no_network ? "console=hvc0 quiet panic=-1" :
	                            "console=hvc0 quiet panic=-1 hull.passt=1";
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
		char staged[4300];

		snprintf(staged, sizeof staged, "%s/v%u/merged", m->dir, i);
		escaped(path, sizeof path, m->volume_stack[i].count > 0u ? staged : m->volume_host[i]);
		snprintf(fsdev[i], sizeof fsdev[i], "local,id=v%u,path=%s,security_model=none%s", i,
		         path, m->volume_readonly[i] ? ",readonly=on" : "");
		snprintf(device[i], sizeof device[i], "virtio-9p-pci,fsdev=v%u,mount_tag=v%u", i, i);
		argv[n++] = "-fsdev";
		argv[n++] = fsdev[i];
		argv[n++] = "-device";
		argv[n++] = device[i];
	}
	/* The guest runs this release's kernel, so its modules are that
	 * release's on the host: shared read-only, for the agent to mount where
	 * modprobe in the machine looks, as a test of network software loads
	 * mac80211_hwsim or pppoe. A path kernel the host has no modules for is
	 * taken to have its drivers built in, and gets no share. */
	if (load_modules) {
		snprintf(path, sizeof path, "/lib/modules/%s", release);
		escaped(modules_at, sizeof modules_at, path);
		snprintf(modules, sizeof modules,
		         "local,id=modules,path=%s,security_model=none,readonly=on", modules_at);
		argv[n++] = "-fsdev";
		argv[n++] = modules;
		argv[n++] = "-device";
		argv[n++] = "virtio-9p-pci,fsdev=modules,mount_tag=modules";
	}
	argv[n++] = "-device";
	argv[n++] = vsock;
	if (!m->no_network) {
		argv[n++] = "-netdev";
		argv[n++] = netdev;
		argv[n++] = "-device";
		argv[n++] = "virtio-net-pci,netdev=net";
	}
	/* After passt's, so that eth0 is its, and each segment the next. */
	for (unsigned i = 0u; i < m->segment_count; i++) {
		char mac[24];

		segment_mac(m, i, mac, sizeof mac);
		snprintf(segment_netdev[i], sizeof segment_netdev[i],
		         "stream,id=s%u,addr.type=fd,addr.str=%u", i, SEGMENT_FD + i);
		snprintf(segment_device[i], sizeof segment_device[i], "virtio-net-pci,netdev=s%u,mac=%s",
		         i, mac);
		argv[n++] = "-netdev";
		argv[n++] = segment_netdev[i];
		argv[n++] = "-device";
		argv[n++] = segment_device[i];
	}
	argv[n] = NULL;

	/* The parent maps the user's subordinate range into the child's user
	 * namespace from outside, by newuidmap, where a stored tree's owners
	 * are to be given back: so the child unshares, says so over `ready`,
	 * and waits on `decide` for the word on its ids. */
	if (pipe2(ready, O_CLOEXEC) != 0 || pipe2(decide, O_CLOEXEC) != 0) {
		fprintf(stderr, "qemu-system-x86_64: pipe: %s\n", strerror(errno));
		return -1;
	}

	pid = fork();
	if (pid == 0) {
		int null = open("/dev/null", O_RDWR);
		int out  = open(log, O_WRONLY | O_CREAT | O_TRUNC, 0600);

		/* Dead with the core, unless kept for the next (adopt()). */
		if (!m->keep)
			prctl(PR_SET_PDEATHSIG, SIGKILL);
		setsid();
		if (null < 0 || out < 0)
			_exit(126);
		dup2(null, 0);
		dup2(out, 1);
		dup2(out, 2);
		/* qemu in a user namespace where the user is root, as a chroot
		 * machine is: its 9p server then shows the guest the user's files
		 * as root's. Outside one it showed them as the user's own uid, so
		 * a setuid-root program was setuid to the user in the guest, and
		 * root running mount, su or sudo ran them as the user, which
		 * systemd's mount units met first. Its devices are opened by the
		 * user's own rights, groups included, which a user namespace
		 * keeps; passt is outside it, by the descriptor below.
		 *
		 * The maps come before the fd work, so the namespace is there for
		 * the parent to map into while the child waits. Where a stored
		 * tree's owners are to be given back, the parent maps the user's
		 * subordinate range beside root from outside, by newuidmap, so a
		 * file given to another id is that id in the guest and not
		 * nobody; `M` then, the range already mapped, drops only the
		 * user's own groups, unmapped here; `S`, no owners, map the one
		 * uid here, as a process may itself. */
		if (unshare(CLONE_NEWUSER | (m->host_overlay || m->staged ? CLONE_NEWNS : 0)) != 0) {
			fprintf(stderr, "qemu-system-x86_64: a user namespace: %s\n", strerror(errno));
			_exit(126);
		}
		{
			char said = 0;

			close(ready[0]);
			close(decide[1]);
			if (write(ready[1], "r", 1u) != 1 || read(decide[0], &said, 1u) != 1) {
				fprintf(stderr, "qemu-system-x86_64: the uid map: %s\n", strerror(errno));
				_exit(126);
			}
			close(ready[1]);
			close(decide[0]);
			if (said == 'M') {
				if (setgroups(0u, NULL) != 0) {
					fprintf(stderr, "qemu-system-x86_64: setgroups: %s\n", strerror(errno));
					_exit(126);
				}
			} else if (!write_map("/proc/self/setgroups", "deny", 0u) ||
			           !write_map("/proc/self/uid_map", "0 %u 1", (unsigned)uid) ||
			           !write_map("/proc/self/gid_map", "0 %u 1", (unsigned)gid)) {
				fprintf(stderr, "qemu-system-x86_64: a user namespace: %s\n", strerror(errno));
				_exit(126);
			}
		}
		/* passt's socket at NETWORK_FD, each segment's port after it, and
		 * nothing of the core's: its sockets, its lock, the map pipes. Each
		 * is moved above them all first, so that none is closed by
		 * another's dup2; a dup carries no CLOEXEC. */
		{
			int moved_network = -1;
			int moved[MAX_SEGMENTS];

			if (m->network >= 0 && (moved_network = fcntl(m->network, F_DUPFD, 64)) < 0)
				_exit(126);
			for (unsigned i = 0u; i < m->segment_count; i++)
				if ((moved[i] = fcntl(m->segment_fd[i], F_DUPFD, 64)) < 0)
					_exit(126);
			if (moved_network >= 0 ? dup2(moved_network, NETWORK_FD) < 0 :
			                         close(NETWORK_FD) != 0 && errno != EBADF)
				_exit(126);  /* (network none): no passt */
			for (unsigned i = 0u; i < m->segment_count; i++)
				if (dup2(moved[i], (int)(SEGMENT_FD + i)) < 0)
					_exit(126);
			syscall(SYS_close_range, SEGMENT_FD + m->segment_count, ~0u, 0u);
		}
		if ((m->host_overlay || m->staged) &&
		    mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL) != 0) {
			fprintf(stderr, "qemu-system-x86_64: making mounts private: %s\n", strerror(errno));
			_exit(126);
		}
		if (m->host_overlay) {
			const char *failed = stage_overlay(m);

			if (failed != NULL) {
				fprintf(stderr, "qemu-system-x86_64: the overlay: %s: %s\n", failed,
				        strerror(errno));
				_exit(126);
			}
			/* stage_overlay left the cwd the machine's directory, with
			 * the overlay at merged/: a stored tree's owners are given
			 * back there before qemu starts, which copies each file so
			 * given up into the overlay's tmpfs, as on chroot. The 9p
			 * share then shows the guest each as its owner, where the
			 * range above holds the id, and nobody where it does not. */
			if (m->owners != NULL) {
				int list = open(m->owners, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);

				if (list < 0) {
					fprintf(stderr, "qemu-system-x86_64: the owners' file: %s\n",
					        strerror(errno));
					_exit(126);
				}
				hull_give_owners(list, "merged/", 2);
			}
		}
		/* Each volume that is a stack, staged as the root's overlay is,
		 * in v0, v1, ... of the machine's directory. */
		for (unsigned i = 0u; i < m->volume_count; i++) {
			char        at[4300];
			const char *failed = NULL;

			if (m->volume_stack[i].count == 0u)
				continue;
			snprintf(at, sizeof at, "%s/v%u", m->dir, i);
			if (mkdir(at, 0700) != 0)
				failed = "its staging";
			if (failed == NULL)
				failed = hull_stack_stage(&m->volume_stack[i], at);
			if (failed != NULL) {
				fprintf(stderr, "qemu-system-x86_64: volume %u: %s: %s\n", i, failed,
				        strerror(errno));
				_exit(126);
			}
		}
		execvp(argv[0], (char *const *)argv);
		fprintf(stderr, "qemu-system-x86_64: %s\n", strerror(errno));
		_exit(127);
	}
	/* The child unshared its user namespace and waits: map the user's
	 * subordinate range into it from outside, where a stored tree's owners
	 * are to be given back and the host grants the user a range, so a chown
	 * inside to one of those ids works; else let the child map its one uid.
	 * A fork that failed, or a child gone before it said anything, leaves
	 * nothing to map and the launch fails below. */
	close(ready[1]);
	close(decide[0]);
	if (pid > 0) {
		char hello = 0;

		if (read(ready[0], &hello, 1u) == 1) {
			char said = m->owners != NULL && hull_map_many(pid, uid, gid) ? 'M' : 'S';

			m->many = said == 'M';
			if (write(decide[1], &said, 1u) != 1) {
				/* the child reads its end closed, and exits */
			}
		}
	}
	close(ready[0]);
	close(decide[1]);
	return pid;
}

/* Is qemu still there? Not reaped: the reader reaps it. */
static int qemu_gone(const hull_machine *m)
{
	siginfo_t info;

	if (m->adopted) {
		struct pollfd fd = { m->pidfd, POLLIN, 0 };

		return poll(&fd, 1u, 0) != 0;
	}
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

	/* An adopted qemu is init's child, to reap; its end is the pidfd's. */
	if (m->adopted) {
		struct pollfd fd = { m->pidfd, POLLIN, 0 };

		while (poll(&fd, 1u, -1) < 0 && errno == EINTR)
			;
		pthread_mutex_lock(&m->lock);
		m->done = 1;
		pthread_cond_broadcast(&m->done_cond);
		pthread_mutex_unlock(&m->lock);
		return;
	}
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
			int taken = 0;

			/* A guest-stats reply on the channel a guest_stats() waits
			 * on: capture its cpu, io and mem and wake the waiter. */
			pthread_mutex_lock(&m->lock);
			if (m->guest_ch != 0u && (uint32_t)number == m->guest_ch && hull_msg_is(&body, "stats")) {
				hull_sexpr_form f;
				long long       v;

				m->guest_usage.cpu = hull_msg_find(&body, "cpu", &f) &&
				                     hull_msg_long(&body, &f, &v) ? (unsigned long long)v : 0u;
				m->guest_usage.io = hull_msg_find(&body, "io", &f) &&
				                    hull_msg_long(&body, &f, &v) ? (unsigned long long)v : 0u;
				m->guest_usage.memory = hull_msg_find(&body, "mem", &f) &&
				                        hull_msg_long(&body, &f, &v) ? (unsigned long long)v : 0u;
				m->guest_ready = 1;
				pthread_cond_signal(&m->guest_cond);
				taken = 1;
			}
			pthread_mutex_unlock(&m->lock);
			if (!taken)
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
	m->pidfd = -1;
	for (unsigned i = 0u; i < MAX_PORTS; i++)
		m->listeners[i] = -1;
	for (unsigned i = 0u; i < MAX_SEGMENTS; i++)
		m->segment_fd[i] = -1;
	pthread_mutex_init(&m->lock, NULL);
	pthread_mutex_init(&m->send_lock, NULL);
	pthread_cond_init(&m->done_cond, NULL);
	pthread_cond_init(&m->quiet, NULL);
	pthread_cond_init(&m->guest_cond, NULL);
	if ((reason = read_plan(m, &doc)) != NULL) {
		release(m);
		return fail(error, HULL_ERR_UNSUPPORTED, reason);
	}
	*out = m;
	host->on_state(core, m, "created");
	return HULL_OK;
}

/* The directories of machines taken back, which adoption below explains. */
static pthread_mutex_t adopted_lock = PTHREAD_MUTEX_INITIALIZER;
static char            adopted_dirs[16][4096];

static int is_adopted_dir(const char *dir)
{
	int found = 0;

	pthread_mutex_lock(&adopted_lock);
	for (unsigned i = 0u; i < 16u && !found; i++)
		found = strcmp(adopted_dirs[i], dir) == 0;
	pthread_mutex_unlock(&adopted_lock);
	return found;
}

static void mark_adopted_dir(const char *dir, int on)
{
	pthread_mutex_lock(&adopted_lock);
	for (unsigned i = 0u; i < 16u; i++) {
		if (on && adopted_dirs[i][0] == '\0') {
			snprintf(adopted_dirs[i], sizeof adopted_dirs[i], "%s", dir);
			break;
		}
		if (!on && strcmp(adopted_dirs[i], dir) == 0)
			adopted_dirs[i][0] = '\0';
	}
	pthread_mutex_unlock(&adopted_lock);
}

static void remove_files(const char *dir)
{
	static const char *const files[] = { "initrd", "console.log", "qemu.log", "passt.log",
	                                     "passt.pid" /* written by a core before 2026-10-21 */ };
	char path[4200];

	static const char *const staging[] = { "merged", "lower", "kept", "mem" };

	for (unsigned i = 0u; i < sizeof files / sizeof files[0]; i++) {
		snprintf(path, sizeof path, "%s/%s", dir, files[i]);
		unlink(path);
	}
	/* What a host-side overlay staged, by name: on the host each is an empty
	 * directory, its mounts having been in qemu's own namespace, and rmdir
	 * takes nothing that is not. */
	for (unsigned i = 0u; i < sizeof staging / sizeof staging[0]; i++) {
		snprintf(path, sizeof path, "%s/%s", dir, staging[i]);
		rmdir(path);
	}
	for (unsigned i = 1u; i <= MAX_VOLUMES; i++) {
		snprintf(path, sizeof path, "%s/lower%u", dir, i);
		rmdir(path);
	}
	/* And each volume's stack, staged in v0, v1, .... */
	for (unsigned i = 0u; i < MAX_VOLUMES; i++) {
		snprintf(path, sizeof path, "%s/v%u", dir, i);
		if (access(path, F_OK) == 0)
			hull_stack_unstage(path);
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
		if (!is_adopted_dir(path))
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
	if (m->dir[0] != '\0') {
		mark_adopted_dir(m->dir, 0);
		remove_files(m->dir);
	}
	m->dir[0] = '\0';
}

/* The lowest port a user may bind, 1024 where it cannot be read. */
static unsigned unprivileged_port_start(void)
{
	char text[32] = "";
	int  fd = open("/proc/sys/net/ipv4/ip_unprivileged_port_start", O_RDONLY | O_CLOEXEC);

	if (fd >= 0) {
		ssize_t n = read(fd, text, sizeof text - 1u);

		text[n > 0 ? n : 0] = '\0';
		close(fd);
	}
	return text[0] >= '0' && text[0] <= '9' ? (unsigned)strtoul(text, NULL, 10) : 1024u;
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
			unsigned lowest = unprivileged_port_start();

			/* Below the lowest port a user may bind, which is the host's
			 * owner's to lower, and said so, with the way round. */
			if (errno == EACCES && m->port_host[i] < lowest)
				snprintf(m->detail, sizeof m->detail,
				         "port %u on %s: below %u, the lowest a user may bind "
				         "(net.ipv4.ip_unprivileged_port_start): give a host port of %u or "
				         "more, or see hull doctor", m->port_host[i], m->address_text, lowest,
				         lowest);
			else
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
	if (!m->done && m->adopted)
		syscall(SYS_pidfd_send_signal, m->pidfd, SIGKILL, NULL, 0u);
	else if (!m->done && m->qemu > 0)
		kill(m->qemu, SIGKILL);
	pthread_mutex_unlock(&m->lock);
}

static hull_status qemu_start(hull_core *core, hull_machine *m, hull_error *error)
{
	struct utsname name;
	const char    *reason;
	const char    *kernel;         /* the -kernel file */
	const char    *release;        /* its release, for its modules on the host */
	char           kbuf[512];      /* the host kernel's path, for bundled */
	char           ibuf[4200];     /* the image's /hull/kernel, for (kernel image) */
	char           rbuf[256];      /* a path kernel's release */
	int            load_modules;   /* the host has this release's modules */
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

	/* The kernel, and the release whose modules it wants. bundled is the
	 * host's own, always modular here. A path kernel is the user's on disk,
	 * and an image kernel is the image's own /hull/kernel (Appendix F): both
	 * are booted as given, and their modules are the host's for the release
	 * the image carries where it has them (a distribution kernel), or none
	 * where it does not (a kernel with its drivers built in). */
	if (m->kernel_path != NULL || m->kernel_image) {
		if (m->kernel_image) {
			snprintf(ibuf, sizeof ibuf, "%s/hull/kernel", m->root);
			kernel = ibuf;
		} else {
			kernel = m->kernel_path;
		}
		if (access(kernel, R_OK) != 0)
			return fail(error, HULL_ERR_RUNTIME, m->kernel_image ?
			            "(kernel image) wants the image's own /hull/kernel, and it has none" :
			            "the kernel at (kernel \"path:...\") cannot be read");
		if (kernel_release(kernel, rbuf, sizeof rbuf)) {
			char dep[600];

			release = rbuf;
			snprintf(dep, sizeof dep, "/lib/modules/%s/modules.dep", release);
			load_modules = access(dep, R_OK) == 0;
		} else {
			release = "";
			load_modules = 0;
		}
	} else {
		snprintf(kbuf, sizeof kbuf, "/boot/vmlinuz-%s", name.release);
		kernel = kbuf;
		release = name.release;
		load_modules = 1;
	}

	if ((reason = make_dir(m)) != NULL ||
	    (reason = write_initramfs(m->dir, release, load_modules)) != NULL ||
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
		if ((reason = open_segments(m)) != NULL) {
			close(m->network);
			m->network = -1;
			break;
		}
		m->qemu = spawn(m, kernel, release, load_modules, cid);
		close(m->network);
		m->network = -1;
		close_segments(m);
		if (m->qemu < 0)
			break;
		fd = dial(m, cid);
		m->cid = cid;
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
		if (m->secrets.data != NULL)
			hull_msg_bytes(&msg, m->secrets.data, m->secrets.length);
		if (m->ssh.data != NULL)
			hull_msg_bytes(&msg, m->ssh.data, m->ssh.length);
		if (m->names != NULL)
			hull_msg_bytes(&msg, m->names, m->names_length);
		hull_msg_raw(&msg, ")");
		forget_secrets(m);
		forget_ssh(m);
		pthread_mutex_lock(&m->send_lock);
		if (m->connected && hull_wire_send(m->wire.fd, &msg) != 0)
			m->connected = 0;
		pthread_mutex_unlock(&m->send_lock);
		if (msg.data != NULL)
			explicit_bzero(msg.data, msg.cap);
		hull_msg_free(&msg);
		if (expect(m, "ready")) {
			if (getrandom(m->token, sizeof m->token, 0) != (ssize_t)sizeof m->token) {
				snprintf(m->detail, sizeof m->detail, "no randomness for the token");
				goto refused;
			}
			hull_msg_raw(&msg, "(5:start(2:ch1:0)(5:token");
			hull_msg_atom(&msg, m->token, sizeof m->token);
			hull_msg_raw(&msg, "))");
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
refused:
	kill_qemu(m);
	waitpid(m->qemu, NULL, 0);
	m->done = 1;
	hull_wire_close(&m->wire);
	remove_dir(m);
	return fail(error, HULL_ERR_RUNTIME, m->detail);
}

/* --- adoption ------------------------------------------------------------ */

/*
 * A kept machine's qemu outlives the core, its parent-death signal not
 * set; a core that comes after takes it back from what persist() gave:
 * qemu's pid, its context id, its directory, the token the agent was
 * started with, passt's pid, the address and the ports. Text, a line
 * each, this engine's own.
 *
 * Taking it back: the pid must still be a qemu whose command line names
 * the directory, checked through a pidfd, which then stands for it, since
 * it is init's child now and not this process's; the agent is dialled
 * and told `resume` with the token, and answers `started` and what the
 * workload wrote meanwhile; the ports are bound again on the address, as
 * the old core's listeners went with it. The directory is kept from the
 * sweep of a dead core's directories, which its name would otherwise
 * fall to.
 */
static hull_status qemu_persist(hull_core *core, hull_machine *m, hull_buf *out,
                                hull_error *error)
{
	size_t n;
	int    wrote;

	(void)core;
	/* The address only where the plan gave one, which it does for ports. */
	wrote = snprintf(m->persisted, sizeof m->persisted,
	                 "pid %ld\ncid %u\npasst %ld\n%s%s%sdir %s\ntoken ", (long)m->qemu, m->cid,
	                 (long)m->passt, m->address_text[0] != '\0' ? "address " : "",
	                 m->address_text, m->address_text[0] != '\0' ? "\n" : "", m->dir);
	if (wrote < 0 || (size_t)wrote >= sizeof m->persisted - 2u * sizeof m->token - 1u -
	                                   (size_t)m->port_count * 24u)
		return fail(error, HULL_ERR_INTERNAL, "the state does not fit");
	n = (size_t)wrote;
	for (size_t i = 0u; i < sizeof m->token; i++)
		n += (size_t)snprintf(m->persisted + n, sizeof m->persisted - n, "%02x", m->token[i]);
	n += (size_t)snprintf(m->persisted + n, sizeof m->persisted - n, "\n");
	for (unsigned i = 0u; i < m->port_count; i++)
		n += (size_t)snprintf(m->persisted + n, sizeof m->persisted - n, "port %u %u %d\n",
		                      m->port_host[i], m->port_guest[i], m->port_udp[i]);
	out->data = (const uint8_t *)m->persisted;
	out->len  = n;
	return HULL_OK;
}

/* The persisted text into `m`. A reason, or NULL. */
static const char *read_state(hull_machine *m, hull_buf state)
{
	char  text[4800];
	char *line;
	char *rest;
	int   seen = 0;

	if (state.len == 0u || state.len >= sizeof text || memchr(state.data, '\0', state.len))
		return "the state is not this engine's";
	memcpy(text, state.data, state.len);
	text[state.len] = '\0';
	for (line = strtok_r(text, "\n", &rest); line != NULL; line = strtok_r(NULL, "\n", &rest)) {
		long          number;
		unsigned      a;
		unsigned      b;
		int           c;
		char          hex[2u * sizeof m->token + 1u];

		if (sscanf(line, "pid %ld", &number) == 1 && number > 0) {
			m->qemu = (pid_t)number;
			seen |= 1;
		} else if (sscanf(line, "cid %u", &a) == 1 && a >= 3u) {
			m->cid = a;
			seen |= 2;
		} else if (sscanf(line, "passt %ld", &number) == 1) {
			m->passt = number > 0 ? (pid_t)number : 0;
		} else if (strncmp(line, "address ", 8u) == 0 &&
		           inet_pton(AF_INET, line + 8, &m->address) == 1) {
			snprintf(m->address_text, sizeof m->address_text, "%s", line + 8);
			seen |= 4;
		} else if (strncmp(line, "dir /", 5u) == 0 && strlen(line + 4) < sizeof m->dir) {
			snprintf(m->dir, sizeof m->dir, "%s", line + 4);
			seen |= 8;
		} else if (sscanf(line, "token %64s", hex) == 1 && strlen(hex) == 2u * sizeof m->token) {
			for (size_t i = 0u; i < sizeof m->token; i++) {
				unsigned byte;

				if (sscanf(hex + 2u * i, "%2x", &byte) != 1)
					return "the state is not this engine's";
				m->token[i] = (uint8_t)byte;
			}
			seen |= 16;
		} else if (sscanf(line, "port %u %u %d", &a, &b, &c) == 3 && m->port_count < MAX_PORTS &&
		           a >= 1u && a <= 65535u && b >= 1u && b <= 65535u) {
			m->port_host[m->port_count]  = a;
			m->port_guest[m->port_count] = b;
			m->port_udp[m->port_count++] = c != 0;
		} else {
			return "the state is not this engine's";
		}
	}
	if (m->port_count > 0u && !(seen & 4))
		return "the state's ports have no address";
	return (seen | 4) == 31 ? NULL : "the state is not this engine's";
}

/* Whether `pid` is a qemu whose command line names `dir`. */
static int qemu_of(pid_t pid, const char *dir)
{
	char    path[64];
	char    line[16384];
	ssize_t n;
	int     fd;
	int     found = 0;

	snprintf(path, sizeof path, "/proc/%ld/cmdline", (long)pid);
	fd = open(path, O_RDONLY | O_CLOEXEC);
	n  = fd >= 0 ? read(fd, line, sizeof line - 1u) : -1;
	if (fd >= 0)
		close(fd);
	if (n <= 0)
		return 0;
	line[n] = '\0';
	if (strncmp(line, "qemu-system-", 12u) != 0)
		return 0;
	for (ssize_t i = 0; i < n && !found; i += (ssize_t)strlen(line + i) + 1)
		found = strstr(line + i, dir) != NULL;
	return found;
}

static hull_status qemu_adopt(hull_core *core, hull_buf state, hull_machine **out,
                              hull_error *error)
{
	static char    detail[512];
	hull_machine  *m;
	hull_msg       msg = { 0 };
	hull_sexpr_doc body;
	const char    *reason;
	int            fd;

	m = calloc(1u, sizeof *m);
	if (m == NULL)
		return fail(error, HULL_ERR_INTERNAL, "out of memory");
	m->core    = core;
	m->wire.fd = -1;
	m->network = -1;
	m->wake    = -1;
	m->pidfd   = -1;
	m->keep    = 1;
	for (unsigned i = 0u; i < MAX_PORTS; i++)
		m->listeners[i] = -1;
	pthread_mutex_init(&m->lock, NULL);
	pthread_mutex_init(&m->send_lock, NULL);
	pthread_cond_init(&m->done_cond, NULL);
	pthread_cond_init(&m->quiet, NULL);
	pthread_cond_init(&m->guest_cond, NULL);
	if ((reason = read_state(m, state)) != NULL) {
		m->dir[0] = '\0';
		release(m);
		return fail(error, HULL_ERR_UNSUPPORTED, reason);
	}
	/* The pidfd first, then the check, so that the pid it stands for is
	 * the one checked. */
	m->pidfd = (int)syscall(SYS_pidfd_open, m->qemu, 0u);
	if (m->pidfd < 0 || !qemu_of(m->qemu, m->dir)) {
		m->dir[0] = '\0';
		release(m);
		return fail(error, HULL_ERR_GONE, "its qemu is gone");
	}
	m->adopted = 1;
	mark_adopted_dir(m->dir, 1);
	fd = dial(m, m->cid);
	if (fd >= 0 && hull_wire_open(&m->wire, fd, HULL_WIRE_FROM_AGENT) != 0) {
		close(fd);
		fd = -1;
	}
	reason = "the guest agent did not answer";
	if (fd >= 0) {
		m->connected = 1;
		hull_msg_raw(&msg, "(5:hello(2:ch1:0)(8:features)(5:proto1:1))");
		send_msg(m, &msg);
		if (expect(m, "hello")) {
			hull_msg_raw(&msg, "(6:resume(2:ch1:0)(5:token");
			hull_msg_atom(&msg, m->token, sizeof m->token);
			hull_msg_raw(&msg, "))");
			send_msg(m, &msg);
			reason = expect(m, "started") ? NULL : "the guest agent would not resume";
		}
	}
	if (reason == NULL && m->port_count > 0u) {
		m->wake = eventfd(0u, EFD_CLOEXEC);
		reason = open_listeners(m);
	}
	if (reason == NULL && pthread_create(&m->reader, NULL, read_agent, m) != 0)
		reason = "the reader thread could not start";
	if (reason != NULL) {
		/* The machine is left as it is, for another core to try: it is
		 * not this one's to stop. */
		snprintf(detail, sizeof detail, "%s", reason);
		hull_wire_close(&m->wire);
		close_listeners(m);
		mark_adopted_dir(m->dir, 0);
		m->dir[0] = '\0';
		release(m);
		return fail(error, HULL_ERR_RUNTIME, detail);
	}
	(void)body;
	m->started = 1;
	if (m->port_count > 0u && m->wake >= 0 &&
	    pthread_create(&m->forwarder, NULL, forward_ports, m) == 0)
		m->forwarding = 1;
	else if (m->port_count > 0u)
		host->log(core, 3, "the machine's ports cannot be forwarded");
	*out = m;
	host->on_state(core, m, "running");
	return HULL_OK;
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
	/* The work directories overlayfs left, which the user could not
	 * remove: its namespace, and so its mounts, are gone with it. */
	hull_stack_unwork(m->upper_work);
	for (unsigned i = 0u; i < m->volume_count; i++)
		hull_stack_unwork(m->volume_stack[i].upper_work);
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

/* The guest's own view, asked of the agent by B.3's (sub (what stats)) on a
 * channel of this request's own, and awaited, 5 s at most. The reply's cpu,
 * io and mem become the engine's (stats ...) the core relays, the same as
 * the host view's but measured inside. One request at a time holds the one
 * guest_ch; read_agent captures the reply and wakes the wait. */
static hull_status qemu_guest_stats(hull_core *core, hull_machine *m, hull_buf *out,
                                    hull_error *error)
{
	hull_msg        msg = { 0 };
	struct timespec deadline;
	uint32_t        ch;
	int             got;

	(void)core;
	pthread_mutex_lock(&m->lock);
	if (!m->started || m->done || !m->connected) {
		pthread_mutex_unlock(&m->lock);
		return fail(error, HULL_ERR_GONE, "the machine is not running");
	}
	ch = ++m->next_ch;
	m->guest_ch = ch;
	m->guest_ready = 0;
	pthread_mutex_unlock(&m->lock);

	hull_msg_raw(&msg, "(3:sub");
	hull_msg_channel(&msg, ch);
	hull_msg_raw(&msg, "(4:what5:stats))");
	pthread_mutex_lock(&m->send_lock);
	if (m->connected && hull_wire_send(m->wire.fd, &msg) != 0)
		m->connected = 0;
	pthread_mutex_unlock(&m->send_lock);
	hull_msg_free(&msg);

	clock_gettime(CLOCK_REALTIME, &deadline);
	deadline.tv_sec += 5;
	pthread_mutex_lock(&m->lock);
	while (!m->guest_ready)
		if (pthread_cond_timedwait(&m->guest_cond, &m->lock, &deadline) == ETIMEDOUT)
			break;
	got = m->guest_ready;
	m->guest_ch = 0u;
	if (got) {
		out->data = m->stats;
		out->len = hull_usage_encode(&m->guest_usage, m->stats, sizeof m->stats);
	}
	pthread_mutex_unlock(&m->lock);
	if (!got)
		return fail(error, HULL_ERR_GONE, "the guest did not answer with its stats");
	return out->len == 0u ? fail(error, HULL_ERR_INTERNAL, "guest stats do not fit") : HULL_OK;
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

/* The guest stopped whole, qemu with it, or let go on: qemu is stopped
 * as a process, every vCPU and device with it, so the guest sees nothing
 * but its clock having jumped, and its network stops answering. */
static hull_status qemu_pause(hull_core *core, hull_machine *m, int paused, hull_error *error)
{
	int gone;

	(void)core;
	pthread_mutex_lock(&m->lock);
	gone = !m->started || m->done || m->qemu <= 0 || kill(m->qemu, paused ? SIGSTOP : SIGCONT) != 0;
	pthread_mutex_unlock(&m->lock);
	return gone ? fail(error, HULL_ERR_GONE, "the machine is not running") : HULL_OK;
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
	{ "volumes.layers",     HULL_SUP_NATIVE,
	  "an overlay mounted host-side, in qemu's namespace, and shared", NULL },
	{ "secrets",            HULL_SUP_NATIVE,
	  "placed by the agent on a tmpfs under one at the machine's /run, which it must "
	  "have", NULL },
	{ "cpus",               HULL_SUP_NATIVE,   NULL, NULL },
	{ "memory",             HULL_SUP_NATIVE,   "a hard allocation", NULL },
	/* Section 6's host forwarder: the engine's, over vsock to the agent. */
	{ "ports",              HULL_SUP_EMULATED,
	  "tcp and udp, forwarded over vsock from the machine's address", NULL },
	{ "network.host",       HULL_SUP_REJECTED,
	  "a vm shares no network namespace with the host: use ports", NULL },
	{ "network.none",       HULL_SUP_NATIVE,   "no NIC: lo alone in the guest", NULL },
	{ "ssh",                HULL_SUP_NATIVE,
	  "the agent supervises the image's own sshd on the guest's port 22, with a host key, "
	  "the user's authorized_keys and a config the core delivers; refused at start if the "
	  "image has no sshd", NULL },
	{ "export",             HULL_SUP_REJECTED,
	  "not yet: the guest's tree is the agent's to write, and it does not", NULL },
	{ "layers",             HULL_SUP_NATIVE,
	  "read-only, in an overlay qemu mounts host-side, the guest's share its merged tree", NULL },
	{ "upper",              HULL_SUP_NATIVE,
	  "the overlay's writes kept in the directory, mounted host-side, one uid", NULL },
	{ "owners",             HULL_SUP_NATIVE,
	  "given in the overlay qemu shares, where the user's subordinate range is mapped", NULL },
	{ "init",               HULL_SUP_NATIVE,
	  "the image's init is PID 1, the agent a child of it, and its poweroff ends the machine",
	  NULL },
	{ "segments",           HULL_SUP_NATIVE,
	  "a NIC each after passt's, eth1 on, on the core's switch, with no address given", NULL },
	{ "impair",             HULL_SUP_NATIVE,
	  "in the core's switch, each way of the machine's port on a segment, in netem's terms", NULL },
	{ "tamper",             HULL_SUP_NATIVE,
	  "in the core's switch, a targeted fault on each way of the machine's port on a segment", NULL },

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
	{ "on_disconnect.keep", HULL_SUP_NATIVE,
	  "kept, and taken back by a core that comes after one that went", NULL },

	/* J.5. A q35 machine on the host's own kernel stands in for the
	 * micro profile and Appendix F's bundled kernel. */
	{ "vm.profile.micro",    HULL_SUP_EMULATED,
	  "a q35 machine on the host's kernel, which boots in about two seconds", NULL },
	{ "vm.kernel.bundled",   HULL_SUP_EMULATED,
	  "the host's own kernel and modules, until Appendix F ships one", NULL },
	{ "vm.kernel.path",      HULL_SUP_NATIVE,
	  "the kernel at the given path, booted as given; its modules are the host's for its "
	  "release where it has them, else it must have its guest drivers built in", NULL },
	{ "vm.kernel.image",     HULL_SUP_NATIVE,
	  "the image's own /hull/kernel, booted as given; a different kernel version per machine, "
	  "its modules the host's for its release where it has them, else built in", NULL },
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
	.adopt        = qemu_adopt,
	.persist      = qemu_persist,
	.exec         = qemu_exec,
	.stream_write = qemu_stream_write,
	.stream_eof   = qemu_stream_eof,
	.stream_close = qemu_stream_close,
	.resize       = qemu_resize,
	.signal       = qemu_signal,
	.stats        = qemu_stats,
	.guest_stats  = qemu_guest_stats,
	.dial         = qemu_dial,
	.pause        = qemu_pause,
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
