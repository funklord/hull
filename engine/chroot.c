/*
 * chroot.c -- the reference engine: a dir source run in place, rootless
 * (project.md sections 5.0 and 4.8).
 *
 * A machine is cloned into new user, mount, UTS and PID namespaces, with
 * the invoking user mapped to root, its directory bound onto itself and
 * chrooted into, or under (mutable overlay) layered beneath a tmpfs and
 * the overlay chrooted into. The first process is a small init, PID 1: it forks the
 * workload as PID 2, forwards it signals, reaps whatever is orphaned to
 * it, and reports the workload's wait status to the engine through a
 * pipe. PID 1 ignores a signal it has no handler for, so the workload
 * cannot be PID 1 itself, or stop's SIGTERM would do nothing; and a
 * SIGKILL to the init ends the whole namespace, so nothing the workload
 * started outlives the machine. That is enough for the fields declared
 * below and no more: there is no network namespace, so a port is the
 * host's own.
 *
 * A volume's host side is cloned with open_tree() before the chroot and
 * attached with move_mount() after it, so its guest path is resolved
 * inside the machine. Mounted before the chroot, a symlink in the rootfs
 * would be followed on the host, and the volume land wherever it led.
 *
 * The workload's output reaches the core through the init, which holds
 * the workload's pipes and relays what they carry, and the workload's
 * end, over a socket pair to the engine: a frame each, `kind fd length`
 * and the bytes. One thread per machine reads them into on_data
 * (hull_engine.h, rule 3) and reports on_exit. Under (on_disconnect keep)
 * that is what lets the machine outlive the core: the init has no
 * parent-death signal, holds the workload's last bytes while no core
 * reads them, and listens on init.sock in the machine's directory for the
 * core that takes the machine back (adopt()). Every engine-owned
 * allocation is the machine handle's, freed by destroy().
 */

#define _GNU_SOURCE

#include <dirent.h>
#include <errno.h>
#include <pwd.h>
#include <fcntl.h>
#include <arpa/inet.h>
#include <linux/if_tun.h>
#include <net/if.h>
#include <net/route.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/prctl.h>
#include <sys/reboot.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <sys/un.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include "access.h"
#include "hostnet.h"
#include "tree.h"
#include "builtin.h"
#include "usage.h"
#include "stack.h"
#include "idmap.h"
#include "ip6.h"

#define MAX_ARGS    256u
#define MAX_ENV     256u
#define MAX_VOLUMES 64u
#define MAX_SEGMENTS 8u
#define MAX_PORTS    64u
/* What pasta gives a machine under NAT, as passt gives a vm (engine/qemu.c):
 * its address, the gateway, and the host's resolver through DNS_ADDRESS. */
#define GUEST_ADDRESS   "10.0.2.15"
#define GATEWAY_ADDRESS "10.0.2.2"
#define DNS_ADDRESS     "10.0.2.3"
#define HELD_LIMIT  (256u * 1024u)  /* output an init holds for no core */
/* How long an init may take to reach its workload: the core waits for it
 * on its own thread, every frontend with it, so a hang is bounded rather
 * than for ever. A large tree's owners given on a loaded host are the
 * slow part, well inside it. */
#ifndef START_LIMIT_MS
#define START_LIMIT_MS 600000
/* Reads of the init's cgroup, 10 ms apart, for its scope to take it. */
#define LIMITS_TRIES 500
#endif
/* (ssh true)'s sshd port: a high one, since the chroot user is unprivileged
 * on the shared host net and cannot bind 22; the machine's 127.107 address
 * makes it unique per machine (section 8.1). */
#define HULL_SSH_PORT 2222

/* A frame from the init: data on fd 1 or 2, or the workload's end, whose
 * wait status is the four bytes after. */
typedef struct frame {
	uint8_t  kind;    /* 'd' data, 'x' the end */
	uint8_t  fd;
	uint8_t  unused[2];
	uint32_t length;
} frame;

struct hull_machine {
	hull_core       *core;       /* what create() was given, for the callbacks */
	char            *root;       /* the dir source, without "dir:" */
	char            *workdir;
	char            *hostname;   /* or NULL */
	char            *init;       /* (init PATH): the image's init, PID 1 once handed to */
	char             binfmt[400]; /* a foreign arch's binfmt_misc registration, or "" */
	char            *argv[MAX_ARGS + 1u];
	char            *envp[MAX_ENV + 1u];
	int              readonly_root;
	int              overlay;    /* (mutable overlay), and the root not read-only */
	int              many;       /* the user's subordinate range mapped, not one uid */
	int              no_new_privs;
	char            *volume_host[MAX_VOLUMES];
	char            *volume_guest[MAX_VOLUMES];
	int              volume_readonly[MAX_VOLUMES];
	hull_stack       volume_stack[MAX_VOLUMES];  /* a volume of (layers ...), or count 0 */
	unsigned         volume_count;
	int              staged;     /* a volume is a stack, staged in the machine's directory */
	char            *secrets;    /* the core's directory of them, or NULL */
	char            *ssh_dir;    /* (ssh true): the core's directory of ssh material, or NULL */
	int              ssh;        /* (ssh true): supervise the image's sshd on the machine's address */
	char            *export;     /* (export PATH): the tree's tar, once the workload ends */
	char            *owners;     /* (owners PATH): owners to give back, before the workload */
	char            *layer_path[MAX_VOLUMES];  /* (layers ...): read-only, each above the last */
	unsigned         layer_count;
	char            *upper;      /* (upper PATH): the overlay's writes, kept, the user's */
	char            *upper_work; /* overlayfs's work directory, beside it, hidden */
	unsigned         env_count;  /* envp's, while secrets are added to it */

	pid_t            pid;        /* the machine's init */
	int              conn;       /* the init's frames, or -1 */
	int              keep;       /* (on_disconnect keep) */
	int              no_network; /* (network none): a namespace with only lo */
	int              nat;        /* no (network ...): its own namespace, pasta's NAT out */
	int              bridge;     /* (network bridge): a veth on a host bridge, its core root */
	int              bridge_own; /* on HULL_BRIDGE, hull addressing it, not (bridge NAME) */
	char             bridge_name[16];
	char             bridge_address[16];  /* its address on HULL_BRIDGE, or "" */
	int              bridge_relay;  /* asking hull0's DNS relay, the host's resolvers loopback */
	unsigned         cpus;       /* (cpus N): N CPUs' time a second, by its scope, or 0 */
	unsigned long long memory;   /* (memory SIZE): its scope's ceiling in bytes, or 0 */
	hull_device      device[HULL_MAX_DEVICES];  /* (devices ...), found on the host at start */
	unsigned         device_count;
	unsigned         port_count; /* (ports ...), forwarded by pasta on the address */
	int              port_udp[MAX_PORTS];
	unsigned         port_host[MAX_PORTS];
	unsigned         port_guest[MAX_PORTS];
	char            *segment[MAX_SEGMENTS];     /* Appendix D's, by name */
	int              segment_fd[MAX_SEGMENTS];  /* their ports, until the init has them */
	unsigned         segment_count;
	char             address_text[64];          /* the plan's, for each NIC's MAC */
	char             segment_address[MAX_SEGMENTS][16];  /* hull's on it, or "" (Appendix D) */
	unsigned         segment_prefix[MAX_SEGMENTS];
	char             segment_address6[MAX_SEGMENTS][46]; /* its IPv6 one, or "" */
	unsigned         segment_prefix6[MAX_SEGMENTS];
	int              segment_gateway[MAX_SEGMENTS];      /* its .1 the way out */
	char            *hosts;      /* the /etc/hosts its addressed segments give it, or NULL */
	char            *resolver;   /* the /etc/resolv.conf naming hull at their .1, or NULL */
	int              adopted;    /* taken back: the init is not this process's child */
	int              pidfd;      /* the init's, when adopted, else -1 */
	pid_t            pasta;      /* pasta's daemon, its network and ports, or 0 */
	char             dir[4096];  /* the machine's directory, for init.sock, or "" */
	uint8_t         *plan;       /* the plan, for persist() */
	size_t           plan_length;
	char            *persisted;  /* what persist() last gave */
	pthread_t        reader;
	int              started;
	pthread_mutex_t  lock;
	pthread_cond_t   done_cond;
	int              done;
	hull_usage       spent;      /* finished execs', which their helpers reaped */
	unsigned char    stats[128]; /* what stats() last gave */
	char             detail[256];
};

static const hull_host_ops *host;

/* --- reading the plan ---------------------------------------------------- */

static void release(hull_machine *m)
{
	hull_hostdev_unclaim(m);
	free(m->root);
	free(m->workdir);
	free(m->hostname);
	free(m->init);
	free(m->secrets);
	free(m->ssh_dir);
	free(m->hosts);
	free(m->resolver);
	free(m->export);
	free(m->owners);
	for (unsigned i = 0u; i < m->layer_count; i++)
		free(m->layer_path[i]);
	free(m->upper);
	free(m->upper_work);
	free(m->plan);
	free(m->persisted);
	if (m->pidfd >= 0)
		close(m->pidfd);
	if (m->conn >= 0)
		close(m->conn);
	for (unsigned i = 0u; m->argv[i] != NULL; i++)
		free(m->argv[i]);
	for (unsigned i = 0u; m->envp[i] != NULL; i++)
		free(m->envp[i]);
	for (unsigned i = 0u; i < m->volume_count; i++) {
		free(m->volume_host[i]);
		free(m->volume_guest[i]);
		hull_stack_release(&m->volume_stack[i]);
	}
	for (unsigned i = 0u; i < m->segment_count; i++) {
		free(m->segment[i]);
		if (m->segment_fd[i] >= 0)
			close(m->segment_fd[i]);
	}
	pthread_mutex_destroy(&m->lock);
	pthread_cond_destroy(&m->done_cond);
	free(m);
}

static hull_status fail(hull_error *error, hull_status code, const char *detail)
{
	if (error != NULL) {
		error->code           = code;
		error->detail         = detail;
		error->runtime_detail = NULL;
	}
	return code;
}

/* An exposed secret's `NAME=VALUE`, after the env's own. */
static void add_exposed(void *context, const char *pair)
{
	hull_machine *m = context;

	if (m->env_count < MAX_ENV && (m->envp[m->env_count] = strdup(pair)) == NULL)
		m->env_count = MAX_ENV;  /* refused as one too many, which is near enough */
	m->env_count++;
}

/* "2G" in bytes, as J.1 writes a size. */
static unsigned long long bytes_of(const char *size)
{
	char              *unit;
	unsigned long long n = strtoull(size, &unit, 10);

	switch (*unit) {
	case 'T': n <<= 10; /* fall through */
	case 'G': n <<= 10; /* fall through */
	case 'M': n <<= 10; /* fall through */
	case 'K': n <<= 10; break;
	default:  break;
	}
	return n;
}

static const char *foreign_arch(hull_machine *m, const char *arch);

/* The plan's machine, read into `m`. Returns a reason, or NULL. */
static const char *read_plan(hull_machine *m, const hull_sexpr_doc *doc)
{
	hull_sexpr_form machine;
	hull_sexpr_form entry;
	hull_sexpr_form item;
	char           *source;
	unsigned        n;
	int             more;

	if (!hull_sexpr_first(doc, &machine) ||
	    !hull_access_find(doc, &machine, "machine", &machine))
		return "the plan holds no machine";
	source = hull_access_value(doc, &machine, "source");
	if (source == NULL || strncmp(source, "dir:", 4u) != 0 || source[4] == '\0') {
		free(source);
		return "chroot runs only a dir source";
	}
	m->root = hull_access_path(doc, source + 4);
	free(source);
	m->workdir  = hull_access_value(doc, &machine, "workdir");
	m->hostname = hull_access_value(doc, &machine, "hostname");
	m->init = hull_access_value(doc, &machine, "init");
	if (m->root == NULL || m->workdir == NULL)
		return "the plan is missing its source or workdir";
	{
		char       *arch = hull_access_value(doc, &machine, "arch");
		const char *why  = foreign_arch(m, arch);

		free(arch);
		if (why != NULL)
			return why;
	}

	if (!hull_access_find(doc, &machine, "command", &entry))
		return "the machine names no command, and no stored tree's runtime gives one, so it needs (command ...)";
	hull_sexpr_child(doc, &entry, &item);
	n = 0u;
	for (more = hull_sexpr_next(doc, &item, &item); more;
	     more = hull_sexpr_next(doc, &item, &item)) {
		if (n == MAX_ARGS)
			return "the command has too many arguments";
		m->argv[n] = hull_access_text(doc, &item);
		if (m->argv[n++] == NULL)
			return "an argument holds a NUL byte";
	}

	n = 0u;
	if (hull_access_find(doc, &machine, "env", &entry)) {
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
			if (key != NULL && text != NULL)
				m->envp[n] = malloc(strlen(key) + strlen(text) + 2u);
			if (m->envp[n] != NULL)
				sprintf(m->envp[n], "%s=%s", key, text);
			free(key);
			free(text);
			if (m->envp[n++] == NULL)
				return "an env value holds a NUL byte, or memory ran out";
		}
	}

	if (hull_access_find(doc, &machine, "volumes", &entry)) {
		hull_sexpr_child(doc, &entry, &item);
		for (more = hull_sexpr_next(doc, &item, &item); more;
		     more = hull_sexpr_next(doc, &item, &item)) {
			char       *mode;
			char       *host_path;
			const char *why;
			unsigned    i = m->volume_count;

			if (m->volume_count == MAX_VOLUMES)
				return "there are too many volumes";
			m->volume_count++;
			m->volume_guest[i] = hull_access_value(doc, &item, "guest");
			mode = hull_access_value(doc, &item, "mode");
			m->volume_readonly[i] = mode != NULL && strcmp(mode, "ro") == 0;
			free(mode);
			if ((why = hull_access_stack(doc, &item, m->volume_readonly[i],
			                             &m->volume_stack[i])) != NULL)
				return why;
			if (m->volume_stack[i].count > 0u) {
				m->staged = 1;
			} else {
				host_path = hull_access_value(doc, &item, "host");
				m->volume_host[i] = host_path != NULL ? hull_access_path(doc, host_path) : NULL;
				free(host_path);
			}
			if ((m->volume_host[i] == NULL && m->volume_stack[i].count == 0u) ||
			    m->volume_guest[i] == NULL)
				return "a volume is missing its host or guest path";
		}
	}

	if (hull_access_find(doc, &machine, "security", &entry)) {
		m->readonly_root = hull_access_true(doc, &entry, "readonly_root");
		m->no_new_privs  = hull_access_true(doc, &entry, "no_new_privs");
	}
	/* A read-only root has nothing for an overlay to hold, so it is the
	 * directory itself, read-only, either way. */
	source     = hull_access_value(doc, &machine, "mutable");
	m->overlay = source != NULL && strcmp(source, "overlay") == 0 && !m->readonly_root;
	free(source);

	source  = hull_access_value(doc, &machine, "on_disconnect");
	m->keep = source != NULL && strcmp(source, "keep") == 0;
	free(source);

	/* (export PATH): what the machine made of its tree, as a tar, owners
	 * as it saw them. Only an overlay's: in place the tree is the user's
	 * own directory already, and its owners the user's. */
	if ((source = hull_access_value(doc, &machine, "export")) != NULL) {
		m->export = hull_access_path(doc, source);
		free(source);
		if (m->export == NULL)
			return "the export's path cannot be read";
		if (!m->overlay)
			return "an export is of an overlay's tree: (export ...) needs (mutable overlay) and "
			       "a root that is not read-only";
	}
	/* (layers "dir:..." ...): read-only layers over the source, each above
	 * the last, and (upper PATH): the overlay's writes kept in a directory
	 * of the user's, as unionfs's were, rather than in memory (3.7). */
	if (hull_access_find(doc, &machine, "layers", &entry)) {
		hull_sexpr_form item;
		int             more;

		hull_sexpr_child(doc, &entry, &item);
		for (more = hull_sexpr_next(doc, &item, &item); more;
		     more = hull_sexpr_next(doc, &item, &item)) {
			char *text = hull_access_text(doc, &item);

			if (m->layer_count == MAX_VOLUMES) {
				free(text);
				return "there are too many layers";
			}
			if (text == NULL || strncmp(text, "dir:", 4u) != 0) {
				free(text);
				return "a layer is a dir: source";
			}
			m->layer_path[m->layer_count] = hull_access_path(doc, text + 4);
			free(text);
			if (m->layer_path[m->layer_count++] == NULL)
				return "a layer's path cannot be read";
		}
		if (!m->overlay)
			return "layers lie under an overlay: (layers ...) needs (mutable overlay) and a root "
			       "that is not read-only";
	}
	if ((source = hull_access_value(doc, &machine, "upper")) != NULL) {
		char *slash;

		m->upper = hull_access_path(doc, source);
		free(source);
		if (m->upper == NULL)
			return "the upper's path cannot be read";
		if (!m->overlay)
			return "an upper is an overlay's: (upper ...) needs (mutable overlay) and a root "
			       "that is not read-only";
		/* overlayfs's work directory must be on the upper's own filesystem,
		 * so beside it, named for it and hidden. */
		if ((m->upper_work = malloc(strlen(m->upper) + 16u)) == NULL)
			return "out of memory";
		slash = strrchr(m->upper, '/');
		if (slash == NULL)
			sprintf(m->upper_work, ".%s.hull-work", m->upper);
		else
			sprintf(m->upper_work, "%.*s/.%s.hull-work", (int)(slash - m->upper), m->upper,
			        slash + 1);
	}
	/* (owners PATH): a stored tree's owners, given back in the merged tree
	 * before the workload, which copies each file so given up into the
	 * overlay's tmpfs; in place they would be owners on the user's disk. */
	if ((source = hull_access_value(doc, &machine, "owners")) != NULL) {
		m->owners = hull_access_path(doc, source);
		free(source);
		if (m->owners == NULL)
			return "the owners' path cannot be read";
		if (!m->overlay)
			return "owners are given above an overlay: (owners ...) needs (mutable overlay) and "
			       "a root that is not read-only";
	}

	{
		const char *reason = hull_access_devices(doc, &machine, m->device, &m->device_count);

		if (reason != NULL)
			return reason;
	}
	/* (cpus N) and (memory SIZE), section 6's ceiling, given by the
	 * scope start_limits() makes. */
	if ((source = hull_access_value(doc, &machine, "cpus")) != NULL) {
		m->cpus = (unsigned)strtoul(source, NULL, 10);
		free(source);
	}
	if ((source = hull_access_value(doc, &machine, "memory")) != NULL) {
		m->memory = bytes_of(source);
		free(source);
	}

	/* (network nat), or no (network ...), is a namespace of the machine's
	 * own with pasta's NAT out, as passt's is a vm's and docker's bridge a
	 * container's (section 8); (network host) is the host's own, asked
	 * for; (network none) a namespace of its own with lo alone. */
	source = hull_access_value(doc, &machine, "network");
	m->no_network = source != NULL && strcmp(source, "none") == 0;
	m->nat        = source == NULL || strcmp(source, "nat") == 0;
	m->bridge     = source != NULL && strcmp(source, "bridge") == 0;
	free(source);
	/* (network bridge): a veth on a host bridge, which only root may put
	 * there, so only a core run as root takes it; hull's own bridge, or
	 * the one (bridge NAME) names, which must be there already. */
	if (m->bridge) {
		char *name = hull_access_value(doc, &machine, "bridge");

		if (getuid() != 0) {
			free(name);
			return "(network bridge) needs hull's core to run as root: (network nat) gives a "
			       "network of the machine's own without it";
		}
		m->bridge_own = name == NULL;
		snprintf(m->bridge_name, sizeof m->bridge_name, "%s", name != NULL ? name : HULL_BRIDGE);
		free(name);
		/* By the core's own network namespace, not sysfs, which shows the
		 * one that mounted it. */
		if (!m->bridge_own && if_nametoindex(m->bridge_name) == 0)
			return "(bridge NAME) names no interface on the host";
		if (m->bridge_own) {
			hull_sexpr_form top;
			int             drawn;

			hull_sexpr_first(doc, &top);
			source = hull_access_value(doc, &top, "address");
			drawn = hull_hostnet_address(source, m->bridge_address);
			free(source);
			if (drawn != 0)
				return "the plan gives the machine no 127.107 address to draw its bridge's from";
		}
	}
	if (hull_access_find(doc, &machine, "ports", &entry)) {
		hull_sexpr_form top;

		/* On hull's own bridge pasta forwards them to the machine's address
		 * there; on one its owner named the address is the workload's, so
		 * there is nothing hull knows to forward to. */
		if (m->bridge && !m->bridge_own)
			return "ports on a bridge (bridge NAME) names are not forwarded: the machine's address "
			       "there is its own, and it is reached at it";
		if (!m->nat && !m->bridge)
			return m->no_network ? "ports need the machine's network, which (network none) has not"
			                     : "ports need a network of the machine's own: (network host) is "
			                       "the host's, where a port is the host's own";
		hull_sexpr_first(doc, &top);
		source = hull_access_value(doc, &top, "address");
		if (source == NULL || source[0] == '\0' || strlen(source) >= sizeof m->address_text) {
			free(source);
			return "the plan gives its ports no address";
		}
		snprintf(m->address_text, sizeof m->address_text, "%s", source);
		free(source);
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
	/* Under NAT the machine asks pasta's forwarder, at DNS_ADDRESS, over
	 * its /etc/resolv.conf, as the agent has a vm ask passt's. */
	if (m->nat && (m->resolver = strdup("nameserver " DNS_ADDRESS "\n")) == NULL)
		return "out of memory";
	/* On hull's bridge, the host's own resolvers that are not its loopback,
	 * which the machine could not reach; a machine on a bridge of its
	 * owner's naming keeps its tree's, its addressing being its own. */
	if (m->bridge_own && (m->resolver = hull_hostnet_resolvers()) == NULL) {
		/* None but the host's loopback: hull0's relay, at its gateway. */
		if (errno == ENOMEM ||
		    (m->resolver = strdup("nameserver " BRIDGE_GATEWAY "\n")) == NULL)
			return "out of memory";
		m->bridge_relay = 1;
	}

	/* (segments ...): a NIC on each in the machine's own network namespace,
	 * which (network none) and NAT give it and (network host) does not: a
	 * tap needs a namespace the user's own, and the host's is not. Named
	 * as a vm's are, after pasta's eth0 under NAT, eth1 on, and from eth0
	 * under (network none), where no uplink comes first. A segment is the
	 * core's switch, which goes with it. */
	if (hull_access_find(doc, &machine, "segments", &entry)) {
		hull_sexpr_form top;

		hull_sexpr_child(doc, &entry, &item);
		for (more = hull_sexpr_next(doc, &item, &item); more;
		     more = hull_sexpr_next(doc, &item, &item)) {
			if (m->segment_count == MAX_SEGMENTS)
				return "there are too many segments";
			if ((m->segment[m->segment_count++] = hull_access_segment_name(doc, &item)) == NULL)
				return "a segment's name is not text";
		}
		if (m->segment_count != 0u && !m->no_network && !m->nat && !m->bridge)
			return "a chroot machine on a segment needs a network namespace of its own: not "
			       "(network host) beside (segments ...)";
		if (m->segment_count != 0u && m->keep)
			return "a machine on a segment cannot outlive the core, whose switch the segment "
			       "is: (on_disconnect keep) and (segments ...) do not go together";
		hull_sexpr_first(doc, &top);
		source = hull_access_value(doc, &top, "address");
		if (m->segment_count != 0u && (source == NULL || strlen(source) >= sizeof m->address_text)) {
			free(source);
			return "the plan gives its segments no address to draw a MAC from";
		}
		snprintf(m->address_text, sizeof m->address_text, "%s", source != NULL ? source : "");
		free(source);
		/* The segments hull addresses, the plan's (names ...): an address
		 * on each NIC, and every sibling's name in /etc/hosts. */
		if (hull_access_find(doc, &top, "names", &entry)) {
			for (unsigned i = 0u; i < m->segment_count; i++) {
				hull_access_named(doc, &entry, m->segment[i], m->segment_address[i],
				                  &m->segment_prefix[i]);
				hull_access_named6(doc, &entry, m->segment[i], m->segment_address6[i],
				                   &m->segment_prefix6[i]);
				m->segment_gateway[i] = hull_access_gateway(doc, &entry, m->segment[i]);
			}
			/* hull at their .1 is the one to ask, which names a sibling
			 * that joins later, and under NAT or on hull's bridge forwards
			 * the rest to the host's resolvers (Appendix D); on a bridge
			 * its owner named the tree's own stays, as above. */
			if ((m->hosts = hull_access_hosts(doc, &entry)) == NULL)
				return "the plan's names cannot be read";
			if (!m->bridge || m->bridge_own) {
				char *resolver = hull_access_resolver(doc, &entry);

				if (resolver != NULL) {
					free(m->resolver);
					m->resolver = resolver;
				} else if (m->no_network) {
					return "the plan's names cannot be read";
				}
			}
		}
	}

	/* The secrets the core wrote (J.3), and those to be in the env. */
	if (hull_access_find(doc, &machine, "secrets", &entry)) {
		hull_sexpr_form top;

		hull_sexpr_first(doc, &top);
		m->secrets = hull_access_value(doc, &top, "secrets");
		if (m->secrets == NULL)
			return "the plan's secrets were given no directory";
		while (m->envp[n] != NULL)
			n++;
		m->env_count = n;
		if (hull_access_exposed(doc, &machine, add_exposed, m) != 0 || m->env_count > MAX_ENV)
			return "a secret to expose cannot be read, holds a NUL, or is one env name too many";
	}

	/* (ssh true): the image's sshd, which the engine supervises on the
	 * machine's own 127.107 address (section 8.1): bound there on the host
	 * network, and under NAT bound in the machine and forwarded there by
	 * pasta. The address is where hull reaches it, so (network none) --
	 * which has no host-reachable address -- is refused. */
	m->ssh = hull_access_true(doc, &machine, "ssh");
	if (m->ssh) {
		hull_sexpr_form top;
		char           *addr;

		if (m->no_network)
			return "(ssh true) needs a network to be reached on: not with (network none)";
		if (m->bridge && !m->bridge_own)
			return "(ssh true) on a bridge (bridge NAME) names is not forwarded: the machine's "
			       "address there is its own";
		hull_sexpr_first(doc, &top);
		m->ssh_dir = hull_access_value(doc, &top, "ssh_dir");
		if (m->ssh_dir == NULL)
			return "(ssh true): the plan delivered no ssh material";
		addr = hull_access_value(doc, &top, "address");
		if (addr == NULL || addr[0] == '\0' || strlen(addr) >= sizeof m->address_text) {
			free(addr);
			return "(ssh true): the plan gives the machine no address for its sshd";
		}
		snprintf(m->address_text, sizeof m->address_text, "%s", addr);
		free(addr);
	}
	return NULL;
}

/* --- the child ----------------------------------------------------------- */

/* Tell the parent which step failed, and why, then leave. */
/* `path` and every directory above it, 0755, as mkdir -p. 0, or -1. */
static int make_path(const char *path)
{
	char at[4200];

	snprintf(at, sizeof at, "%s", path);
	for (char *slash = strchr(at + 1, '/'); ; slash = strchr(slash + 1, '/')) {
		if (slash != NULL)
			*slash = '\0';
		if (mkdir(at, 0755) != 0 && errno != EEXIST)
			return -1;
		if (slash == NULL)
			return 0;
		*slash = '/';
	}
}

/*
 * A foreign arch, run translated by the host's qemu-user: hull's name for
 * it, qemu's, and the ELF header's magic and mask that binfmt_misc knows
 * its binaries by, as qemu's own qemu-binfmt-conf.sh gives them.
 */
static const struct foreign_arch {
	const char *name;
	const char *qemu;
	const char *magic;
	const char *mask;
} foreign_arches[] = {
	{ "arm64", "aarch64",
	  "\\x7f\\x45\\x4c\\x46\\x02\\x01\\x01\\x00\\x00\\x00\\x00\\x00\\x00\\x00\\x00\\x00\\x02\\x00\\xb7\\x00",
	  "\\xff\\xff\\xff\\xff\\xff\\xff\\xff\\x00\\xff\\xff\\xff\\xff\\xff\\xff\\xff\\xff\\xfe\\xff\\xff\\xff" },
	{ "arm", "arm",
	  "\\x7f\\x45\\x4c\\x46\\x01\\x01\\x01\\x00\\x00\\x00\\x00\\x00\\x00\\x00\\x00\\x00\\x02\\x00\\x28\\x00",
	  "\\xff\\xff\\xff\\xff\\xff\\xff\\xff\\x00\\xff\\xff\\xff\\xff\\xff\\xff\\xff\\xff\\xfe\\xff\\xff\\xff" },
	{ "riscv64", "riscv64",
	  "\\x7f\\x45\\x4c\\x46\\x02\\x01\\x01\\x00\\x00\\x00\\x00\\x00\\x00\\x00\\x00\\x00\\x02\\x00\\xf3\\x00",
	  "\\xff\\xff\\xff\\xff\\xff\\xff\\xff\\x00\\xff\\xff\\xff\\xff\\xff\\xff\\xff\\xff\\xfe\\xff\\xff\\xff" },
	{ "ppc64le", "ppc64le",
	  "\\x7f\\x45\\x4c\\x46\\x02\\x01\\x01\\x00\\x00\\x00\\x00\\x00\\x00\\x00\\x00\\x00\\x02\\x00\\x15\\x00",
	  "\\xff\\xff\\xff\\xff\\xff\\xff\\xff\\xfc\\xff\\xff\\xff\\xff\\xff\\xff\\xff\\xff\\xfe\\xff\\xff\\x00" },
	{ "s390x", "s390x",
	  "\\x7f\\x45\\x4c\\x46\\x02\\x02\\x01\\x00\\x00\\x00\\x00\\x00\\x00\\x00\\x00\\x00\\x00\\x02\\x00\\x16",
	  "\\xff\\xff\\xff\\xff\\xff\\xff\\xff\\xfc\\xff\\xff\\xff\\xff\\xff\\xff\\xff\\xff\\xff\\xfe\\xff\\xff" },
};

/* The host's arch in the spec's spelling, as the CLI gives the planner. */
static const char *host_arch(void)
{
	static struct utsname host;

	if (uname(&host) != 0)
		return "";
	if (strcmp(host.machine, "x86_64") == 0)
		return "amd64";
	if (strcmp(host.machine, "aarch64") == 0)
		return "arm64";
	return host.machine;
}

/* Whether the ELF at `path` asks for no program interpreter, as a binary
 * binfmt_misc opens at registration (its F flag) must not: it runs inside
 * the machine's root, where the host's loader is not. */
static int static_elf(const char *path)
{
	unsigned char header[64];
	int           fd = open(path, O_RDONLY | O_CLOEXEC);
	int           result = 0;

	if (fd < 0)
		return 0;
	if (read(fd, header, sizeof header) == (ssize_t)sizeof header &&
	    memcmp(header, "\177ELF", 4u) == 0 && header[4] == 2 && header[5] == 1) {
		uint64_t phoff = 0u;
		unsigned size  = (unsigned)(header[54] | header[55] << 8);
		unsigned count = (unsigned)(header[56] | header[57] << 8);

		for (int i = 7; i >= 0; i--)
			phoff = phoff << 8 | header[32 + i];
		result = size >= 4u;
		for (unsigned i = 0u; result && i < count; i++) {
			unsigned char type[4];

			if (pread(fd, type, 4u, (off_t)(phoff + (uint64_t)i * size)) != 4)
				result = 0;
			else if ((type[0] | type[1] << 8 | type[2] << 16 | (unsigned)type[3] << 24) == 3u)
				result = 0;   /* PT_INTERP */
		}
	}
	close(fd);
	return result;
}

/* `(arch ARCH)`: nothing for the host's own; for one qemu-user runs, its
 * binfmt_misc registration in m->binfmt, naming the host's static
 * qemu-ARCH; or why the machine cannot run. */
static const char *foreign_arch(hull_machine *m, const char *arch)
{
	const struct foreign_arch *f = NULL;
	static const char *const   dirs[] = { "/usr/bin", "/usr/local/bin" };
	char                       qemu[64];
	static char                why[200];

	m->binfmt[0] = '\0';
	if (arch == NULL || strcmp(arch, "host") == 0 || strcmp(arch, host_arch()) == 0)
		return NULL;
	for (size_t i = 0u; i < sizeof foreign_arches / sizeof foreign_arches[0]; i++)
		if (strcmp(foreign_arches[i].name, arch) == 0)
			f = &foreign_arches[i];
	if (f == NULL) {
		snprintf(why, sizeof why, "chroot does not run arch %s", arch);
		return why;
	}
	for (size_t i = 0u; i < sizeof dirs / sizeof dirs[0]; i++) {
		snprintf(qemu, sizeof qemu, "%s/qemu-%s", dirs[i], f->qemu);
		if (access(qemu, X_OK) != 0)
			continue;
		if (!static_elf(qemu)) {
			snprintf(why, sizeof why, "(arch %s) is run by %s, which is not a static binary, "
			         "as it must be to run in the machine's root", arch, qemu);
			return why;
		}
		snprintf(m->binfmt, sizeof m->binfmt, ":hull-%s:M::%s:%s:%s:F", f->qemu, f->magic,
		         f->mask, qemu);
		return NULL;
	}
	snprintf(why, sizeof why, "(arch %s) on chroot is run by qemu-%s, which is not installed "
	         "(Debian's qemu-user)", arch, f->qemu);
	return why;
}

static void child_fail(int report, const char *step)
{
	char   message[200];
	int    n = snprintf(message, sizeof message, "%s: %s", step, strerror(errno));
	size_t length = n < 0 ? 0u : (size_t)n < sizeof message ? (size_t)n : sizeof message - 1u;

	if (write(report, message, length) < 0)
		_exit(127);
	_exit(127);
}

/* fork() for the machine's init, which is a raw clone of the core and so
 * still looks to glibc like a process with the core's threads: glibc's
 * fork() takes every malloc arena's lock and stdio's list lock first,
 * and one held by a core thread at the clone -- the switch's, a stop's,
 * an exec's stdin writer -- is held for ever here, hanging the init and
 * the core waiting on it. The system call takes none, and what it starts
 * only execs or relays. Found 2026-10-27, with a core thread churning
 * malloc: the first machine's init stuck in __malloc_fork_lock_parent. */
static pid_t bare_fork(void)
{
	return (pid_t)syscall(SYS_clone, SIGCHLD, NULL, NULL, NULL, 0L);
}

/* `text` as `name` in `dir`, the machine's staging tmpfs, cloned as a
 * read-only tree to place once the root is the machine's. */
static int stage_named(const char *dir, const char *name, const char *text, int report)
{
	struct mount_attr readonly = { .attr_set = MOUNT_ATTR_RDONLY | MOUNT_ATTR_NOSUID |
	                                           MOUNT_ATTR_NODEV | MOUNT_ATTR_NOEXEC };
	char              path[4300];
	size_t            length = strlen(text);
	int               fd;
	int               tree;

	snprintf(path, sizeof path, "%s/%s", dir, name);
	fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0444);
	if (fd < 0 || write(fd, text, length) != (ssize_t)length || close(fd) != 0)
		child_fail(report, "the segments' names");
	tree = open_tree(AT_FDCWD, path, OPEN_TREE_CLONE | OPEN_TREE_CLOEXEC);
	if (tree < 0 || mount_setattr(tree, "", AT_EMPTY_PATH, &readonly, sizeof readonly) != 0)
		child_fail(report, "the segments' names");
	return tree;
}

/* `tree` over `path`, a file of the machine's: made where it is absent or
 * a link and the root's writes are an overlay's, which a link's target
 * outside the machine would not be, and refused where the tree is the
 * user's own -- or, where it is not `required`, left as it is. */
static void place_named(int tree, const char *path, int overlay, int required, int report)
{
	struct stat place;
	char        step[200];

	if (lstat(path, &place) != 0 || !S_ISREG(place.st_mode)) {
		int fd;

		if (!overlay && !required) {
			close(tree);
			return;
		}
		if (lstat(path, &place) == 0 && !S_ISLNK(place.st_mode)) {
			errno = EISDIR;
			snprintf(step, sizeof step, "an addressed segment's names go over %s, which is not "
			         "a file", path);
			child_fail(report, step);
		}
		if (!overlay) {
			errno = ENOENT;
			snprintf(step, sizeof step, "an addressed segment's names go over %s, which is not "
			         "in the machine, and is made only above an overlay", path);
			child_fail(report, step);
		}
		if (make_path("/etc") != 0 || (unlink(path) != 0 && errno != ENOENT) ||
		    (fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644)) < 0)
			child_fail(report, path);
		close(fd);
	}
	snprintf(step, sizeof step, "the segments' names over %s", path);
	if (move_mount(tree, "", AT_FDCWD, path, MOVE_MOUNT_F_EMPTY_PATH) != 0)
		child_fail(report, step);
}

static int write_file(const char *path, const char *text)
{
	int     fd = open(path, O_WRONLY | O_CLOEXEC);
	ssize_t n;

	if (fd < 0)
		return -1;
	n = write(fd, text, strlen(text));
	close(fd);
	return n == (ssize_t)strlen(text) ? 0 : -1;
}

/* The flags a bind remount must keep, or the kernel refuses it in a user
 * namespace: they are locked by the mount the namespace was copied from. */
static unsigned long locked_flags(const char *path)
{
	struct statvfs st;
	unsigned long  flags = 0u;

	if (statvfs(path, &st) != 0)
		return 0u;
	if (st.f_flag & ST_NOSUID)
		flags |= MS_NOSUID;
	if (st.f_flag & ST_NODEV)
		flags |= MS_NODEV;
	if (st.f_flag & ST_NOEXEC)
		flags |= MS_NOEXEC;
	if (st.f_flag & ST_NOATIME)
		flags |= MS_NOATIME;
	if (st.f_flag & ST_NODIRATIME)
		flags |= MS_NODIRATIME;
	if (st.f_flag & ST_RELATIME)
		flags |= MS_RELATIME;
	return flags;
}

/*
 * The machine's /dev, where its tree has the directory: a tmpfs of its own
 * with the host's null, zero, full, random, urandom and tty bound in, as a
 * user namespace may bind a device but not make one, its pts a devpts of
 * its own and shm a tmpfs. Mounted before the old root goes, from the
 * host's /dev. Without it /dev was the tree's own, empty in an image, so
 * the first `>/dev/null` made a regular file, what was written to it was
 * read back, and a build committed it into its tree.
 */
/* `path`'s directories below the root, as `mkdir -p` makes them. */
static int make_parents(const char *path)
{
	char copy[300];

	if (snprintf(copy, sizeof copy, "%s", path) >= (int)sizeof copy)
		return -1;
	for (char *slash = strchr(copy, '/'); slash != NULL; slash = strchr(slash + 1, '/')) {
		*slash = '\0';
		if (mkdir(copy, 0755) != 0 && errno != EEXIST)
			return -1;
		*slash = '/';
	}
	return 0;
}

/*
 * The host's devices the machine is given, (devices ...): each node bound
 * at its own path in the machine's /dev, as null is, and a path the spec
 * gave under /dev that is a link to it, as /dev/serial/by-id is, made a
 * link there too, so a program finds the device where it would on the
 * host.
 */
static void bind_devices(const hull_machine *m, int report)
{
	int vfio = 0;

	for (unsigned i = 0u; i < m->device_count; i++)
		vfio = vfio || m->device[i].kind == HULL_DEVICE_PCI;
	/* A PCI device's group node wants VFIO's container node beside it. */
	if (vfio) {
		int fd;

		if (make_parents("dev/vfio/vfio") != 0 ||
		    (fd = open("dev/vfio/vfio", O_WRONLY | O_CREAT | O_CLOEXEC, 0600)) < 0 ||
		    close(fd) != 0 || mount(HULL_VFIO, "dev/vfio/vfio", NULL, MS_BIND, NULL) != 0)
			child_fail(report, "binding /dev/vfio/vfio into /dev");
	}
	for (unsigned i = 0u; i < m->device_count; i++) {
		const hull_hostdev *host = &m->device[i].host;
		char                path[300];
		int                 fd;

		snprintf(path, sizeof path, "dev/%s", host->node + 5);
		if (make_parents(path) != 0 ||
		    (fd = open(path, O_WRONLY | O_CREAT | O_CLOEXEC, 0600)) < 0 || close(fd) != 0 ||
		    mount(host->node, path, NULL, MS_BIND, NULL) != 0)
			child_fail(report, "binding a device of (devices ...) into /dev");
		if (strncmp(host->named, "/dev/", 5u) == 0) {
			snprintf(path, sizeof path, "dev/%s", host->named + 5);
			if (make_parents(path) != 0 || symlink(host->node, path) != 0)
				child_fail(report, "a link in /dev to a device of (devices ...)");
		}
	}
}

static void make_dev(const hull_machine *m, int report)
{
	static const char *const nodes[] = { "null", "zero", "full", "random", "urandom", "tty" };
	static const char *const links[][2] = {
		{ "fd", "/proc/self/fd" }, { "stdin", "/proc/self/fd/0" },
		{ "stdout", "/proc/self/fd/1" }, { "stderr", "/proc/self/fd/2" },
		{ "ptmx", "pts/ptmx" },
	};
	char path[64];

	if (mount("tmpfs", "dev", "tmpfs", MS_NOSUID | MS_NOEXEC, "mode=0755,size=1m") != 0)
		child_fail(report, "mounting /dev");
	for (size_t i = 0u; i < sizeof nodes / sizeof nodes[0]; i++) {
		char host[64];
		int  fd;

		snprintf(path, sizeof path, "dev/%s", nodes[i]);
		snprintf(host, sizeof host, "/dev/%s", nodes[i]);
		fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0666);
		if (fd < 0 || close(fd) != 0 || mount(host, path, NULL, MS_BIND, NULL) != 0)
			child_fail(report, "binding a device into /dev");
	}
	for (size_t i = 0u; i < sizeof links / sizeof links[0]; i++) {
		snprintf(path, sizeof path, "dev/%s", links[i][0]);
		if (symlink(links[i][1], path) != 0)
			child_fail(report, "a link in /dev");
	}
	if (mkdir("dev/pts", 0755) != 0 || mkdir("dev/shm", 01777) != 0)
		child_fail(report, "a directory in /dev");
	/* A devpts of the machine's own, for a pty it opens; a kernel that
	 * refuses one to a user namespace leaves the directory empty. */
	if (mount("devpts", "dev/pts", "devpts", MS_NOSUID | MS_NOEXEC,
	          "newinstance,ptmxmode=0666,mode=0620") != 0)
		errno = 0;
	if (mount("tmpfs", "dev/shm", "tmpfs", MS_NOSUID | MS_NODEV, "mode=1777") != 0)
		child_fail(report, "mounting /dev/shm");
	bind_devices(m, report);
}

/* The topmost of the overlay's lower layers holding `relative`, as layer()
 * has them in the working directory: what it is, in `st`; 0 where none
 * does. */
static int lower_entry(const hull_machine *m, const char *relative, struct stat *st)
{
	char path[4400];

	for (unsigned i = m->layer_count + 1u; i > 0u; i--) {
		if (i == 1u)
			snprintf(path, sizeof path, "lower/%s", relative);
		else
			snprintf(path, sizeof path, "lower%u/%s", i - 1u, relative);
		if (lstat(path, st) == 0)
			return 1;
	}
	return 0;
}

/*
 * What hull makes at the guest path `path` -- an empty file where `file`,
 * else a directory -- made in the overlay's upper layer `upper` before the
 * overlay is mounted, where the lower layers do not hold it: so it is
 * never made in the merged tree, which would copy the directory above it
 * up with its owner, and fail EOVERFLOW where the namespace does not map
 * that owner -- a tree in a setgid shared directory, its directories in
 * the group of the directory. Each directory above it the lowers hold is
 * made in the upper with their mode, so it is what it was but for the
 * owner, which the namespace could not show anyway. A file is made over
 * a link, as place_named() did; a directory never is, a link at a guest
 * path being followed inside the machine. Where something stands in the
 * way, or cannot be made, it is left to the merged tree as before.
 */
static void stage_upper(const hull_machine *m, const char *upper, const char *path, int file)
{
	char        relative[4200];
	char        made[4400];
	struct stat st;
	size_t      end = 0u;

	snprintf(relative, sizeof relative, "%s", path + strspn(path, "/"));
	if (relative[0] == '\0')
		return;
	/* Up to the first the lowers do not hold as a directory: below a
	 * directory the upper has, nothing is copied up. */
	for (;;) {
		char *slash = strchr(relative + end, '/');
		int   last = slash == NULL;

		if (!last)
			*slash = '\0';
		if (!lower_entry(m, relative, &st)) {
			if (!last)
				*slash = '/';
			break;
		}
		if (last) {
			if (S_ISREG(st.st_mode) || S_ISDIR(st.st_mode) || !file || !S_ISLNK(st.st_mode))
				return;
			break;
		}
		if (!S_ISDIR(st.st_mode))
			return;
		*slash = '/';
		end = (size_t)(slash - relative) + 1u;
	}
	/* Nothing the lowers hold above it: it is made at the root, which is
	 * the upper's own. */
	if (end == 0u)
		return;
	/* Each directory the lowers hold, then the rest as make_path() would. */
	for (size_t at = 0u;;) {
		char *slash = strchr(relative + at, '/');

		if (slash != NULL)
			*slash = '\0';
		snprintf(made, sizeof made, "%s/%s", upper, relative);
		if (slash == NULL) {
			if (file)
				close(open(made, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0644));
			else
				mkdir(made, 0755);
			return;
		}
		if (lower_entry(m, relative, &st) && S_ISDIR(st.st_mode)) {
			if (mkdir(made, st.st_mode & 07777) == 0)
				chmod(made, st.st_mode & 07777);
		} else if (mkdir(made, 0755) != 0 && errno != EEXIST) {
			return;
		}
		*slash = '/';
		at = (size_t)(slash - relative) + 1u;
	}
}

/*
 * The directory as an overlay, in the machine's own mount namespace: a
 * clone of it beneath, and above it a tmpfs, which goes with the machine.
 * The clone is taken first, then the tmpfs is mounted over the
 * directory's own path, so the layers need no other place on the host;
 * the overlay is mounted at merged/ there, which becomes the root. The
 * paths given to overlayfs are relative, so a comma or a colon in the
 * directory's path cannot change the mount's options. The upper layer's
 * top takes the directory's mode, so / is what it is in place.
 *
 * A change to a file copies it up, and its parents, keeping their owners;
 * one the namespace does not map cannot be, and the change fails with
 * EOVERFLOW where in place it would have worked.
 */
static void layer(const hull_machine *m, int report)
{
	struct stat lower;
	int         tree = open_tree(AT_FDCWD, m->root, OPEN_TREE_CLONE | OPEN_TREE_CLOEXEC);
	int         layers[MAX_VOLUMES];
	int         kept = -1;
	char        options[8800u + 16u * MAX_VOLUMES];
	char        upper_dir[4200] = "upper";
	char        work_dir[4200] = "work";
	size_t      at;

	if (tree < 0 || fstat(tree, &lower) != 0)
		child_fail(report, "the overlay's lower layer");
	/* Every layer cloned while its path is still the host's, before the
	 * tmpfs goes over the directory's. */
	for (unsigned i = 0u; i < m->layer_count; i++)
		if ((layers[i] = open_tree(AT_FDCWD, m->layer_path[i],
		                           OPEN_TREE_CLONE | OPEN_TREE_CLOEXEC)) < 0)
			child_fail(report, "a layer");
	/* overlayfs wants the upper and its work directory on one mount, so the
	 * directory holding both is cloned, once, and they are named in it. */
	if (m->upper != NULL) {
		char        parent[4200];
		const char *slash = strrchr(m->upper, '/');

		snprintf(parent, sizeof parent, "%.*s", slash != NULL && slash != m->upper ?
		         (int)(slash - m->upper) : 1, slash != NULL ? m->upper : ".");
		snprintf(upper_dir, sizeof upper_dir, "kept/%s", slash != NULL ? slash + 1 : m->upper);
		snprintf(work_dir, sizeof work_dir, "kept/%s", strrchr(m->upper_work, '/') != NULL ?
		         strrchr(m->upper_work, '/') + 1 : m->upper_work);
		/* The work directory is hull's, made here: the user's own group and
		 * not setgid, though its parent may be setgid to another, since
		 * overlayfs refuses a work directory of a group the namespace does
		 * not map, EPERM, which is what one inherited from a shared setgid
		 * directory is. The upper itself it takes in any group. */
		if (strpbrk(upper_dir, ",:") != NULL ||
		    (mkdir(m->upper, 0755) != 0 && errno != EEXIST) ||
		    (mkdir(m->upper_work, 0700) != 0 && errno != EEXIST) ||
		    chown(m->upper_work, (uid_t)-1, 0) != 0 || chmod(m->upper_work, 0700) != 0 ||
		    (kept = open_tree(AT_FDCWD, parent, OPEN_TREE_CLONE | OPEN_TREE_CLOEXEC)) < 0)
			child_fail(report, "the overlay's upper layer, kept");
	}
	if (mount("tmpfs", m->root, "tmpfs", 0u, "mode=0700") != 0 || chdir(m->root) != 0 ||
	    mkdir("lower", 0700) != 0 || mkdir("upper", 0700) != 0 ||
	    mkdir("work", 0700) != 0 || mkdir("merged", 0700) != 0)
		child_fail(report, "the overlay's tmpfs");
	if (move_mount(tree, "", AT_FDCWD, "lower", MOVE_MOUNT_F_EMPTY_PATH) != 0)
		child_fail(report, "the overlay's lower layer");
	close(tree);
	for (unsigned i = 0u; i < m->layer_count; i++) {
		char name[32];

		snprintf(name, sizeof name, "lower%u", i + 1u);
		if (mkdir(name, 0700) != 0 ||
		    move_mount(layers[i], "", AT_FDCWD, name, MOVE_MOUNT_F_EMPTY_PATH) != 0)
			child_fail(report, "a layer");
		close(layers[i]);
	}
	if (kept >= 0) {
		/* The user's own directory: its mode is its own, not the source's. */
		if (mkdir("kept", 0700) != 0 ||
		    move_mount(kept, "", AT_FDCWD, "kept", MOVE_MOUNT_F_EMPTY_PATH) != 0)
			child_fail(report, "the overlay's upper layer, kept");
		close(kept);
	} else if (chmod("upper", lower.st_mode & 07777) != 0) {
		/* No chown: the namespace maps the user and the user's group, to
		 * root, who made upper/, so the directory's owner and group are
		 * root's here already or are not mapped and cannot be given. */
		child_fail(report, "the overlay's upper layer");
	}
	/* What hull makes in the tree: its siblings' names and its resolver
	 * over /etc, and the volumes' guest paths. */
	if (m->hosts != NULL)
		stage_upper(m, upper_dir, "/etc/hosts", 1);
	if (m->resolver != NULL)
		stage_upper(m, upper_dir, "/etc/resolv.conf", 1);
	for (unsigned i = 0u; i < m->volume_count; i++)
		stage_upper(m, upper_dir, m->volume_guest[i], 0);
	/* The topmost lower layer first: the last of (layers ...), down to the
	 * source. */
	at = (size_t)snprintf(options, sizeof options, "lowerdir=");
	for (unsigned i = m->layer_count; i > 0u; i--)
		at += (size_t)snprintf(options + at, sizeof options - at, "lower%u:", i);
	snprintf(options + at, sizeof options - at, "lower,upperdir=%s,workdir=%s,userxattr",
	         upper_dir, work_dir);
	if (mount("overlay", "merged", "overlay", 0u, options) != 0)
		child_fail(report, "mounting the overlay");
}

/*
 * Close every descriptor from 3 up but those in `keep`. The init and the
 * exec helper are forked from the process running the engine and never
 * exec, so they would otherwise hold whatever it had open: its sockets,
 * its lock, and the far end of a stdin pipe, which then never reached
 * EOF. close_range() over the gaps allocates nothing, which matters in a
 * child forked from a threaded process.
 */
static void close_others(int *keep, unsigned n)
{
	unsigned from = 3u;

	for (unsigned i = 1u; i < n; i++)
		for (unsigned j = i; j > 0u && keep[j - 1u] > keep[j]; j--) {
			int t = keep[j];

			keep[j] = keep[j - 1u];
			keep[j - 1u] = t;
		}
	for (unsigned i = 0u; i < n; i++) {
		if (keep[i] < 3)
			continue;
		if ((unsigned)keep[i] > from)
			close_range(from, (unsigned)keep[i] - 1u, 0);
		from = (unsigned)keep[i] + 1u;
	}
	close_range(from, ~0u, 0);
}

static volatile sig_atomic_t pending;

static void hold(int signal)
{
	pending = signal;
}

/*
 * The machine's PID 1. Everything up to the workload's exec happens here,
 * so the workload inherits it; then the workload runs as PID 2 and this
 * waits. A signal forwarded is checked before each wait and on every
 * interrupted one: one that lands just before a wait blocks is forwarded
 * at the next wakeup, and stop's SIGKILL is the backstop either way.
 */
/* The init's relaying, past the workload's start. Static, since the init
 * is forked from a threaded process and must not allocate. */
static volatile sig_atomic_t child_ended;
static uint8_t               relay_held[HELD_LIMIT];
static size_t                relay_length;

static void note_child(int signal)
{
	(void)signal;
	child_ended = 1;
}

/* All of `n` bytes to `fd`, through EINTR, forwarding a pending signal
 * meanwhile. 0, or -1 once the core has gone. */
static int relay_write(int fd, const void *bytes, size_t n, pid_t workload)
{
	const uint8_t *at = bytes;

	while (n > 0u) {
		ssize_t wrote = write(fd, at, n);

		if (pending != 0) {
			kill(workload, (int)pending);
			pending = 0;
		}
		if (wrote < 0 && errno == EINTR)
			continue;
		if (wrote <= 0)
			return -1;
		at += wrote;
		n -= (size_t)wrote;
	}
	return 0;
}

static int send_frame(int fd, int kind, int which, const void *bytes, size_t n, pid_t workload)
{
	frame f = { (uint8_t)kind, (uint8_t)which, { 0u, 0u }, (uint32_t)n };

	return relay_write(fd, &f, sizeof f, workload) != 0 ||
	       relay_write(fd, bytes, n, workload) != 0 ? -1 : 0;
}

/* Held for a core to come: whole frames, the oldest dropped first. */
static void relay_hold(int which, const uint8_t *bytes, size_t n)
{
	frame  f = { 'd', (uint8_t)which, { 0u, 0u }, (uint32_t)n };
	size_t record = sizeof f + n;
	size_t at = 0u;

	if (record > HELD_LIMIT)
		return;
	while (relay_length - at + record > HELD_LIMIT) {
		frame old;

		memcpy(&old, relay_held + at, sizeof old);
		at += sizeof old + old.length;
	}
	memmove(relay_held, relay_held + at, relay_length - at);
	relay_length -= at;
	memcpy(relay_held + relay_length, &f, sizeof f);
	memcpy(relay_held + relay_length + sizeof f, bytes, n);
	relay_length += record;
}

/*
 * The init once the workload runs: the workload's pipes read into frames
 * to the core, until the workload is reaped and its pipes have nothing
 * more to give; then its end, and the init leaves, which ends the
 * namespace. With no core, as under keep after the core has gone, what
 * the pipes carry is held, and the end waits for a core: the listener's
 * next connection is the core taking the machine back, and is given what
 * was held first.
 */
/* (export PATH)'s file, opened before the pivot, so the user's, or -1. */
static int export_fd = -1;

/* (owners PATH)'s list, opened before the pivot too: given back in the
 * merged root, under "/" once the root is the namespace's. */
static int owners_fd = -1;

static int export_sink(void *context, const uint8_t *data, size_t n)
{
	(void)context;
	while (n > 0u) {
		ssize_t wrote = write(export_fd, data, n);

		if (wrote < 0 && errno == EINTR)
			continue;
		if (wrote <= 0)
			return -1;
		data += wrote;
		n -= (size_t)wrote;
	}
	return 0;
}

/* Seconds the image's own shutdown is given before the namespace is
 * ended regardless, as a vm's agent gives it (agent/agent.c). */
#define SHUTDOWN_GRACE 20

/* The name hull's init goes by once it is PID 1's child, which is how the
 * engine finds it to signal (signal_init). */
#define HANDED_NAME "hull-init"

static int handed;  /* this init is PID 1's child, the image's init PID 1 */

/*
 * (init PATH), as on a vm (section 4.11): PID 1 becomes the image's init
 * and this one goes on in a child of it, the reaper of what it starts,
 * relaying the workload as it did. PID 1's signals are given back and
 * every descriptor of this one's closed on its exec, so the image's init
 * holds nothing of hull's; should the exec fail, PID 1 reaps for ever
 * rather than end the machine under its workload. Before any handler is
 * set, so none is given to the image's init.
 */
static void hand_over(const hull_machine *m, int report)
{
	pid_t   agent;
	int     execed[2];
	char    failed;
	ssize_t got;

	if (access(m->init, X_OK) != 0)
		child_fail(report, "the machine has no init it can run at its (init ...) path");
	if (pipe2(execed, O_CLOEXEC) != 0)
		child_fail(report, "pipe");
	agent = bare_fork();
	if (agent < 0)
		child_fail(report, "fork");
	if (agent > 0) {
		char *const argv[] = { m->init, NULL };
		sigset_t    none;

		close(execed[0]);
		sigemptyset(&none);
		sigprocmask(SIG_SETMASK, &none, NULL);
		syscall(SYS_close_range, 3u, ~0u, CLOSE_RANGE_CLOEXEC);
		execv(m->init, argv);
		if (write(execed[1], "x", 1u) != 1) {
			/* hull's init reads the pipe's end alone, and goes on */
		}
		close(execed[1]);
		for (;;)
			if (wait(NULL) < 0)
				pause();
	}
	/* Not before the image's init is PID 1: its exec closes the pipe,
	 * and a failed one says so on it. Without the wait the workload ran
	 * while PID 1 was still hull's, and saw it there. */
	close(execed[1]);
	while ((got = read(execed[0], &failed, 1u)) < 0 && errno == EINTR)
		;
	close(execed[0]);
	if (got > 0)
		child_fail(report, "the image's init could not be run as PID 1");
	handed = 1;
	prctl(PR_SET_CHILD_SUBREAPER, 1L, 0L, 0L, 0L);
	prctl(PR_SET_NAME, HANDED_NAME, 0L, 0L, 0L);
}

/*
 * The workload has ended and said so: the machine goes. Handed over, the
 * image's own shutdown is asked for, /sbin/poweroff, which every init of
 * note answers, so what it started ends as it would; after the grace the
 * namespace is ended regardless, reboot() from inside a PID namespace
 * ending its PID 1.
 */
static void finish(void)
{
	if (handed) {
		pid_t asked = bare_fork();

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
		reboot(RB_POWER_OFF);
	}
	_exit(0);
}

/* The merged root as engine/tree.c's tar, its own mount alone, so no
 * /proc, volume or secret, each owner as this namespace sees it, which
 * is what keeps an owner of the subordinate range off the user's disk.
 * A failure empties the file, which no commit takes, says why on the
 * workload's stderr, and makes the end 125. */
static void export_tree(int conn, int *status, pid_t workload)
{
	char detail[512] = "";

	if (export_fd < 0)
		return;
	if (hull_tree_tar_fs("/", 1, export_sink, NULL, detail, sizeof detail) != 0 ||
	    fsync(export_fd) != 0) {
		char said[600];
		int  n = snprintf(said, sizeof said, "hull: export: %s\n",
		                  detail[0] != '\0' ? detail : strerror(errno));

		if (ftruncate(export_fd, 0) != 0) {
			/* nothing more can be done; the status says it */
		}
		if (conn >= 0 && n > 0)
			send_frame(conn, 'd', 2, said, (size_t)n, workload);
		*status = 125 << 8;
	}
	close(export_fd);
	export_fd = -1;
}

static void relay(int conn, int listener, int out, int err, pid_t workload)
{
	uint8_t buffer[16384];
	int     status = 0;
	int     ended = 0;

	for (;;) {
		struct pollfd fds[4] = { { out, POLLIN, 0 }, { err, POLLIN, 0 },
		                         { conn, POLLIN, 0 }, { conn < 0 ? listener : -1, POLLIN, 0 } };
		int           which;
		pid_t         reaped;
		int           s;

		if (pending != 0) {
			kill(workload, (int)pending);
			pending = 0;
		}
		child_ended = 0;
		while ((reaped = waitpid(-1, &s, WNOHANG)) > 0)
			if (reaped == workload) {
				status = s;
				ended = 1;
			}
		/* Ended, and its pipes drained or held open only by an orphan,
		 * which the namespace's end takes: the end is said. */
		if (ended) {
			int idle = poll(fds, 2u, 0) == 0 || (out < 0 && err < 0);

			if (idle)
				export_tree(conn, &status, workload);
			if (idle && conn >= 0 &&
			    send_frame(conn, 'x', 0, &status, sizeof status, workload) == 0)
				finish();
			if (idle && listener < 0)
				finish();
		}
		if (poll(fds, 4u, ended ? 200 : -1) < 0) {
			if (errno == EINTR)
				continue;
			_exit(126);
		}
		for (which = 1; which <= 2; which++) {
			struct pollfd *f = &fds[which - 1];
			int           *fd = which == 1 ? &out : &err;
			ssize_t        n;

			if (*fd < 0 || f->revents == 0)
				continue;
			n = read(*fd, buffer, sizeof buffer);
			if (n < 0 && errno == EINTR)
				continue;
			if (n <= 0) {
				close(*fd);
				*fd = -1;
				continue;
			}
			if (conn >= 0 && send_frame(conn, 'd', which, buffer, (size_t)n, workload) != 0) {
				close(conn);
				conn = -1;
			}
			if (conn < 0 && listener >= 0)
				relay_hold(which, buffer, (size_t)n);
		}
		/* The core says nothing on this socket: readable is its end. */
		if (conn >= 0 && fds[2].revents != 0 && read(conn, buffer, sizeof buffer) <= 0) {
			close(conn);
			conn = -1;
		}
		if (conn < 0 && listener >= 0 && fds[3].revents != 0) {
			conn = accept4(listener, NULL, NULL, SOCK_CLOEXEC);
			if (conn >= 0 && relay_write(conn, relay_held, relay_length, workload) != 0) {
				close(conn);
				conn = -1;
			} else if (conn >= 0) {
				relay_length = 0u;
			}
		}
		if (conn < 0 && listener < 0 && out < 0 && err < 0 && !ended)
			continue;  /* the core went and nothing is kept: the death signal comes */
	}
}

/*
 * Frames between `tap`, a NIC's, and `port`, its segment's port on the
 * core's switch, which frames each after its length as 32 bits in network
 * order, as qemu's -netdev stream does: in a process of the machine's own,
 * which goes with its namespace. Returns when a port closes.
 */
static void relay_frames(const int *tap, const int *port, unsigned n)
{
	static uint8_t from_port[MAX_SEGMENTS][4u + 65536u];
	size_t         held[MAX_SEGMENTS] = { 0u };
	uint8_t        frame[4u + 65536u];
	struct pollfd  fds[2u * MAX_SEGMENTS];

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

/* How deep and how far unmapped_owner() looks: a tree's own top, where
 * its owners are set, without reading a whole distribution's files. */
#define OWNER_DEPTH   3u
#define OWNER_ENTRIES 20000u

/* Under the directory `fd`, its path `path`, the first entry -- itself,
 * then what it holds to OWNER_DEPTH -- whose owner or group the machine
 * cannot map (hull_id_mapped()), the tree's top aside: 1 with its path, the id and which kind
 * in `found`, `id` and `group`; 0 where none is. `fd` is closed. */
static int unmapped_owner(int fd, char *path, size_t cap, unsigned depth, unsigned *seen,
                          uid_t uid, gid_t gid, int many, unsigned long *id, int *group)
{
	struct stat    self;
	DIR           *dir;
	struct dirent *entry;
	size_t         length = strlen(path);
	int            found = 0;

	if (fstat(fd, &self) != 0) {
		close(fd);
		return 0;
	}
	/* Not the tree's own top, whose upper is hull's, never copied up. */
	if (depth != 0u && (!hull_id_mapped(0, self.st_uid, uid, gid, many) ||
	                    !hull_id_mapped(1, self.st_gid, uid, gid, many))) {
		*group = hull_id_mapped(0, self.st_uid, uid, gid, many);
		*id    = *group ? (unsigned long)self.st_gid : (unsigned long)self.st_uid;
		close(fd);
		return 1;
	}
	if (depth >= OWNER_DEPTH || (dir = fdopendir(fd)) == NULL) {
		close(fd);
		return 0;
	}
	while (!found && *seen < OWNER_ENTRIES && (entry = readdir(dir)) != NULL) {
		struct stat each;

		if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
			continue;
		(*seen)++;
		if (fstatat(dirfd(dir), entry->d_name, &each, AT_SYMLINK_NOFOLLOW) != 0)
			continue;
		snprintf(path + length, cap - length, "%s%s", length > 1u ? "/" : "", entry->d_name);
		if (S_ISDIR(each.st_mode)) {
			int sub = openat(dirfd(dir), entry->d_name,
			                 O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);

			if (sub >= 0)
				found = unmapped_owner(sub, path, cap, depth + 1u, seen, uid, gid, many,
				                       id, group);
		} else if (!hull_id_mapped(0, each.st_uid, uid, gid, many) ||
		           !hull_id_mapped(1, each.st_gid, uid, gid, many)) {
			*group = hull_id_mapped(0, each.st_uid, uid, gid, many);
			*id    = *group ? (unsigned long)each.st_gid : (unsigned long)each.st_uid;
			found  = 1;
		}
		if (!found)
			path[length] = '\0';
	}
	closedir(dir);
	return found;
}

/*
 * What a machine above an overlay is told of its tree, into `out`, or ""
 * : the first owner in the source or a layer the machine cannot map,
 * which the overlay refuses to copy up, so that a write there fails with
 * EOVERFLOW, "Value too large for defined data type", and nothing says
 * why. Found in this workspace's own trees, whose directories are the
 * group users and setgid, where the user's primary group is its own. The
 * line in /etc/subuid or /etc/subgid that would map the id is named;
 * hull maps what such a line grants as itself (hull_map_many()).
 */
static void owner_notice(const hull_machine *m, uid_t uid, gid_t gid, char *out, size_t cap)
{
	const char *trees[1u + MAX_VOLUMES];
	unsigned    count = 0u;

	out[0] = '\0';
	if (!m->overlay)
		return;
	trees[count++] = m->root;
	for (unsigned i = 0u; i < m->layer_count; i++)
		trees[count++] = m->layer_path[i];
	for (unsigned i = 0u; i < count; i++) {
		char           path[4096] = "/";
		unsigned       seen = 0u;
		unsigned long  id = 0u;
		int            group = 0;
		int            fd = trees[i] != NULL ?
		                    open(trees[i], O_RDONLY | O_DIRECTORY | O_CLOEXEC) : -1;
		struct passwd *me = getpwuid(uid);

		if (fd < 0 ||
		    !unmapped_owner(fd, path, sizeof path, 0u, &seen, uid, gid, m->many, &id, &group))
			continue;
		if (id == 0u)
			snprintf(out, cap, "hull: %s %s owns %.400s in the tree, the host's root, which no "
			         "machine of a user's maps: a write there under (mutable overlay) fails, "
			         "Value too large for defined data type\n", group ? "group" : "user",
			         "0", path);
		else
			snprintf(out, cap, "hull: %s %lu owns %.400s in the tree and is not mapped into the "
			         "machine: a write there under (mutable overlay) fails, Value too large "
			         "for defined data type; the line `%s:%lu:1` in %s maps it, a "
			         "one-time setting for the host's owner\n", group ? "group" : "user", id,
			         path, me != NULL ? me->pw_name : "USER", id,
			         group ? "/etc/subgid" : "/etc/subuid");
		return;
	}
}

/*
 * The machine's NICs on its segments, eth0, eth1, ..., each a tap in its
 * own network namespace with the MAC hull_access_segment_mac() gives it,
 * left down, as a vm's are until its workload brings them up; and the
 * relay between them and their ports, forked here. The init keeps neither.
 */
static void join_segments(const hull_machine *m, int report)
{
	int   tap[MAX_SEGMENTS];
	pid_t relayer;
	int   routed = m->nat || m->bridge;   /* a default route already, its own */
	int   routed6 = routed;

	for (unsigned i = 0u; i < m->segment_count; i++) {
		struct ifreq request = { 0 };
		int          fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);

		/* After pasta's eth0 under NAT, as after passt's on a vm. */
		snprintf(request.ifr_name, sizeof request.ifr_name, "eth%u",
		         i + (m->nat || m->bridge ? 1u : 0u));
		request.ifr_flags = IFF_TAP | IFF_NO_PI;
		if (fd < 0 || (tap[i] = open("/dev/net/tun", O_RDWR | O_CLOEXEC)) < 0 ||
		    ioctl(tap[i], TUNSETIFF, &request) != 0)
			child_fail(report, "a segment's NIC");
		request.ifr_hwaddr.sa_family = 1; /* ARPHRD_ETHER */
		hull_access_segment_mac(m->address_text, m->segment[i], i,
		                        (unsigned char *)request.ifr_hwaddr.sa_data);
		if (ioctl(fd, SIOCSIFHWADDR, &request) != 0)
			child_fail(report, "a segment's NIC's address");
		/* Hull's address, where it addresses the segment, and the NIC up:
		 * the segment owns it, as it names it to the siblings. */
		if (m->segment_address[i][0] != '\0') {
			struct sockaddr_in *at = (struct sockaddr_in *)&request.ifr_addr;
			uint32_t            mask = 0xffffffffu << (32u - m->segment_prefix[i]);

			memset(&request.ifr_addr, 0, sizeof request.ifr_addr);
			at->sin_family = AF_INET;
			if (inet_pton(AF_INET, m->segment_address[i], &at->sin_addr) != 1 ||
			    ioctl(fd, SIOCSIFADDR, &request) != 0)
				child_fail(report, "a segment's NIC's IPv4 address");
			at->sin_addr.s_addr = htonl(mask);
			if (ioctl(fd, SIOCSIFNETMASK, &request) != 0)
				child_fail(report, "a segment's NIC's netmask");
		}
		/* Up where hull addresses it, by either family or both. */
		if (m->segment_address[i][0] != '\0' || m->segment_address6[i][0] != '\0') {
			if (ioctl(fd, SIOCGIFFLAGS, &request) != 0)
				child_fail(report, "a segment's NIC's flags");
			request.ifr_flags |= IFF_UP;
			if (ioctl(fd, SIOCSIFFLAGS, &request) != 0)
				child_fail(report, "a segment's NIC up");
			if (m->segment_address6[i][0] != '\0' &&
			    hull_ip6_add(request.ifr_name, m->segment_address6[i], m->segment_prefix6[i]) != 0)
				child_fail(report, "a segment's NIC's IPv6 address");
		}
		/* The first segment whose .1 is a gateway is the way out of a
		 * machine with none of its own (Appendix D), and the first whose
		 * ::1 is, by IPv6. */
		if (!routed6 && m->segment_gateway[i] && m->segment_address6[i][0] != '\0') {
			if (hull_ip6_route(request.ifr_name, m->segment_address6[i],
			                   m->segment_prefix6[i]) != 0)
				child_fail(report, "a segment's IPv6 route out through its ::1");
			routed6 = 1;
		}
		if (!routed && m->segment_gateway[i] && m->segment_address[i][0] != '\0') {
			struct rtentry      route = { 0 };
			struct sockaddr_in *gateway = (struct sockaddr_in *)&route.rt_gateway;
			uint32_t            mask = 0xffffffffu << (32u - m->segment_prefix[i]);
			struct in_addr      address;

			inet_pton(AF_INET, m->segment_address[i], &address);
			((struct sockaddr_in *)&route.rt_dst)->sin_family     = AF_INET;
			((struct sockaddr_in *)&route.rt_genmask)->sin_family = AF_INET;
			gateway->sin_family      = AF_INET;
			gateway->sin_addr.s_addr = htonl((ntohl(address.s_addr) & mask) + 1u);
			route.rt_flags = RTF_UP | RTF_GATEWAY;
			route.rt_dev   = request.ifr_name;
			if (ioctl(fd, SIOCADDRT, &route) != 0)
				child_fail(report, "a segment's route out through its .1");
			routed = 1;
		}
		close(fd);
	}
	relayer = bare_fork();
	if (relayer < 0)
		child_fail(report, "the segments' relay");
	if (relayer == 0) {
		int keep[2u * MAX_SEGMENTS];

		for (unsigned i = 0u; i < m->segment_count; i++) {
			keep[2u * i] = tap[i];
			keep[2u * i + 1u] = m->segment_fd[i];
		}
		close_others(keep, 2u * m->segment_count);
		relay_frames(tap, m->segment_fd, m->segment_count);
		_exit(0);
	}
	for (unsigned i = 0u; i < m->segment_count; i++) {
		close(tap[i]);
		close(m->segment_fd[i]);
	}
}

static void run_init(const hull_machine *m, uid_t uid, gid_t gid, int conn, int dir_fd,
                     int report, int mapped)
{
	static const int forwarded[] = { SIGTERM, SIGINT, SIGHUP, SIGQUIT, SIGUSR1, SIGUSR2 };
	struct sigaction action;
	struct stat      st;
	char             map[64];
	char             step[320];
	int              volumes[MAX_VOLUMES];
	int              secrets = -1;
	int              ssh_tree = -1;
	int              hosts_tree = -1;
	int              resolver_tree = -1;
	const char      *ssh_bin = NULL;  /* the image's sshd, once found after the pivot */
	int              null = open("/dev/null", O_RDWR | O_CLOEXEC);
	int              out[2];
	int              err[2];
	int              listener = -1;
	pid_t            workload;

	/* The init's own stdio is nothing; the workload's are pipes of its. */
	if (null < 0 || dup2(null, 0) < 0 || dup2(null, 1) < 0 || dup2(null, 2) < 0)
		child_fail(report, "stdio");
	{
		int      keep[4u + MAX_SEGMENTS] = { report, conn, mapped };
		unsigned n = 3u;

		if (dir_fd >= 0)
			keep[n++] = dir_fd;
		for (unsigned i = 0u; i < m->segment_count; i++)
			keep[n++] = m->segment_fd[i];
		close_others(keep, n);
	}
	if (pipe2(out, O_CLOEXEC) != 0 || pipe2(err, O_CLOEXEC) != 0)
		child_fail(report, "the workload's pipes");
	/* The machine dies with the process that runs its engine, the core,
	 * unless kept: then it listens, on init.sock in its directory, for
	 * the core that takes it back. The init binds it, so that the peer a
	 * connection names is the init. The directory is the user's, 0700,
	 * so no one else can connect. */
	if (!m->keep && prctl(PR_SET_PDEATHSIG, SIGKILL, 0L, 0L, 0L) != 0)
		child_fail(report, "the parent-death signal");
	if (m->keep) {
		struct sockaddr_un at = { .sun_family = AF_UNIX };

		snprintf(at.sun_path, sizeof at.sun_path, "/proc/self/fd/%d/init.sock", dir_fd);
		listener = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
		if (listener < 0 || bind(listener, (struct sockaddr *)&at, sizeof at) != 0 ||
		    listen(listener, 1) != 0)
			child_fail(report, "init.sock");
		close(dir_fd);
	}
	/* A session of its own, so a terminal's Ctrl-C reaches hull and not the
	 * machine behind its back: only stop() signals a machine. */
	if (setsid() < 0)
		child_fail(report, "setsid");
	/* The engine says whether it mapped many uids from outside, `M`, or the
	 * machine is to map its one, `S`, as a process may itself; or `O`, one
	 * where many were wanted and the host gives the user no range, which
	 * is said, with its way round, once, on the workload's stderr. */
	{
		char said = 0;

		char    notice[1024];
		ssize_t told = 0;
		ssize_t n;

		if (read(mapped, &said, 1u) != 1)
			child_fail(report, "the uid map");
		while (told < (ssize_t)sizeof notice &&
		       (n = read(mapped, notice + told, sizeof notice - (size_t)told)) > 0)
			told += n;
		close(mapped);
		if (told > 0 && write(err[1], notice, (size_t)told) < 0) {
			/* the workload's stderr is gone; nothing to tell */
		}
		if (said == 'O') {
			static const char one[] =
			    "hull: one uid is mapped, the host giving this user no subordinate range: "
			    "a chown to another of the machine's users fails, and apt-get wants "
			    "-o APT::Sandbox::User=root; hull doctor gives the line that adds a range\n";

			if (write(err[1], one, sizeof one - 1u) < 0) {
				/* the workload's stderr is gone; nothing to tell */
			}
		}
		if (said == 'M') {
			/* The user's own groups, unmapped here and so nobody's, go:
			 * by the system call, not glibc's setgroups(), which tells
			 * every thread glibc knows of and waits for each to answer.
			 * This child is a raw clone of the multithreaded core, so
			 * glibc still lists the core's threads -- the switch's among
			 * them -- which are not here to answer, and it waited for
			 * ever: found 2026-10-27, a core hung making a machine. */
			if (syscall(SYS_setgroups, 0, NULL) != 0)
				child_fail(report, "setgroups");
		} else {
			snprintf(map, sizeof map, "0 %u 1", (unsigned)uid);
			if (write_file("/proc/self/setgroups", "deny") != 0 ||
			    write_file("/proc/self/uid_map", map) != 0)
				child_fail(report, "uid_map");
			snprintf(map, sizeof map, "0 %u 1", (unsigned)gid);
			if (write_file("/proc/self/gid_map", map) != 0)
				child_fail(report, "gid_map");
		}
	}
	if (mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL) != 0)
		child_fail(report, "making mounts private");
	if (!m->overlay && mount(m->root, m->root, NULL, MS_BIND | MS_REC, NULL) != 0)
		child_fail(report, "binding the root");
	if (m->readonly_root &&
	    mount(NULL, m->root, NULL, MS_BIND | MS_REMOUNT | MS_RDONLY | locked_flags(m->root),
	          NULL) != 0)
		child_fail(report, "making the root read-only");
	/* A volume that is a stack is staged in the machine's directory, on a
	 * tmpfs of this namespace's, so that on the host it stays empty; the
	 * stack holds its own clones of the layers and the upper, and so
	 * outlives the old root it is staged under. */
	if ((m->staged || m->hosts != NULL || m->resolver != NULL) &&
	    mount("tmpfs", m->dir, "tmpfs", 0u, "mode=0700") != 0)
		child_fail(report, "the volumes' staging");
	/* The names of its addressed segments, and hull at their .1 to ask,
	 * staged beside the volumes and placed over /etc/hosts and
	 * /etc/resolv.conf once the root is the machine's: the tree is not
	 * written, so a read-only or stored root takes them too. */
	if (m->hosts != NULL)
		hosts_tree = stage_named(m->dir, "hosts", m->hosts, report);
	if (m->resolver != NULL)
		resolver_tree = stage_named(m->dir, "resolv.conf", m->resolver, report);
	for (unsigned i = 0u; i < m->volume_count; i++) {
		struct mount_attr readonly = { .attr_set = MOUNT_ATTR_RDONLY };
		char              staged[4300];

		snprintf(step, sizeof step, "volume %s", m->volume_stack[i].count > 0u ?
		         m->volume_guest[i] : m->volume_host[i]);
		if (m->volume_stack[i].count > 0u) {
			const char *why;

			snprintf(staged, sizeof staged, "%s/v%u", m->dir, i);
			if (mkdir(staged, 0700) != 0)
				child_fail(report, step);
			if ((why = hull_stack_stage(&m->volume_stack[i], staged)) != NULL) {
				snprintf(step, sizeof step, "volume %s: %s", m->volume_guest[i], why);
				child_fail(report, step);
			}
			snprintf(staged, sizeof staged, "%s/v%u/merged", m->dir, i);
		}
		volumes[i] = open_tree(AT_FDCWD, m->volume_stack[i].count > 0u ? staged :
		                                                                m->volume_host[i],
		                       OPEN_TREE_CLONE | OPEN_TREE_CLOEXEC | AT_RECURSIVE);
		if (volumes[i] < 0)
			child_fail(report, step);
		if (m->volume_readonly[i] &&
		    mount_setattr(volumes[i], "", AT_EMPTY_PATH | AT_RECURSIVE, &readonly,
		                  sizeof readonly) != 0)
			child_fail(report, step);
	}
	if (m->secrets != NULL) {
		struct mount_attr readonly = { .attr_set = MOUNT_ATTR_RDONLY |
		                                           MOUNT_ATTR_NOSUID | MOUNT_ATTR_NODEV |
		                                           MOUNT_ATTR_NOEXEC };

		secrets = open_tree(AT_FDCWD, m->secrets, OPEN_TREE_CLONE | OPEN_TREE_CLOEXEC);
		if (secrets < 0 ||
		    mount_setattr(secrets, "", AT_EMPTY_PATH, &readonly, sizeof readonly) != 0)
			child_fail(report, "the secrets");
	}
	if (m->ssh_dir != NULL) {
		struct mount_attr readonly = { .attr_set = MOUNT_ATTR_RDONLY |
		                                           MOUNT_ATTR_NOSUID | MOUNT_ATTR_NODEV |
		                                           MOUNT_ATTR_NOEXEC };

		ssh_tree = open_tree(AT_FDCWD, m->ssh_dir, OPEN_TREE_CLONE | OPEN_TREE_CLOEXEC);
		if (ssh_tree < 0 ||
		    mount_setattr(ssh_tree, "", AT_EMPTY_PATH, &readonly, sizeof readonly) != 0)
			child_fail(report, "the ssh material");
	}
	if (m->hostname != NULL && sethostname(m->hostname, strlen(m->hostname)) != 0)
		child_fail(report, "sethostname");
	/* A namespace of its own starts with lo down: up, so that a dial to a
	 * port nothing holds is refused rather than unreachable. */
	if (m->no_network || m->nat || m->bridge) {
		struct ifreq lo = { 0 };
		int          fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);

		snprintf(lo.ifr_name, sizeof lo.ifr_name, "lo");
		if (fd < 0 || ioctl(fd, SIOCGIFFLAGS, &lo) != 0)
			child_fail(report, "lo");
		lo.ifr_flags |= IFF_UP;
		if (ioctl(fd, SIOCSIFFLAGS, &lo) != 0)
			child_fail(report, "lo");
		close(fd);
	}
	/* On a bridge the core has put eth0 here already: up, and on hull's own
	 * given its address and the gateway as its route out; on one its owner
	 * named, its addressing is the workload's, as a bare segment's is. */
	if (m->bridge) {
		struct ifreq request = { 0 };
		int          fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);

		snprintf(request.ifr_name, sizeof request.ifr_name, "eth0");
		if (m->bridge_own) {
			struct sockaddr_in *at = (struct sockaddr_in *)&request.ifr_addr;

			at->sin_family = AF_INET;
			if (fd < 0 || inet_pton(AF_INET, m->bridge_address, &at->sin_addr) != 1 ||
			    ioctl(fd, SIOCSIFADDR, &request) != 0)
				child_fail(report, "the bridge's eth0 address");
			at->sin_addr.s_addr = htonl(0xffffffffu << (32u - BRIDGE_PREFIX));
			if (ioctl(fd, SIOCSIFNETMASK, &request) != 0)
				child_fail(report, "the bridge's eth0 netmask");
		}
		if (fd < 0 || ioctl(fd, SIOCGIFFLAGS, &request) != 0)
			child_fail(report, "the bridge's eth0");
		request.ifr_flags |= IFF_UP;
		if (ioctl(fd, SIOCSIFFLAGS, &request) != 0)
			child_fail(report, "the bridge's eth0 up");
		if (m->bridge_own) {
			struct rtentry      route = { 0 };
			struct sockaddr_in *gateway = (struct sockaddr_in *)&route.rt_gateway;

			((struct sockaddr_in *)&route.rt_dst)->sin_family     = AF_INET;
			((struct sockaddr_in *)&route.rt_genmask)->sin_family = AF_INET;
			gateway->sin_family = AF_INET;
			inet_pton(AF_INET, BRIDGE_GATEWAY, &gateway->sin_addr);
			route.rt_flags = RTF_UP | RTF_GATEWAY;
			route.rt_dev   = request.ifr_name;
			if (ioctl(fd, SIOCADDRT, &route) != 0)
				child_fail(report, "the bridge's route out");
		}
		close(fd);
	}
	if (m->segment_count != 0u)
		join_segments(m, report);
	/* The export's file, while the host's paths are still to be seen; the
	 * user's, as the namespace's root is, and no file of the workload's. */
	if (m->export != NULL &&
	    (export_fd = open(m->export, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC,
	                      0600)) < 0)
		child_fail(report, "the export's file");
	if (m->owners != NULL &&
	    (owners_fd = open(m->owners, O_RDONLY | O_NOFOLLOW | O_CLOEXEC)) < 0)
		child_fail(report, "the owners' file");
	if (m->overlay)
		layer(m, report);
	/* Its root becomes the mount namespace's own, by pivot_root and not
	 * chroot(2): the kernel refuses a new user namespace to a process
	 * whose root is not its namespace's, so in a chroot `unshare -rn`
	 * failed, which netcfgd's live scripts and any nested rootless tool
	 * need. The old root is stacked on the new and detached. */
	{
		struct stat now;
		struct stat new_root;

		if (chdir(m->overlay ? "merged" : m->root) != 0 || stat("/", &now) != 0 ||
		    stat(".", &new_root) != 0)
			child_fail(report, "the machine's root");
		/* This namespace's processes, where the dir has room for them:
		 * nothing is created in the user's dir. Mounted before the old
		 * root goes, since a user namespace may mount a proc only while
		 * another is wholly visible to it. */
		if (stat("proc", &st) == 0 && S_ISDIR(st.st_mode) &&
		    mount("proc", "proc", "proc", MS_NOSUID | MS_NODEV | MS_NOEXEC, NULL) != 0)
			child_fail(report, "mounting /proc");
		/* A foreign arch: binfmt_misc of the machine's own user namespace
		 * (Linux 6.7), on its /proc where a program looks for it, given
		 * qemu-user while the host's binary is still to be seen; F opens
		 * it now, so it runs in the machine's root. The mount holds the
		 * registration, which goes with it, and nothing outside sees it. */
		if (m->binfmt[0] != '\0') {
			int fd;

			if (stat("proc", &st) != 0 || !S_ISDIR(st.st_mode)) {
				errno = ENOENT;
				child_fail(report, "a foreign arch registers its emulator on the tree's /proc, "
				                   "and it has none");
			}
			if (mount("binfmt_misc", "proc/sys/fs/binfmt_misc", "binfmt_misc",
			          MS_NOSUID | MS_NODEV | MS_NOEXEC, NULL) != 0)
				child_fail(report, "binfmt_misc in the machine's user namespace (Linux 6.7 and "
				                   "later)");
			fd = open("proc/sys/fs/binfmt_misc/register", O_WRONLY | O_CLOEXEC);
			if (fd < 0 || write(fd, m->binfmt, strlen(m->binfmt)) != (ssize_t)strlen(m->binfmt))
				child_fail(report, "registering the foreign arch's emulator");
			close(fd);
		}
		/* And its devices; a machine of the host's own / keeps the
		 * host's /dev, which is already its tree's. */
		if (!(now.st_dev == new_root.st_dev && now.st_ino == new_root.st_ino) &&
		    stat("dev", &st) == 0 && S_ISDIR(st.st_mode))
			make_dev(m, report);
		/* Devices with nowhere to go are refused, not dropped. */
		else if (!(now.st_dev == new_root.st_dev && now.st_ino == new_root.st_ino) &&
		         m->device_count != 0u)
			child_fail(report, "(devices ...) are bound into the machine's /dev, and its tree has "
			                   "no /dev");
		/* A machine of the host's own / has nothing to pivot to: its root
		 * is the bind stacked over /, which it is put on, so that its root
		 * is still the namespace's. */
		if (now.st_dev == new_root.st_dev && now.st_ino == new_root.st_ino ?
		        chdir("/") != 0 || chroot(".") != 0 :
		        syscall(SYS_pivot_root, ".", ".") != 0 || umount2(".", MNT_DETACH) != 0)
			child_fail(report, "pivot_root");
		if (chdir("/") != 0)
			child_fail(report, "the machine's root");
	}
	if (owners_fd >= 0) {
		hull_give_owners(owners_fd, "/", err[1]);
		owners_fd = -1;
	}
	for (unsigned i = 0u; i < m->volume_count; i++) {
		struct stat place;

		snprintf(step, sizeof step, "volume %s", m->volume_guest[i]);
		/* A guest path that is not there is made where the root's writes
		 * are the overlay's; in a dir used in place, or a read-only root,
		 * it would be made in the user's own tree, so it is refused,
		 * naming it as the guest path. */
		if (stat(m->volume_guest[i], &place) != 0 && errno == ENOENT) {
			if (!m->overlay) {
				snprintf(step, sizeof step, "volume %s: the guest path is not in the machine, "
				         "and is made only above an overlay", m->volume_guest[i]);
				errno = ENOENT;
				child_fail(report, step);
			}
			if (make_path(m->volume_guest[i]) != 0)
				child_fail(report, step);
		}
		/* A symlink at the guest path is followed, inside the machine. */
		if (move_mount(volumes[i], "", AT_FDCWD, m->volume_guest[i],
		               MOVE_MOUNT_F_EMPTY_PATH | MOVE_MOUNT_T_SYMLINKS) != 0)
			child_fail(report, step);
	}
	/* Its siblings' names over /etc/hosts, and hull's .1 over
	 * /etc/resolv.conf, made where the root's writes are the overlay's,
	 * and refused where they would be the user's own. */
	if (hosts_tree >= 0)
		place_named(hosts_tree, "/etc/hosts", m->overlay, 1, report);
	/* Under NAT a tree used in place with no resolv.conf keeps none, as
	 * docker's run does not write one into a bind: the machine has its
	 * network, and only its names are the host's resolver's to give. */
	if (resolver_tree >= 0)
		place_named(resolver_tree, "/etc/resolv.conf", m->overlay, !m->nat && !m->bridge, report);
	/* The secrets at /run/hull/secrets, J.3's place, on a tmpfs at /run:
	 * the dir's /run, which FHS has emptied at every boot, is hidden, and
	 * nothing is created in the user's dir. A dir with no /run cannot
	 * have them. */
	if (secrets >= 0 || ssh_tree >= 0) {
		/* A tmpfs at /run for the material, hiding the dir's own /run (FHS
		 * empties it each boot); a dir with no /run cannot have either. */
		if (mount("tmpfs", "/run", "tmpfs", MS_NOSUID | MS_NODEV, "mode=0755,size=1m") != 0)
			child_fail(report, "a tmpfs at /run, which the dir must have");
		if (mkdir("/run/hull", 0755) != 0)
			child_fail(report, "/run/hull");
	}
	if (secrets >= 0) {
		if (mkdir("/run/hull/secrets", 0700) != 0 ||
		    move_mount(secrets, "", AT_FDCWD, "/run/hull/secrets", MOVE_MOUNT_F_EMPTY_PATH) != 0)
			child_fail(report, "the secrets");
	}
	if (ssh_tree >= 0) {
		/* The host key, authorized_keys and the core's (qemu-shaped) config
		 * at /run/hull/ssh; a config of chroot's own beside them, binding
		 * the machine's own 127.107 address on a high port -- the chroot
		 * user is unprivileged on the shared host net, so not port 22
		 * (section 8.1). The image's sshd, found at the usual place, serves
		 * it; absent, (ssh true) is refused here rather than run without one. */
		static const char *const where[] = { "/usr/sbin/sshd", "/usr/bin/sshd", NULL };
		char  config[1024];
		int   fd;
		int   n;
		unsigned i;

		if (mkdir("/run/hull/ssh", 0700) != 0 ||
		    move_mount(ssh_tree, "", AT_FDCWD, "/run/hull/ssh", MOVE_MOUNT_F_EMPTY_PATH) != 0)
			child_fail(report, "the ssh material");
		n = snprintf(config, sizeof config,
		             "Port %d\nListenAddress %s\nHostKey /run/hull/ssh/host_key\n"
		             "AuthorizedKeysFile /run/hull/ssh/authorized_keys\n"
		             "PasswordAuthentication no\nPermitRootLogin prohibit-password\nUsePAM no\n",
		             HULL_SSH_PORT, m->nat || m->bridge ? "0.0.0.0" : m->address_text);
		fd = open("/run/hull/ssh.conf", O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0400);
		if (n <= 0 || n >= (int)sizeof config || fd < 0 || write(fd, config, (size_t)n) != n)
			child_fail(report, "the ssh config");
		close(fd);
		for (i = 0u; where[i] != NULL && access(where[i], X_OK) != 0; i++)
			;
		if (where[i] == NULL)
			child_fail(report, "(ssh true): the image has no sshd at /usr/sbin/sshd or /usr/bin/sshd");
		ssh_bin = where[i];
	}
	if (chdir(m->workdir) != 0)
		child_fail(report, "workdir");
	if (m->no_new_privs && prctl(PR_SET_NO_NEW_PRIVS, 1L, 0L, 0L, 0L) != 0)
		child_fail(report, "no_new_privs");
	if (m->init != NULL)
		hand_over(m, report);

	memset(&action, 0, sizeof action);
	action.sa_handler = hold;
	for (unsigned i = 0u; i < sizeof forwarded / sizeof forwarded[0]; i++)
		sigaction(forwarded[i], &action, NULL);
	action.sa_handler = note_child;
	sigaction(SIGCHLD, &action, NULL);
	action.sa_handler = SIG_IGN;
	sigaction(SIGPIPE, &action, NULL);
	/* The image's sshd beside the workload: a child the init reaps like any
	 * other, so its exit does not end the machine (only the workload's does,
	 * in relay). It binds the machine's address on HULL_SSH_PORT per its
	 * config; its own stdio goes nowhere, since it is supervised, not a
	 * stream. */
	if (ssh_bin != NULL) {
		pid_t sshd = bare_fork();

		if (sshd == 0) {
			action.sa_handler = SIG_DFL;
			for (unsigned i = 0u; i < sizeof forwarded / sizeof forwarded[0]; i++)
				sigaction(forwarded[i], &action, NULL);
			sigaction(SIGCHLD, &action, NULL);
			sigaction(SIGPIPE, &action, NULL);
			/* Its own session, so a signal to the init's foreground group
			 * does not reach it; its stdio is the init's /dev/null, inherited
			 * (the init dups it there above and close_others spares 0-2), so
			 * the -e logs go nowhere, as a supervised child's should. */
			if (setsid() < 0)
				_exit(126);
			execl(ssh_bin, "sshd", "-D", "-e", "-f", "/run/hull/ssh.conf", (char *)NULL);
			_exit(127);
		}
	}
	workload = bare_fork();
	if (workload < 0)
		child_fail(report, "fork");
	if (workload == 0) {
		action.sa_handler = SIG_DFL;
		for (unsigned i = 0u; i < sizeof forwarded / sizeof forwarded[0]; i++)
			sigaction(forwarded[i], &action, NULL);
		sigaction(SIGCHLD, &action, NULL);
		sigaction(SIGPIPE, &action, NULL);
		if (setsid() < 0 || dup2(out[1], 1) < 0 || dup2(err[1], 2) < 0)
			child_fail(report, "the workload's stdio");
		hull_access_execute((char *const *)m->argv, (char *const *)m->envp);
		child_fail(report, m->argv[0]);
	}
	/* The engine reads `report` to its end: this copy, and the workload's
	 * once it execs. */
	close(report);
	close(out[1]);
	close(err[1]);
	relay(conn, listener, out[0], err[0], workload);
}

/* --- the reader ---------------------------------------------------------- */

static int read_full(int fd, void *bytes, size_t n)
{
	uint8_t *at = bytes;

	while (n > 0u) {
		ssize_t got = read(fd, at, n);

		if (got < 0 && errno == EINTR)
			continue;
		if (got <= 0)
			return -1;
		at += got;
		n -= (size_t)got;
	}
	return 0;
}

static void end_pasta(hull_machine *m);

static void *read_workload(void *arg)
{
	/* The thread's own: one runs per machine, and a buffer they shared
	 * was overwritten by one while the other's on_data copied it out. */
	uint8_t        buffer[65536];
	hull_machine  *m = arg;
	frame          f;
	int            status = 0;
	int            said = 0;

	/* The init's frames, to the end it says or to the socket's end. */
	while (read_full(m->conn, &f, sizeof f) == 0) {
		uint32_t left = f.length;

		if (f.kind == 'x' && f.length == sizeof status &&
		    read_full(m->conn, &status, sizeof status) == 0) {
			said = 1;
			break;
		}
		while (left > 0u) {
			size_t n = left < sizeof buffer ? left : sizeof buffer;

			if (read_full(m->conn, buffer, n) != 0)
				break;
			if (f.kind == 'd')
				host->on_data(m->core, NULL, f.fd == 2 ? 2 : 1, buffer, n);
			left -= (uint32_t)n;
		}
		if (left > 0u)
			break;
	}
	/* Waited for without reaping, then reaped and marked done under the
	 * lock, so that stop() signals the init's pid only while it is the
	 * init's or its zombie's. An adopted init is init's to reap; its end
	 * is its pidfd's. */
	if (m->adopted) {
		struct pollfd fd = { m->pidfd, POLLIN, 0 };

		while (poll(&fd, 1u, -1) < 0 && errno == EINTR)
			;
		pthread_mutex_lock(&m->lock);
	} else {
		siginfo_t info;
		int       own;

		while (waitid(P_PID, (id_t)m->pid, &info, WEXITED | WNOWAIT) < 0 && errno == EINTR)
			;
		pthread_mutex_lock(&m->lock);
		waitpid(m->pid, &own, 0);
		/* The init's own, as when stop killed it, if it said nothing. */
		if (!said)
			status = own;
	}
	m->done = 1;
	pthread_cond_broadcast(&m->done_cond);
	pthread_mutex_unlock(&m->lock);
	/* Its ports free before anyone hears it has ended. */
	end_pasta(m);
	if (!said && m->adopted)
		status = SIGKILL;  /* a wait status: killed by it */
	if (WIFSIGNALED(status))
		host->on_exit(m->core, m, NULL, -1, WTERMSIG(status));
	else
		host->on_exit(m->core, m, NULL, WEXITSTATUS(status), 0);
	host->on_state(m->core, m, "stopped");
	return NULL;
}

/* --- the machine's directory ------------------------------------------------ */

/*
 * hull/chroot-PID-N under the runtime directory, PID the core's: where a
 * kept machine's init.sock is. A core killed outright leaves those of
 * machines not kept; one core runs per user, holding core.lock, so one
 * named for a pid that is not running was a dead core's, and it is
 * removed, init.sock by name, unless a machine taken back uses it.
 */
static pthread_mutex_t adopted_lock = PTHREAD_MUTEX_INITIALIZER;
static char            adopted_dirs[16][4096];

static void mark_adopted(const char *dir, int on)
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

static int is_adopted(const char *dir)
{
	int found = 0;

	pthread_mutex_lock(&adopted_lock);
	for (unsigned i = 0u; i < 16u && !found; i++)
		found = strcmp(adopted_dirs[i], dir) == 0;
	pthread_mutex_unlock(&adopted_lock);
	return found;
}

static void remove_dir(const char *dir)
{
	char path[4200];

	snprintf(path, sizeof path, "%s/init.sock", dir);
	unlink(path);
	rmdir(dir);
}

static void sweep(const char *parent)
{
	DIR           *list = opendir(parent);
	struct dirent *item;

	while (list != NULL && (item = readdir(list)) != NULL) {
		char  path[4200];
		long  pid;
		char *end;

		if (strncmp(item->d_name, "chroot-", 7u) != 0)
			continue;
		pid = strtol(item->d_name + 7, &end, 10);
		if (end == item->d_name + 7 || *end != '-' || pid <= 0 || pid == (long)getpid() ||
		    kill((pid_t)pid, 0) == 0 || errno != ESRCH)
			continue;
		snprintf(path, sizeof path, "%s/%s", parent, item->d_name);
		if (!is_adopted(path))
			remove_dir(path);
	}
	if (list != NULL)
		closedir(list);
}

/*
 * The machine's network on a bridge, as the root core: hull0 made first
 * where it is its (hostnet.c), then a veth whose far end is the machine's
 * eth0, moved into the init's namespace, and whose near end is on the
 * bridge. The near end goes with the namespace, so nothing is left to
 * remove. Before the init goes on, which then sets eth0's address on
 * hull's own. NULL, or why in `why`.
 */
static const char *start_bridge(const hull_machine *m, char *why, size_t cap)
{
	const char *ip = hull_hostnet_ip();
	char        near[16];
	char        pid_text[24];

	if (m->bridge_own && hull_hostnet_bridge(why, cap) != NULL)
		return why;
	if (m->bridge_relay && hull_hostnet_relay(why, cap) != NULL)
		return why;
	snprintf(near, sizeof near, "hb%ld", (long)m->pid);
	snprintf(pid_text, sizeof pid_text, "%ld", (long)m->pid);
	{
		const char *pair[] = { ip, "link", "add", near, "type", "veth", "peer", "name", "eth0",
		                       "netns", pid_text, NULL };
		const char *on[] = { ip, "link", "set", near, "master", m->bridge_name, "up", NULL };

		if (hull_hostnet_run(pair, why, cap) != 0 || hull_hostnet_run(on, why, cap) != 0)
			return why;
	}
	return NULL;
}

/*
 * cpus and memory, section 6's ceiling for ns, as a systemd scope the init
 * is moved into before its workload starts: CPUQuota of N CPUs' time and
 * MemoryMax. A cgroup the user may write is one systemd delegates, so its
 * manager is the rootless way there -- the user's for a core run as the
 * user, the system's for one run as root -- and a host with none on the
 * bus is refused rather than run unlimited. The scope is the init's and
 * what it forks, and ends with the last of them. It is asked for by
 * busctl, and the init's own cgroup read until it is there, since the
 * manager moves it once the call has returned. NULL, or why in `why`.
 */
static const char *start_limits(const hull_machine *m, char *why, size_t cap)
{
	char        unit[40];
	char        pid_text[24];
	char        count[4];
	char        quota[24];
	char        memory[24];
	char        said[160];
	const char *argv[24];
	unsigned    n = 0u;
	unsigned    properties = 1u;

	snprintf(unit, sizeof unit, "hull-%ld.scope", (long)m->pid);
	snprintf(pid_text, sizeof pid_text, "%ld", (long)m->pid);
	argv[n++] = "busctl";
	argv[n++] = getuid() == 0 ? "--system" : "--user";
	argv[n++] = "call";
	argv[n++] = "org.freedesktop.systemd1";
	argv[n++] = "/org/freedesktop/systemd1";
	argv[n++] = "org.freedesktop.systemd1.Manager";
	argv[n++] = "StartTransientUnit";
	argv[n++] = "ssa(sv)a(sa(sv))";
	argv[n++] = unit;
	argv[n++] = "fail";
	argv[n++] = count;
	argv[n++] = "PIDs";
	argv[n++] = "au";
	argv[n++] = "1";
	argv[n++] = pid_text;
	if (m->cpus != 0u) {
		snprintf(quota, sizeof quota, "%llu", (unsigned long long)m->cpus * 1000000ull);
		argv[n++] = "CPUQuotaPerSecUSec";
		argv[n++] = "t";
		argv[n++] = quota;
		properties++;
	}
	if (m->memory != 0u) {
		snprintf(memory, sizeof memory, "%llu", m->memory);
		argv[n++] = "MemoryMax";
		argv[n++] = "t";
		argv[n++] = memory;
		properties++;
	}
	argv[n++] = "0";
	argv[n] = NULL;
	snprintf(count, sizeof count, "%u", properties);
	if (hull_hostnet_run(argv, said, sizeof said) != 0) {
		snprintf(why, cap, "cpus and memory are a systemd scope, and the %s manager gave none: %s",
		         getuid() == 0 ? "system's" : "user's", said);
		return why;
	}
	{
		char path[48];

		snprintf(path, sizeof path, "/proc/%ld/cgroup", (long)m->pid);
		for (int tries = 0; tries < LIMITS_TRIES; tries++) {
			struct timespec step = { 0, 10000000L };
			char            line[512];
			FILE           *file = fopen(path, "re");
			int             there = 0;

			if (file != NULL) {
				while (fgets(line, sizeof line, file) != NULL)
					there = there || (strncmp(line, "0::", 3u) == 0 && strstr(line, unit) != NULL);
				fclose(file);
			}
			if (there)
				return NULL;
			nanosleep(&step, NULL);
		}
	}
	snprintf(why, cap, "cpus and memory: the scope %s did not take the machine's init in %d s",
	         unit, LIMITS_TRIES / 100);
	return why;
}

/*
 * The machine's network under NAT: pasta, as the user, attached to the
 * init's user and network namespaces by its pid, configuring eth0 there
 * as passt does a vm's -- GUEST_ADDRESS, the gateway, the host's resolver
 * at DNS_ADDRESS -- with --no-map-gw, so the machine cannot reach what
 * listens on the host's loopback only for the user. Each port, and an
 * sshd's, is forwarded in from the machine's 127.107 address; pasta's
 * loopback path, which passt lacks, carries a connection from the host's
 * loopback in (section 8.1). Nothing goes the other way, -T and -U none.
 * pasta's parent exits once it is set up, 0 or with its reason on the
 * pipe; the daemon it leaves ends when the init's /proc entry goes, so it
 * needs no supervising and outlives a core under keep with its machine.
 * NULL, or why in `why`.
 */
/*
 * Whether `pid` is pasta's daemon for the init `init`: pasta, the user's,
 * given the init's pid as its last word. Read again before each signal,
 * as a pid is a moment and not a process.
 */
static int pasta_of(pid_t pid, pid_t init)
{
	char    path[64];
	char    words[4096];
	char    want[24];
	ssize_t n;
	int     fd;
	size_t  last = 0u;

	snprintf(path, sizeof path, "/proc/%ld/cmdline", (long)pid);
	if ((fd = open(path, O_RDONLY | O_CLOEXEC)) < 0)
		return 0;
	n = read(fd, words, sizeof words - 1u);
	close(fd);
	if (n <= 0)
		return 0;
	words[n] = '\0';
	for (ssize_t i = 0; i + 1 < n; i++)
		if (words[i] == '\0')
			last = (size_t)i + 1u;
	snprintf(want, sizeof want, "%ld", (long)init);
	return strcmp(words + last, want) == 0 &&
	       (strcmp(words, "pasta") == 0 ||
	        (strlen(words) > 6u && strcmp(words + strlen(words) - 6u, "/pasta") == 0));
}

/* pasta's daemon for `init`, which its parent left on exiting: found in
 * /proc, as it writes no pid file where AppArmor lets it be read here
 * (engine/qemu.c says the same of passt). 0 where none is seen. */
static pid_t find_pasta(pid_t init)
{
	DIR           *proc = opendir("/proc");
	struct dirent *entry;
	pid_t          found = 0;

	while (proc != NULL && found == 0 && (entry = readdir(proc)) != NULL) {
		pid_t pid;

		if (entry->d_name[0] < '1' || entry->d_name[0] > '9')
			continue;
		pid = (pid_t)strtol(entry->d_name, NULL, 10);
		if (pasta_of(pid, init))
			found = pid;
	}
	if (proc != NULL)
		closedir(proc);
	return found;
}

/*
 * pasta ended with its machine, and waited for, at most 2 s: left to see
 * the namespace go by itself it took about a second, its ports held all
 * the while, so a run of the same machine straight after -- on any
 * engine, a test's loop or `make check-flip`'s next backend -- found its
 * address and port in use. A daemon gone or another's by now is left.
 */
static void end_pasta(hull_machine *m)
{
	if (m->pasta <= 0 || !pasta_of(m->pasta, m->pid)) {
		m->pasta = 0;
		return;
	}
	kill(m->pasta, SIGTERM);
	for (int tries = 0; tries < 200; tries++) {
		char  path[64];
		char  stat_line[256];
		int   fd;
		ssize_t n;
		char *state;

		snprintf(path, sizeof path, "/proc/%ld/stat", (long)m->pasta);
		if ((fd = open(path, O_RDONLY | O_CLOEXEC)) < 0)
			break;
		n = read(fd, stat_line, sizeof stat_line - 1u);
		close(fd);
		stat_line[n > 0 ? n : 0] = '\0';
		/* Exited, its sockets closed, though not yet reaped. */
		if ((state = strrchr(stat_line, ')')) != NULL && state[1] == ' ' && state[2] == 'Z')
			break;
		usleep(10000);
	}
	m->pasta = 0;
}

static const char *start_pasta(hull_machine *m, char *why, size_t cap)
{
	char        pid_text[24];
	char        forward[MAX_PORTS + 1u][96];
	const char *argv[40 + 2u * (MAX_PORTS + 1u)];
	unsigned    n = 0u;
	unsigned    tcp = 0u;
	unsigned    udp = 0u;
	int         said[2];
	int         status;
	pid_t       pid;
	ssize_t     got;

	argv[n++] = "pasta";
	argv[n++] = "--quiet";
	if (m->bridge) {
		/* On hull's bridge, its ports alone: pasta's loopback path splices
		 * a connection to the machine's address there, and its tap, which
		 * this pasta cannot do without, is left down and unaddressed, the
		 * machine's network being the bridge's. */
		argv[n++] = "--ns-ifname";
		argv[n++] = "hp0";
		argv[n++] = "--ipv4-only";
		argv[n++] = "--address";
		argv[n++] = m->bridge_address;
		argv[n++] = "--netmask";
		argv[n++] = "15";
	} else {
		argv[n++] = "--config-net";
		argv[n++] = "--ns-ifname";
		argv[n++] = "eth0";
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
	}
	for (unsigned i = 0u; i <= m->port_count; i++) {
		int udp_port = i < m->port_count && m->port_udp[i];

		if (i == m->port_count && !m->ssh)
			break;
		snprintf(forward[i], sizeof forward[i], "%s/%u:%u", m->address_text,
		         i < m->port_count ? m->port_host[i] : (unsigned)HULL_SSH_PORT,
		         i < m->port_count ? m->port_guest[i] : (unsigned)HULL_SSH_PORT);
		argv[n++] = udp_port ? "-u" : "-t";
		argv[n++] = forward[i];
		if (udp_port)
			udp++;
		else
			tcp++;
	}
	if (tcp == 0u) {
		argv[n++] = "-t";
		argv[n++] = "none";
	}
	if (udp == 0u) {
		argv[n++] = "-u";
		argv[n++] = "none";
	}
	argv[n++] = "-T";
	argv[n++] = "none";
	argv[n++] = "-U";
	argv[n++] = "none";
	snprintf(pid_text, sizeof pid_text, "%ld", (long)m->pid);
	argv[n++] = pid_text;
	argv[n] = NULL;

	if (pipe2(said, O_CLOEXEC) != 0) {
		snprintf(why, cap, "the machine's network: %s", strerror(errno));
		return why;
	}
	pid = fork();
	if (pid == 0) {
		int null = open("/dev/null", O_RDWR);

		if (null < 0 || dup2(null, 0) < 0 || dup2(said[1], 1) < 0 || dup2(said[1], 2) < 0)
			_exit(126);
		syscall(SYS_close_range, 3u, ~0u, 0u);
		execvp(argv[0], (char *const *)argv);
		fprintf(stderr, "pasta, which a chroot machine's network needs: %s", strerror(errno));
		_exit(127);
	}
	close(said[1]);
	if (pid < 0) {
		close(said[0]);
		snprintf(why, cap, "pasta cannot be started: %s", strerror(errno));
		return why;
	}
	while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
		;
	/* What it said, without waiting on the daemon, which may hold the
	 * pipe's other end. */
	fcntl(said[0], F_SETFL, O_NONBLOCK);
	got = read(said[0], why, cap - 1u);
	close(said[0]);
	if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
		m->pasta = find_pasta(m->pid);
		return NULL;
	}
	if (got <= 0)
		snprintf(why, cap, "pasta could not give the machine its network");
	else {
		why[got] = '\0';
		while (got > 0 && (why[got - 1] == '\n' || why[got - 1] == '\r'))
			why[--got] = '\0';
	}
	return why;
}

static const char *make_dir(hull_machine *m)
{
	static unsigned counter;
	const char     *runtime = getenv("XDG_RUNTIME_DIR");
	char            parent[3900];

	if (runtime == NULL || runtime[0] != '/')
		return "XDG_RUNTIME_DIR is not set";
	snprintf(parent, sizeof parent, "%s/hull", runtime);
	mkdir(parent, 0700);
	sweep(parent);
	snprintf(m->dir, sizeof m->dir, "%s/chroot-%ld-%u", parent, (long)getpid(),
	         __atomic_add_fetch(&counter, 1u, __ATOMIC_RELAXED));
	if (mkdir(m->dir, 0700) != 0) {
		m->dir[0] = '\0';
		return "the machine's directory cannot be made";
	}
	return NULL;
}

/* --- operations ---------------------------------------------------------- */

static hull_status chroot_create(hull_core *core, hull_buf plan, hull_machine **out,
                                 hull_error *error)
{
	hull_sexpr_doc doc;
	hull_machine  *m;
	const char    *reason;

	(void)core;
	if (hull_sexpr_open(&doc, plan.data, plan.len, HULL_SEXPR_CANONICAL, NULL) != HULL_SEXPR_OK)
		return fail(error, HULL_ERR_UNSUPPORTED, "the plan is not canonical");
	m = calloc(1u, sizeof *m);
	if (m == NULL)
		return fail(error, HULL_ERR_INTERNAL, "out of memory");
	m->core  = core;
	m->conn  = -1;
	m->pidfd = -1;
	for (unsigned i = 0u; i < MAX_SEGMENTS; i++)
		m->segment_fd[i] = -1;
	pthread_mutex_init(&m->lock, NULL);
	pthread_cond_init(&m->done_cond, NULL);
	reason = read_plan(m, &doc);
	if (reason == NULL && m->keep) {
		m->plan_length = plan.len;
		if ((m->plan = malloc(plan.len)) == NULL)
			reason = "out of memory";
		else
			memcpy(m->plan, plan.data, plan.len);
	}
	if (reason != NULL) {
		release(m);
		return fail(error, HULL_ERR_UNSUPPORTED, reason);
	}
	*out = m;
	host->on_state(core, m, "created");
	return HULL_OK;
}

static hull_status chroot_start(hull_core *core, hull_machine *m, hull_error *error)
{
	int         pair[2];
	int         report[2];
	int         mapped[2];
	int         dir_fd = -1;
	int         wanted;
	uid_t       uid = getuid();
	gid_t       gid = getgid();
	ssize_t     n;
	const char *reason;
	const char *net = NULL;
	char        net_why[sizeof ((hull_machine *)0)->detail];

	if (m->started)
		return fail(error, HULL_ERR_BUSY, "the machine has been started already");
	/* Its devices as the host has them now: a board is plugged in, reset
	 * and plugged in again between a plan and a start. */
	for (unsigned i = 0u; i < m->device_count; i++) {
		if (hull_hostdev_find(&m->device[i], m->detail, sizeof m->detail) != NULL)
			return fail(error, HULL_ERR_RUNTIME, m->detail);
		/* Its /dev/pts is a devpts of its own, where nothing is bound. */
		if (strncmp(m->device[i].host.node, "/dev/pts/", 9u) == 0) {
			snprintf(m->detail, sizeof m->detail,
			         "%s is a pty of the host's, and a machine's /dev/pts is its own",
			         m->device[i].host.node);
			return fail(error, HULL_ERR_RUNTIME, m->detail);
		}
	}
	if (hull_hostdev_claim(m, m->device, m->device_count, m->detail, sizeof m->detail) != NULL)
		return fail(error, HULL_ERR_RUNTIME, m->detail);
	/* A kept machine's directory, for its init.sock. */
	if ((m->keep || m->staged || m->hosts != NULL || m->resolver != NULL) &&
	    ((reason = make_dir(m)) != NULL ||
	     (m->keep && (dir_fd = open(m->dir, O_PATH | O_DIRECTORY | O_CLOEXEC)) < 0))) {
		snprintf(m->detail, sizeof m->detail, "%s",
		         reason != NULL ? reason : "the machine's directory cannot be opened");
		if (m->dir[0] != '\0')
			remove_dir(m->dir);
		m->dir[0] = '\0';
		return fail(error, HULL_ERR_RUNTIME, m->detail);
	}
	/* A port on each segment, from the core's switch, which the init takes
	 * and the engine then closes, so that the port goes with the machine. */
	for (unsigned i = 0u; i < m->segment_count; i++) {
		if (host->struct_size < offsetof(hull_host_ops, segment) + sizeof host->segment ||
		    host->segment == NULL) {
			snprintf(m->detail, sizeof m->detail, "the core has no switch for a segment");
			m->segment_fd[i] = -1;
		} else {
			m->segment_fd[i] = host->segment(m->core, m->segment[i], m->detail,
			                                 sizeof m->detail);
		}
		if (m->segment_fd[i] < 0) {
			for (unsigned j = 0u; j < i; j++) {
				close(m->segment_fd[j]);
				m->segment_fd[j] = -1;
			}
			if (dir_fd >= 0)
				close(dir_fd);
			return fail(error, HULL_ERR_RUNTIME, m->detail);
		}
	}
	if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, pair) != 0 ||
	    pipe2(report, O_CLOEXEC) != 0 || pipe2(mapped, O_CLOEXEC) != 0) {
		if (dir_fd >= 0)
			close(dir_fd);
		return fail(error, HULL_ERR_RUNTIME, "pipe");
	}
	/* clone with no stack is fork into new namespaces: the child is PID 1
	 * of its own. The ids are read first, since in the child's new user
	 * namespace they are unmapped until it maps them. */
	m->pid = (pid_t)syscall(SYS_clone, CLONE_NEWUSER | CLONE_NEWNS | CLONE_NEWUTS |
	                        CLONE_NEWPID | (m->no_network || m->nat || m->bridge ? CLONE_NEWNET : 0) | SIGCHLD, NULL,
	                        NULL, NULL, 0L);
	if (m->pid < 0)
		return fail(error, HULL_ERR_RUNTIME, "clone");
	if (m->pid == 0) {
		close(report[0]);
		close(pair[0]);
		close(mapped[1]);
		run_init(m, uid, gid, pair[1], dir_fd, report[1], mapped[0]);
	}
	close(pair[1]);
	close(report[1]);
	close(mapped[0]);
	for (unsigned i = 0u; i < m->segment_count; i++) {
		close(m->segment_fd[i]);
		m->segment_fd[i] = -1;
	}
	/* Many uids only above an overlay, whose upper layer is a tmpfs in the
	 * machine's own namespace: in a directory used in place an owner other
	 * than the user would be one on the user's own disk. */
	/* Not with a kept upper, which is on the user's disk. */
	{
		int kept = m->upper != NULL;

		/* Nor with a volume's upper kept, on the user's disk too. */
		for (unsigned i = 0u; i < m->volume_count; i++)
			kept = kept || m->volume_stack[i].upper != NULL;
		wanted = m->overlay && !m->readonly_root && !kept;
		m->many = wanted && hull_map_many(m->pid, uid, gid);
	}
	/* Its network before the init goes on, so the workload starts with
	 * one; an init whose network cannot be had is killed, and the report
	 * read below then says why. */
	if (m->nat && (net = start_pasta(m, net_why, sizeof net_why)) != NULL)
		kill(m->pid, SIGKILL);
	if (m->bridge && (net = start_bridge(m, net_why, sizeof net_why)) != NULL)
		kill(m->pid, SIGKILL);
	/* Its ports and sshd on hull's bridge, forwarded by pasta from its
	 * 127.107 address as under NAT. */
	if (net == NULL && m->bridge_own && (m->port_count != 0u || m->ssh) &&
	    (net = start_pasta(m, net_why, sizeof net_why)) != NULL)
		kill(m->pid, SIGKILL);
	/* Its limits too, so nothing it runs is ever outside them. */
	if (net == NULL && (m->cpus != 0u || m->memory != 0u) &&
	    (net = start_limits(m, net_why, sizeof net_why)) != NULL)
		kill(m->pid, SIGKILL);
	if (write(mapped[1], m->many ? "M" : wanted ? "O" : "S", 1u) != 1) {
		/* The child reads its end closed and says so on the report. */
	}
	/* After it, what the machine is to be told of its tree's owners, if
	 * anything: read to the end, the end being this close. */
	{
		char notice[1024];

		owner_notice(m, uid, gid, notice, sizeof notice);
		if (notice[0] != '\0' && write(mapped[1], notice, strlen(notice)) < 0) {
			/* a notice lost is a notice; the machine runs */
		}
	}
	close(mapped[1]);
	if (dir_fd >= 0)
		close(dir_fd);
	m->conn = pair[0];

	/* The report pipe closes on exec; anything written on it is why the
	 * child could not get that far, and nothing in START_LIMIT_MS is an
	 * init that never will, killed and said so. */
	{
		struct pollfd waiting = { .fd = report[0], .events = POLLIN };
		int           ready;

		while ((ready = poll(&waiting, 1u, START_LIMIT_MS)) < 0 && errno == EINTR)
			;
		if (ready == 0) {
			kill(m->pid, SIGKILL);
			snprintf(m->detail, sizeof m->detail,
			         "the machine's init did not reach its workload in %d s, and was killed",
			         START_LIMIT_MS / 1000);
			n = (ssize_t)strlen(m->detail);
		} else {
			n = read(report[0], m->detail, sizeof m->detail - 1u);
		}
		if (net != NULL) {
			snprintf(m->detail, sizeof m->detail, "%s", net);
			n = (ssize_t)strlen(m->detail);
		}
	}
	close(report[0]);
	if (n > 0) {
		m->detail[n] = '\0';
		waitpid(m->pid, NULL, 0);
		close(m->conn);
		m->conn = -1;
		m->done = 1;
		if (m->dir[0] != '\0')
			remove_dir(m->dir);
		m->dir[0] = '\0';
		return fail(error, HULL_ERR_RUNTIME, m->detail);
	}
	if (pthread_create(&m->reader, NULL, read_workload, m) != 0) {
		kill(m->pid, SIGKILL);
		waitpid(m->pid, NULL, 0);
		m->done = 1;
		return fail(error, HULL_ERR_INTERNAL, "the reader thread could not start");
	}
	m->started = 1;
	host->on_state(core, m, "running");
	return HULL_OK;
}

/* Wait for the workload to be gone, up to `timeout_ms`. */
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

/* `signal` to the init, unless it has been reaped and its pid freed. */
/*
 * hull's init where (init PATH) handed PID 1 to the image's: PID 1's child
 * named HANDED_NAME, which relays the workload and takes its signals; 0
 * where there is none. The children are the kernel's list, of a process
 * the reader has not reaped, under the machine's lock.
 */
static pid_t handed_init(const hull_machine *m)
{
	char  path[64];
	FILE *list;
	long  child;
	pid_t found = 0;

	snprintf(path, sizeof path, "/proc/%ld/task/%ld/children", (long)m->pid, (long)m->pid);
	list = fopen(path, "re");
	while (list != NULL && found == 0 && fscanf(list, "%ld", &child) == 1) {
		char  comm[32] = "";
		FILE *name;

		snprintf(path, sizeof path, "/proc/%ld/comm", child);
		if ((name = fopen(path, "re")) != NULL) {
			if (fgets(comm, sizeof comm, name) != NULL)
				comm[strcspn(comm, "\n")] = '\0';
			fclose(name);
		}
		if (strcmp(comm, HANDED_NAME) == 0)
			found = (pid_t)child;
	}
	if (list != NULL)
		fclose(list);
	return found;
}

static void signal_init(hull_machine *m, int signal)
{
	pid_t to;

	pthread_mutex_lock(&m->lock);
	/* Handed over, a signal is the workload's, by hull's init, as without
	 * the key; a KILL is PID 1's, which ends the namespace whole. */
	to = m->init != NULL && !m->adopted && signal != SIGKILL && !m->done ? handed_init(m) : 0;
	if (to > 0)
		kill(to, signal);
	else if (!m->done && m->adopted)
		syscall(SYS_pidfd_send_signal, m->pidfd, signal, NULL, 0u);
	else if (!m->done)
		kill(m->pid, signal);
	pthread_mutex_unlock(&m->lock);
}

/* SIGTERM, and SIGKILL if that is not enough within `timeout_ms`. */
static hull_status chroot_stop(hull_core *core, hull_machine *m, int timeout_ms,
                               hull_error *error)
{
	(void)core;
	(void)error;
	if (!m->started || wait_done(m, 0))
		return HULL_OK;
	host->on_state(core, m, "stopping");
	signal_init(m, SIGTERM);
	if (!wait_done(m, timeout_ms)) {
		signal_init(m, SIGKILL);
		wait_done(m, 60000);
	}
	return HULL_OK;
}

static hull_status chroot_destroy(hull_core *core, hull_machine *m, hull_error *error)
{
	if (m->started) {
		chroot_stop(core, m, 0, error);
		pthread_join(m->reader, NULL);
	}
	/* The work directories overlayfs left, which the user could not
	 * remove: its namespace, and so its mounts, are gone with it. */
	hull_stack_unwork(m->upper_work);
	for (unsigned i = 0u; i < m->volume_count; i++)
		hull_stack_unwork(m->volume_stack[i].upper_work);
	if (m->dir[0] != '\0') {
		mark_adopted(m->dir, 0);
		remove_dir(m->dir);
	}
	release(m);
	return HULL_OK;
}

/* --- adoption ------------------------------------------------------------ */

/*
 * A kept machine is taken back from what persist() gave: the init's pid,
 * the machine's directory, and the plan, which gives the exec helper the
 * machine's env, workdir and no_new_privs again. The init is checked by
 * the pid its own socket names, connected to through the directory, the
 * pid then stood for by a pidfd; it gives what it held, then the frames
 * go on as before.
 */
static hull_status chroot_persist(hull_core *core, hull_machine *m, hull_buf *out,
                                  hull_error *error)
{
	char   head[4200];
	int    n;

	(void)core;
	n = snprintf(head, sizeof head, "pid %ld\ndir %s\nplan\n", (long)m->pid, m->dir);
	if (m->plan == NULL || n < 0 || (size_t)n >= sizeof head)
		return fail(error, HULL_ERR_INTERNAL, "the state does not fit");
	free(m->persisted);
	m->persisted = malloc((size_t)n + m->plan_length);
	if (m->persisted == NULL)
		return fail(error, HULL_ERR_INTERNAL, "out of memory");
	memcpy(m->persisted, head, (size_t)n);
	memcpy(m->persisted + n, m->plan, m->plan_length);
	out->data = (const uint8_t *)m->persisted;
	out->len  = (size_t)n + m->plan_length;
	return HULL_OK;
}

static hull_status chroot_adopt(hull_core *core, hull_buf state, hull_machine **out,
                                hull_error *error)
{
	static char        detail[300];
	const char        *text = (const char *)state.data;
	const char        *plan_at;
	hull_sexpr_doc     doc;
	hull_machine      *m;
	struct ucred       peer;
	socklen_t          length = sizeof peer;
	struct sockaddr_un at = { .sun_family = AF_UNIX };
	long               pid = 0;
	int                dir_fd;
	const char        *reason = NULL;
	char               dir[4096];

	plan_at = state.len > 0u ? memmem(text, state.len, "\nplan\n", 6u) : NULL;
	if (plan_at == NULL || sscanf(text, "pid %ld\ndir %4095[^\n]\n", &pid, dir) != 2 ||
	    pid <= 1 || dir[0] != '/')
		return fail(error, HULL_ERR_UNSUPPORTED, "the state is not this engine's");
	plan_at += 6;
	m = calloc(1u, sizeof *m);
	if (m == NULL)
		return fail(error, HULL_ERR_INTERNAL, "out of memory");
	m->core  = core;
	m->conn  = -1;
	m->pid   = (pid_t)pid;
	m->keep  = 1;
	for (unsigned i = 0u; i < MAX_SEGMENTS; i++)
		m->segment_fd[i] = -1;
	pthread_mutex_init(&m->lock, NULL);
	pthread_cond_init(&m->done_cond, NULL);
	m->plan_length = state.len - (size_t)(plan_at - text);
	m->plan = malloc(m->plan_length);
	if (m->plan == NULL ||
	    hull_sexpr_open(&doc, memcpy(m->plan, plan_at, m->plan_length), m->plan_length,
	                    HULL_SEXPR_CANONICAL, NULL) != HULL_SEXPR_OK ||
	    (reason = read_plan(m, &doc)) != NULL) {
		release(m);
		return fail(error, HULL_ERR_UNSUPPORTED, reason != NULL ? reason : "the plan is not one");
	}
	/* The pidfd first, then the socket's peer checked against it. */
	m->pidfd = (int)syscall(SYS_pidfd_open, m->pid, 0u);
	dir_fd = open(dir, O_PATH | O_DIRECTORY | O_CLOEXEC);
	snprintf(at.sun_path, sizeof at.sun_path, "/proc/self/fd/%d/init.sock", dir_fd);
	m->conn = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (m->pidfd < 0 || dir_fd < 0 || m->conn < 0 ||
	    connect(m->conn, (struct sockaddr *)&at, sizeof at) != 0 ||
	    getsockopt(m->conn, SOL_SOCKET, SO_PEERCRED, &peer, &length) != 0 ||
	    (long)peer.pid != pid) {
		snprintf(detail, sizeof detail, "its init is gone");
		if (dir_fd >= 0)
			close(dir_fd);
		release(m);
		return fail(error, HULL_ERR_GONE, detail);
	}
	close(dir_fd);
	snprintf(m->dir, sizeof m->dir, "%s", dir);
	m->adopted = 1;
	mark_adopted(m->dir, 1);
	if (pthread_create(&m->reader, NULL, read_workload, m) != 0) {
		mark_adopted(m->dir, 0);
		m->dir[0] = '\0';
		release(m);
		return fail(error, HULL_ERR_RUNTIME, "the reader thread could not start");
	}
	m->started = 1;
	*out = m;
	host->on_state(core, m, "running");
	return HULL_OK;
}

/* --- exec --------------------------------------------------------------- */

/*
 * A command in a running machine. A helper forked from the engine joins
 * the init's user, mount, UTS and PID namespaces -- it can, being single
 * threaded, which the process running the engine is not -- chroots
 * through the init's root, and forks the command, which is then a
 * process of the machine. The helper waits for it and reports its wait
 * status through a pipe, as the init does for the workload. The command
 * has PR_SET_PDEATHSIG, so killing the helper ends it: that is
 * stream_close(). A signal for the command goes to the helper, which
 * forwards it as the init does, so that only its parent, which reaps it,
 * ever signals the command's pid.
 *
 * With `(tty true)` the core opens a pseudo-terminal and the command's
 * stdin, stdout and stderr are its other end, its controlling terminal.
 * The terminal is the host's, passed in open: nothing is mounted in the
 * machine for it.
 */
struct hull_stream {
	hull_machine *machine;
	hull_core    *core;
	pid_t         helper;
	int           in;      /* the command's stdin, write end; -1 once ended */
	int           out;
	int           err;     /* -1 on a terminal, where it is the same as out */
	int           tty;     /* the terminal's master, else -1 */
	int           status;  /* the helper's status pipe */
	pthread_t     reader;
	pthread_mutex_t lock;
	int           reaped;  /* the helper's pid is no longer its own */
	int           writing; /* a stream_write() is using `in` */
	int           ending;  /* stream_eof() has been called */
	int           dialed;  /* a raw connection to the machine's sshd, no helper */
};

static void *read_exec(void *arg)
{
	hull_stream  *st = arg;
	struct pollfd fds[2] = { { st->out, POLLIN, 0 }, { st->err, POLLIN, 0 } };
	uint8_t       buffer[4096];
	int           open_count = st->err < 0 ? 1 : 2;
	int           status = 0;
	int           seen;
	struct rusage spent;

	while (open_count > 0) {
		if (poll(fds, 2, -1) < 0) {
			if (errno == EINTR)
				continue;
			break;
		}
		for (int i = 0; i < 2; i++) {
			ssize_t n;

			if (fds[i].fd < 0 || fds[i].revents == 0)
				continue;
			n = read(fds[i].fd, buffer, sizeof buffer);
			if (n > 0) {
				host->on_data(st->core, st, i + 1, buffer, (size_t)n);
			} else if (n == 0 || (errno != EINTR && errno != EAGAIN)) {
				/* A terminal's master reads EIO once nothing holds the
				 * other end: its end, as a pipe's is 0. resize() uses
				 * it under the lock. */
				pthread_mutex_lock(&st->lock);
				if (fds[i].fd == st->tty)
					st->tty = -1;
				close(fds[i].fd);
				pthread_mutex_unlock(&st->lock);
				fds[i].fd = -1;
				open_count--;
			}
		}
	}
	/* Waited for without reaping, then reaped under the lock: until it
	 * is, the pid is the helper's or its zombie's, and stream_close() may
	 * signal it; after, it may be anybody's. */
	{
		siginfo_t info;

		while (waitid(P_PID, (id_t)st->helper, &info, WEXITED | WNOWAIT) < 0 && errno == EINTR)
			;
	}
	pthread_mutex_lock(&st->lock);
	memset(&spent, 0, sizeof spent);
	wait4(st->helper, &status, 0, &spent);
	st->reaped = 1;
	pthread_mutex_unlock(&st->lock);
	/* The helper is outside the machine's pid namespace, so once it is
	 * gone, what it and the command it reaped spent is counted here. Its
	 * blocks are 512 bytes of storage io, as /proc's io counts them. */
	pthread_mutex_lock(&st->machine->lock);
	st->machine->spent.cpu += (unsigned long long)(spent.ru_utime.tv_sec + spent.ru_stime.tv_sec) *
	                          1000000ull +
	                          (unsigned long long)(spent.ru_utime.tv_usec + spent.ru_stime.tv_usec);
	st->machine->spent.io += (unsigned long long)(spent.ru_inblock + spent.ru_oublock) * 512u;
	pthread_mutex_unlock(&st->machine->lock);
	if (read(st->status, &seen, sizeof seen) == (ssize_t)sizeof seen)
		status = seen;
	close(st->status);
	if (WIFSIGNALED(status))
		host->on_exit(st->core, st->machine, st, -1, WTERMSIG(status));
	else
		host->on_exit(st->core, st->machine, st, WEXITSTATUS(status), 0);
	return NULL;
}

/* A dial's reader: the socket to the machine's sshd, relayed raw as the
 * stream's output until the far end closes, then the connection's own end,
 * which is the machine's close (0). No helper to reap. It does not close
 * the socket; stream_close does, after joining this thread. */
static void *read_dial(void *arg)
{
	hull_stream *st = arg;
	uint8_t      buffer[4096];

	for (;;) {
		ssize_t n = read(st->out, buffer, sizeof buffer);

		if (n > 0)
			host->on_data(st->core, st, 1, buffer, (size_t)n);
		else if (n == 0 || (errno != EINTR && errno != EAGAIN))
			break;
	}
	host->on_exit(st->core, st->machine, st, 0, 0);
	return NULL;
}

/*
 * A connection to the machine's sshd, for `hull proxy-ssh` and `hull ssh`
 * (section 8.1). chroot shares the host's network, so the sshd bound the
 * machine's own 127.107 address on HULL_SSH_PORT (run_init), and this is a
 * plain connect to it on the host loopback -- no agent, unlike qemu's over
 * vsock. Only the sshd is reachable this way; a machine with none, or a
 * dial to any other port, is refused.
 */
static hull_status chroot_dial(hull_core *core, hull_machine *m, uint16_t port,
                               hull_stream **out, hull_error *error)
{
	struct sockaddr_in to;
	struct pollfd      wait;
	hull_stream       *st;
	int                fd;
	int                failed = 0;
	socklen_t          length = sizeof failed;

	if (!m->ssh)
		return fail(error, HULL_ERR_UNSUPPORTED, "that machine runs no sshd to reach");
	if (port != 22u)
		return fail(error, HULL_ERR_UNSUPPORTED, "a chroot machine has no port to dial but its "
		            "sshd's, which is port 22");
	memset(&to, 0, sizeof to);
	to.sin_family = AF_INET;
	to.sin_port   = htons(HULL_SSH_PORT);
	if (inet_pton(AF_INET, m->address_text, &to.sin_addr) != 1)
		return fail(error, HULL_ERR_GONE, "the machine has no address to dial");
	fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
	if (fd < 0)
		return fail(error, HULL_ERR_INTERNAL, "no socket to dial with");
	if (connect(fd, (struct sockaddr *)&to, sizeof to) != 0) {
		wait = (struct pollfd){ fd, POLLOUT, 0 };
		if (errno != EINPROGRESS)
			failed = errno;
		else if (poll(&wait, 1u, 2000) != 1)
			failed = ETIMEDOUT;
		else if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &failed, &length) != 0)
			failed = errno;
	}
	if (failed != 0) {
		close(fd);
		snprintf(m->detail, sizeof m->detail, "port 22: %s", strerror(failed));
		return fail(error, HULL_ERR_RUNTIME, m->detail);
	}
	if (fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) & ~O_NONBLOCK) != 0 ||
	    (st = calloc(1u, sizeof *st)) == NULL) {
		close(fd);
		return fail(error, HULL_ERR_INTERNAL, "out of memory");
	}
	st->machine = m;
	st->core    = core;
	st->helper  = -1;
	st->status  = -1;
	st->tty     = -1;
	st->out     = fd;
	st->in      = dup(fd);
	st->dialed  = 1;
	pthread_mutex_init(&st->lock, NULL);
	if (st->in < 0 || pthread_create(&st->reader, NULL, read_dial, st) != 0) {
		if (st->in >= 0)
			close(st->in);
		pthread_mutex_destroy(&st->lock);
		close(fd);
		free(st);
		return fail(error, HULL_ERR_INTERNAL, "the reader thread could not start");
	}
	*out = st;
	return HULL_OK;
}

/* The helper: into the machine, then the command. */
static void run_helper(const hull_machine *m, char *const *argv, const int *ns, int root,
                       int in, int out, int err, int tty, int report, int status_pipe)
{
	static const int kinds[] = { CLONE_NEWUSER, CLONE_NEWNS, CLONE_NEWUTS, CLONE_NEWPID,
	                             CLONE_NEWNET };
	static const int forwarded[] = { SIGTERM, SIGINT, SIGHUP, SIGQUIT, SIGUSR1, SIGUSR2 };
	struct sigaction action;
	int              status = 0;
	pid_t            command;
	int              keep[12] = { ns[0], ns[1], ns[2], ns[3], ns[4], root, in, out, err, report,
	                              status_pipe, -1 };

	/* Held from the start: a signal before the command exists is
	 * forwarded once it does, rather than ending the helper. */
	memset(&action, 0, sizeof action);
	action.sa_handler = hold;
	for (unsigned i = 0u; i < sizeof forwarded / sizeof forwarded[0]; i++)
		sigaction(forwarded[i], &action, NULL);
	close_others(keep, 11u);
	/* The net namespace only where the machine has its own: the host's is
	 * not this user namespace's to join. */
	for (unsigned i = 0u; i < 5u; i++)
		if (ns[i] >= 0 && setns(ns[i], kinds[i]) != 0)
			child_fail(report, "joining the machine");
	if (fchdir(root) != 0 || chroot(".") != 0 || chdir(m->workdir) != 0)
		child_fail(report, "the machine's root");
	command = fork();
	if (command < 0)
		child_fail(report, "fork");
	if (command == 0) {
		action.sa_handler = SIG_DFL;
		for (unsigned i = 0u; i < sizeof forwarded / sizeof forwarded[0]; i++)
			sigaction(forwarded[i], &action, NULL);
		if (prctl(PR_SET_PDEATHSIG, SIGKILL, 0L, 0L, 0L) != 0 || dup2(in, 0) < 0 ||
		    dup2(out, 1) < 0 || dup2(err, 2) < 0 || setsid() < 0)
			child_fail(report, "the command's stdio");
		if (tty && ioctl(0, TIOCSCTTY, 0) != 0)
			child_fail(report, "the command's terminal");
		if (m->no_new_privs && prctl(PR_SET_NO_NEW_PRIVS, 1L, 0L, 0L, 0L) != 0)
			child_fail(report, "no_new_privs");
		hull_access_execute(argv, (char *const *)m->envp);
		child_fail(report, argv[0]);
	}
	close(report);
	for (;;) {
		if (pending != 0) {
			kill(command, (int)pending);
			pending = 0;
		}
		if (waitpid(command, &status, 0) == command || errno != EINTR)
			break;
	}
	if (write(status_pipe, &status, sizeof status) != (ssize_t)sizeof status)
		_exit(126);
	_exit(0);
}

/*
 * A pseudo-terminal for an exec, of the size it asked for: `in` and `out`
 * get the master, twice, so that ending the input does not hang the
 * terminal up; all three child ends are the one other end. Returns 1, or
 * 0 with nothing left open.
 */
static int open_terminal(const struct winsize *size, int *in, int *out, int *err)
{
	char           name[64];
	int            master = posix_openpt(O_RDWR | O_NOCTTY | O_CLOEXEC);
	int            other  = -1;
	int            copy   = -1;

	if (master < 0)
		return 0;
	if (grantpt(master) != 0 || unlockpt(master) != 0 ||
	    ptsname_r(master, name, sizeof name) != 0 ||
	    (other = open(name, O_RDWR | O_NOCTTY | O_CLOEXEC)) < 0 ||
	    ioctl(master, TIOCSWINSZ, size) != 0 ||
	    (copy = fcntl(master, F_DUPFD_CLOEXEC, 0)) < 0) {
		if (other >= 0)
			close(other);
		close(master);
		return 0;
	}
	in[0]  = other;
	in[1]  = copy;
	out[0] = master;
	out[1] = other;
	err[0] = -1;
	err[1] = other;
	return 1;
}

/* The words of `(exec (argv ...) ...)`, NULL-terminated, into `argv`,
 * and the terminal it asks for: its size, or 0 columns for none. */
static int read_argv(hull_buf spec, char **argv, struct winsize *size)
{
	hull_sexpr_doc  doc;
	hull_sexpr_form message;
	hull_sexpr_form entry;
	hull_sexpr_form word;
	unsigned        n = 0u;
	int             more;

	if (hull_sexpr_open(&doc, spec.data, spec.len, HULL_SEXPR_CANONICAL, NULL) != HULL_SEXPR_OK ||
	    !hull_sexpr_first(&doc, &message) || !hull_access_find(&doc, &message, "argv", &entry))
		return 0;
	{
		int cols = 0;
		int rows = 0;

		hull_access_terminal(&doc, &message, &cols, &rows);
		size->ws_col = (unsigned short)cols;
		size->ws_row = (unsigned short)rows;
	}
	hull_sexpr_child(&doc, &entry, &word);
	for (more = hull_sexpr_next(&doc, &word, &word); more && n < MAX_ARGS;
	     more = hull_sexpr_next(&doc, &word, &word))
		if ((argv[n++] = hull_access_text(&doc, &word)) == NULL)
			return 0;
	argv[n] = NULL;
	return n != 0u && !more;
}

static void free_argv(char **argv)
{
	for (unsigned i = 0u; argv[i] != NULL; i++)
		free(argv[i]);
}

static hull_status chroot_exec(hull_core *core, hull_machine *m, hull_buf spec,
                               hull_stream **out, hull_error *error)
{
	static const char *const kinds[] = { "user", "mnt", "uts", "pid", "net" };
	char        *argv[MAX_ARGS + 1u] = { NULL };
	char         path[64];
	int          ns[5] = { -1, -1, -1, -1, -1 };
	int          root;
	int          in[2] = { -1, -1 }, o[2] = { -1, -1 }, e[2] = { -1, -1 };
	int          report[2], status[2];
	struct winsize size = { 0 };
	int          tty;
	int          opened;
	hull_stream *st;
	ssize_t      n;
	int          done;

	pthread_mutex_lock(&m->lock);
	done = m->done;
	pthread_mutex_unlock(&m->lock);
	if (!m->started || done)
		return fail(error, HULL_ERR_GONE, "the machine is not running");
	if (!read_argv(spec, argv, &size)) {
		free_argv(argv);
		return fail(error, HULL_ERR_UNSUPPORTED, "an exec names a command");
	}
	tty = size.ws_col != 0u;
	for (unsigned i = 0u; i < (m->no_network || m->nat || m->bridge ? 5u : 4u); i++) {
		snprintf(path, sizeof path, "/proc/%ld/ns/%s", (long)m->pid, kinds[i]);
		ns[i] = open(path, O_RDONLY | O_CLOEXEC);
	}
	snprintf(path, sizeof path, "/proc/%ld/root", (long)m->pid);
	root = open(path, O_PATH | O_DIRECTORY | O_CLOEXEC);
	st = calloc(1u, sizeof *st);
	opened = tty ? open_terminal(&size, in, o, e) : pipe2(in, O_CLOEXEC) == 0 &&
	         pipe2(o, O_CLOEXEC) == 0 && pipe2(e, O_CLOEXEC) == 0;
	if (ns[0] < 0 || ns[1] < 0 || ns[2] < 0 || ns[3] < 0 || ((m->no_network || m->nat || m->bridge) && ns[4] < 0) ||
	    root < 0 || st == NULL ||
	    !opened || pipe2(report, O_CLOEXEC) != 0 || pipe2(status, O_CLOEXEC) != 0) {
		const int ends[] = { ns[0], ns[1], ns[2], ns[3], ns[4], root, in[0], in[1], o[0],
		                     o[1], e[0], e[1] };

		/* On a terminal one descriptor stands in three places. */
		for (unsigned i = 0u; i < sizeof ends / sizeof ends[0]; i++) {
			int seen = 0;

			for (unsigned j = 0u; j < i; j++)
				seen |= ends[j] == ends[i];
			if (ends[i] >= 0 && !seen)
				close(ends[i]);
		}
		free(st);
		free_argv(argv);
		return fail(error, HULL_ERR_RUNTIME, tty ? "no terminal could be opened" :
		            "the machine's namespaces cannot be opened");
	}
	st->machine = m;
	st->core    = core;
	pthread_mutex_init(&st->lock, NULL);
	st->helper  = fork();
	if (st->helper == 0) {
		close(report[0]);
		close(status[0]);
		run_helper(m, argv, ns, root, in[0], o[1], e[1], tty, report[1], status[1]);
	}
	for (unsigned i = 0u; i < 5u; i++)
		if (ns[i] >= 0)
			close(ns[i]);
	close(root);
	/* On a terminal the three are the one other end, closed once. */
	close(in[0]);
	if (!tty) {
		close(o[1]);
		close(e[1]);
	}
	close(report[1]);
	close(status[1]);
	free_argv(argv);
	st->in     = in[1];
	/* Never blocking, so that stream_eof() can end a write. On a
	 * terminal it is the master's description, which the reader then
	 * finds not blocking either. */
	fcntl(st->in, F_SETFL, fcntl(st->in, F_GETFL) | O_NONBLOCK);
	st->out    = o[0];
	st->err    = tty ? -1 : e[0];
	st->tty    = tty ? o[0] : -1;
	st->status = status[0];
	n = st->helper < 0 ? -1 : read(report[0], m->detail, sizeof m->detail - 1u);
	close(report[0]);
	if (n != 0) {
		if (n > 0)
			m->detail[n] = '\0';
		else
			snprintf(m->detail, sizeof m->detail, "the helper could not start");
		if (st->helper > 0)
			waitpid(st->helper, NULL, 0);
		close(st->in);
		close(st->out);
		if (st->err >= 0)
			close(st->err);
		close(st->status);
		free(st);
		return fail(error, HULL_ERR_RUNTIME, m->detail);
	}
	if (pthread_create(&st->reader, NULL, read_exec, st) != 0) {
		kill(st->helper, SIGKILL);
		waitpid(st->helper, NULL, 0);
		close(st->in);
		close(st->out);
		if (st->err >= 0)
			close(st->err);
		close(st->status);
		free(st);
		return fail(error, HULL_ERR_INTERNAL, "the reader thread could not start");
	}
	*out = st;
	return HULL_OK;
}

/* stdin, written whole however long the command takes to read it, on
 * a thread of the caller's own. The descriptor does not block, so the
 * wait is a poll that stream_eof() can end: it marks the input ending,
 * and the write gives up within a tenth of a second. The descriptor is
 * closed by whichever of the two is last, so that no write is ever made
 * to a number reused for something else. */
static hull_status chroot_stream_write(hull_core *core, hull_stream *st, const uint8_t *data,
                                       size_t length, hull_error *error)
{
	hull_status status = HULL_OK;
	int         fd;

	(void)core;
	pthread_mutex_lock(&st->lock);
	fd = st->ending ? -1 : st->in;
	st->writing = fd >= 0;
	pthread_mutex_unlock(&st->lock);
	if (fd < 0)
		return fail(error, HULL_ERR_GONE, "the command's input has closed");
	while (length > 0u) {
		struct pollfd ready = { fd, POLLOUT, 0 };
		ssize_t       n;
		int           ending;

		pthread_mutex_lock(&st->lock);
		ending = st->ending;
		pthread_mutex_unlock(&st->lock);
		if (ending) {
			status = fail(error, HULL_ERR_GONE, "the command's input has closed");
			break;
		}
		n = write(fd, data, length);
		if (n < 0 && (errno == EAGAIN || errno == EINTR)) {
			poll(&ready, 1, 100);
			continue;
		}
		if (n <= 0) {
			status = fail(error, HULL_ERR_GONE, "the command's input has closed");
			break;
		}
		data += n;
		length -= (size_t)n;
	}
	pthread_mutex_lock(&st->lock);
	st->writing = 0;
	if (st->ending && st->in >= 0) {
		close(st->in);
		st->in = -1;
	}
	pthread_mutex_unlock(&st->lock);
	return status;
}

/* The end of stdin. May be called while a write is under way on another
 * thread, which then gives up; the descriptor is that write's to close. */
static hull_status chroot_stream_eof(hull_core *core, hull_stream *st, hull_error *error)
{
	(void)core;
	(void)error;
	pthread_mutex_lock(&st->lock);
	st->ending = 1;
	if (!st->writing && st->in >= 0) {
		close(st->in);
		st->in = -1;
	}
	pthread_mutex_unlock(&st->lock);
	return HULL_OK;
}

static hull_status chroot_resize(hull_core *core, hull_stream *st, int rows, int cols,
                                 hull_error *error)
{
	struct winsize size = { .ws_row = (unsigned short)rows, .ws_col = (unsigned short)cols };
	int            done;

	(void)core;
	if (st == NULL)
		return fail(error, HULL_ERR_UNSUPPORTED, "a machine's workload has no terminal");
	pthread_mutex_lock(&st->lock);
	done = st->tty < 0 || ioctl(st->tty, TIOCSWINSZ, &size) != 0;
	pthread_mutex_unlock(&st->lock);
	if (done)
		return fail(error, HULL_ERR_UNSUPPORTED, "the command has no terminal");
	return HULL_OK;
}

/* A signal for the workload goes to the init, and for a command to its
 * helper: each forwards it, and is the one that reaps what it signals. */
static hull_status chroot_signal(hull_core *core, hull_machine *m, hull_stream *st, int signum,
                                 hull_error *error)
{
	(void)core;
	(void)error;
	if (st == NULL) {
		signal_init(m, signum);
		return HULL_OK;
	}
	pthread_mutex_lock(&st->lock);
	if (st->dialed && st->out >= 0) {
		/* A connection has no process: any signal resets it, as on
		 * docker. Its helper is -1, and kill(-1) is every process the
		 * user has -- which a signal here once was, `hull proxy-ssh`
		 * forwarding the SIGHUP ssh ends its ProxyCommand with. */
		struct linger now = { 1, 0 };

		setsockopt(st->out, SOL_SOCKET, SO_LINGER, &now, sizeof now);
		shutdown(st->out, SHUT_RDWR);
	} else if (!st->reaped && st->helper > 0) {
		kill(st->helper, signum);
	}
	pthread_mutex_unlock(&st->lock);
	return HULL_OK;
}

/* Whether process `pid` is stopped: `T` in /proc/PID/stat, after its name,
 * which is in parentheses and may hold either. */
static int stopped(long pid)
{
	char  path[64];
	char  text[512];
	char *close;
	FILE *f;
	size_t n;

	snprintf(path, sizeof path, "/proc/%ld/stat", pid);
	if ((f = fopen(path, "r")) == NULL)
		return 1;
	n = fread(text, 1u, sizeof text - 1u, f);
	fclose(f);
	text[n] = '\0';
	close = strrchr(text, ')');
	return close == NULL || close[1] == '\0' || close[2] == 'T' || close[2] == 't';
}

/* SIGSTOP or SIGCONT to every process in `pid`'s pid namespace: the
 * count still running after the sweep, for SIGSTOP, else 0; or -1 where
 * the namespace cannot be read. */
static int signal_namespace(pid_t pid, int signum)
{
	struct stat    own;
	struct dirent *entry;
	DIR           *proc;
	char           path[64];
	int            running = 0;

	snprintf(path, sizeof path, "/proc/%ld/ns/pid", (long)pid);
	if (stat(path, &own) != 0 || (proc = opendir("/proc")) == NULL)
		return -1;
	while ((entry = readdir(proc)) != NULL) {
		struct stat other;
		char       *end;
		long        n = strtol(entry->d_name, &end, 10);

		if (*end != '\0' || n <= 0)
			continue;
		snprintf(path, sizeof path, "/proc/%ld/ns/pid", n);
		if (stat(path, &other) != 0 || other.st_ino != own.st_ino || other.st_dev != own.st_dev)
			continue;
		if (signum == SIGSTOP && stopped(n))
			continue;
		kill((pid_t)n, signum);
		running += signum == SIGSTOP;
	}
	closedir(proc);
	return running;
}

/* Every process of the machine stopped, or let go on. A process forked
 * as the sweep ran is missed by it, so the sweep is made again until one
 * finds every process stopped already, a second at most. */
static hull_status chroot_pause(hull_core *core, hull_machine *m, int paused, hull_error *error)
{
	int left = -1;

	(void)core;
	pthread_mutex_lock(&m->lock);
	if (!m->started || m->done) {
		pthread_mutex_unlock(&m->lock);
		return fail(error, HULL_ERR_GONE, "the machine is not running");
	}
	if (!paused) {
		left = signal_namespace(m->pid, SIGCONT);
	} else {
		for (int sweep = 0; sweep < 100; sweep++) {
			struct timespec pause = { 0, 10000000L };

			if ((left = signal_namespace(m->pid, SIGSTOP)) <= 0)
				break;
			nanosleep(&pause, NULL);
		}
	}
	pthread_mutex_unlock(&m->lock);
	if (left < 0)
		return fail(error, HULL_ERR_GONE, "the machine's processes cannot be read");
	return left == 0 ? HULL_OK : fail(error, HULL_ERR_RUNTIME, "the machine's processes would not stop");
}

/* Ends the command, if it has not ended, and frees the stream; on_exit
 * has been or is then reported. */
/* What the host spends on the machine: every process in its pid
 * namespace, and the execs that have ended. Read under the lock, so that
 * the init's pid is still the init's. */
static hull_status chroot_stats(hull_core *core, hull_machine *m, hull_buf *out,
                                hull_error *error)
{
	hull_usage usage;
	int        read_all;

	(void)core;
	pthread_mutex_lock(&m->lock);
	usage = m->spent;
	read_all = m->started && !m->done && hull_usage_namespace(m->pid, &usage);
	pthread_mutex_unlock(&m->lock);
	if (!read_all)
		return fail(error, HULL_ERR_GONE, "the machine is not running");
	out->data = m->stats;
	out->len = hull_usage_encode(&usage, m->stats, sizeof m->stats);
	return out->len == 0u ? fail(error, HULL_ERR_INTERNAL, "stats do not fit") : HULL_OK;
}

static hull_status chroot_stream_close(hull_core *core, hull_stream *st, hull_error *error)
{
	chroot_stream_eof(core, st, error);
	pthread_mutex_lock(&st->lock);
	if (st->dialed) {
		/* No helper to signal; wake read_dial, which is reading the socket,
		 * by shutting it. It leaves the fd for this to close after the join. */
		if (st->out >= 0)
			shutdown(st->out, SHUT_RDWR);
	} else if (!st->reaped) {
		kill(st->helper, SIGKILL);
	}
	pthread_mutex_unlock(&st->lock);
	pthread_join(st->reader, NULL);
	if (st->dialed && st->out >= 0)
		close(st->out);
	pthread_mutex_destroy(&st->lock);
	free(st);
	return HULL_OK;
}

/* --- the declaration ----------------------------------------------------- */

#define ARCH_NOTE "translated by the host's qemu-user, registered through binfmt_misc in the " \
                  "machine's own user namespace; the kernel is the host's, which answers " \
                  "its system calls"

static const hull_cap_entry entries[] = {
	/* The dir itself, bound onto itself and chrooted into; or beneath an
	 * overlayfs in the machine's mount namespace, whose upper layer is a
	 * tmpfs rather than section 3.7's upper/ on the host. */
	/* A stored tree reaches the plan as a dir; an image ref reaches it
	 * only where the store has none of that name. */
	{ "source.ref.overlay",     HULL_SUP_REJECTED,
	  "an image runs from the store, and this one is not in it: `hull pull` it first", NULL },
	{ "source.dir.direct",      HULL_SUP_NATIVE,   NULL, NULL },
	{ "source.dir.overlay",     HULL_SUP_EMULATED,
	  "an overlayfs in the machine's mount namespace; writes are held in memory and go with it",
	  NULL },
	{ "projection.bind",        HULL_SUP_NATIVE,   NULL, NULL },
	{ "writeback.none",         HULL_SUP_NATIVE,   NULL, NULL },
	{ "command",                HULL_SUP_NATIVE,   NULL, NULL },
	{ "workdir",                HULL_SUP_NATIVE,   NULL, NULL },
	{ "env",                    HULL_SUP_NATIVE,   "the environment is what env names, and docker's default PATH where it names none",
	  NULL },
	{ "hostname",               HULL_SUP_NATIVE,   NULL, NULL },
	{ "volumes",                HULL_SUP_NATIVE,
	  "a guest path that is not there is made above an overlay, and refused in a dir used "
	  "in place", NULL },
	{ "volumes.layers",         HULL_SUP_NATIVE,
	  "an overlay mounted at the guest path, staged in the machine's namespace", NULL },
	{ "secrets",                HULL_SUP_NATIVE,
	  "on a tmpfs the machine mounts at /run, which the dir must have; one exposed is "
	  "in the env of the workload and of every exec", NULL },
	{ "security.readonly_root", HULL_SUP_NATIVE,   NULL, NULL },
	{ "security.no_new_privs",  HULL_SUP_NATIVE,   NULL, NULL },
	{ "on_disconnect.stop",     HULL_SUP_NATIVE,   NULL, NULL },
	{ "on_disconnect.keep",     HULL_SUP_NATIVE,
	  "kept, and taken back by a core that comes after one that went; its init holds "
	  "the workload's last 256 KiB meanwhile", NULL },
	{ "ports",                  HULL_SUP_NATIVE,
	  "forwarded by pasta from the machine's 127.107 address into its own network; refused "
	  "with (network host) or (network none)", NULL },
	{ "network.host",           HULL_SUP_NATIVE,
	  "the host's own network, asked for: no key is a network of the machine's own, pasta's "
	  "NAT out", NULL },
	{ "network.bridge",         HULL_SUP_NATIVE,
	  "a veth on hull's own bridge, hull0, NATed out, its ports forwarded by pasta from its "
	  "127.107 address, or on the host bridge (bridge NAME) names; only with hull's core run "
	  "as root, refused at create otherwise", NULL },
	{ "bridge",                 HULL_SUP_NATIVE,
	  "the host bridge (network bridge) joins, there already; the machine addresses itself",
	  NULL },
	{ "network.nat",            HULL_SUP_NATIVE,
	  "a network namespace of the machine's own, pasta's NAT out, as no network key gives",
	  NULL },
	{ "network.none",           HULL_SUP_NATIVE,
	  "a network namespace of the machine's own, with lo alone", NULL },
	{ "devices.usb",            HULL_SUP_NATIVE,
	  "its usbfs node, found by sysfs at start and bound into /dev", NULL },
	{ "devices.serial",         HULL_SUP_NATIVE,   "the host's node, bound into /dev", NULL },
	{ "devices.parallel",       HULL_SUP_NATIVE,   "the host's node, bound into /dev", NULL },
	{ "devices.node",           HULL_SUP_NATIVE,   "the host's node, bound into /dev", NULL },
	{ "devices.pci",            HULL_SUP_NATIVE,
	  "its VFIO group's node and /dev/vfio/vfio bound in, for a driver in user space", NULL },
	{ "cpus",                   HULL_SUP_NATIVE,
	  "a systemd scope's CPUQuota, N CPUs' time a second", NULL },
	{ "memory",                 HULL_SUP_NATIVE,
	  "a ceiling, a systemd scope's MemoryMax", NULL },
	{ "ssh",                    HULL_SUP_NATIVE,
	  "the image's own sshd, supervised at a high port on the machine's 127.107 address, "
	  "bound there on the host network and forwarded there by pasta under NAT; refused with "
	  "(network none) and at start if the image has no sshd", NULL },
	{ "segments",               HULL_SUP_NATIVE,
	  "a tap each in the machine's own network namespace, relayed to the core's switch: eth1 "
	  "on after pasta's eth0 under NAT, eth0 on with (network none); refused with (network "
	  "host)", NULL },
	{ "impair",                 HULL_SUP_NATIVE,
	  "in the core's switch, each way of the machine's port on a segment, in netem's terms", NULL },
	{ "tamper",                 HULL_SUP_NATIVE,
	  "in the core's switch, a targeted fault on each way of the machine's port on a segment", NULL },
	{ "export",                 HULL_SUP_NATIVE,
	  "a tar of the overlay's tree when the workload ends, owners as the machine saw them",
	  NULL },
	{ "layers",                 HULL_SUP_NATIVE,
	  "read-only, in the machine's overlay, each above the last", NULL },
	{ "upper",                  HULL_SUP_NATIVE,
	  "the overlay's writes kept in the directory, one uid, its work directory hidden beside it",
	  NULL },
	{ "owners",                 HULL_SUP_NATIVE,
	  "given in the overlay before the workload, where many uids are mapped", NULL },
	/* A foreign arch, its programs translated on the host's kernel. */
	{ "arch.arm64",             HULL_SUP_EMULATED, ARCH_NOTE, NULL },
	{ "arch.arm",               HULL_SUP_EMULATED, ARCH_NOTE, NULL },
	{ "arch.riscv64",           HULL_SUP_EMULATED, ARCH_NOTE, NULL },
	{ "arch.ppc64le",           HULL_SUP_EMULATED, ARCH_NOTE, NULL },
	{ "arch.s390x",             HULL_SUP_EMULATED, ARCH_NOTE, NULL },
	{ "init",                   HULL_SUP_NATIVE,
	  "the image's init is PID 1, the engine's a child of it, and its poweroff ends the machine",
	  NULL },
	{ NULL,                     HULL_SUP_NATIVE,   NULL, NULL },
};

static const char *const classes[] = { "ns", NULL };

static const hull_capabilities caps = {
	.struct_size    = sizeof (hull_capabilities),
	.engine_name    = "chroot",
	.engine_version = "0",
	.classes        = classes,
	.entries        = entries,
	.migration      = HULL_MIG_PINNED,  /* "practically: don't" (H.3) */
};

static const hull_engine_ops ops = {
	.struct_size = sizeof (hull_engine_ops),
	.create      = chroot_create,
	.start       = chroot_start,
	.stop        = chroot_stop,
	.destroy     = chroot_destroy,
	.adopt        = chroot_adopt,
	.persist      = chroot_persist,
	.exec         = chroot_exec,
	.stream_write = chroot_stream_write,
	.stream_eof   = chroot_stream_eof,
	.stream_close = chroot_stream_close,
	.resize       = chroot_resize,
	.signal       = chroot_signal,
	.stats        = chroot_stats,
	.pause        = chroot_pause,
	.dial         = chroot_dial,
};

static const hull_engine engine = {
	.struct_size = sizeof (hull_engine),
	.abi_major   = HULL_ABI_MAJOR,
	.abi_minor   = HULL_ABI_MINOR,
	.caps        = &caps,
	.ops         = &ops,
};

const hull_engine *hull_engine_chroot(const hull_host_ops *host_ops)
{
	host = host_ops;
	return &engine;
}
