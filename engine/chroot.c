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
#include <grp.h>
#include <fcntl.h>
#include <linux/if_tun.h>
#include <net/if.h>
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
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include "access.h"
#include "tree.h"
#include "builtin.h"
#include "usage.h"
#include "stack.h"

#define MAX_ARGS    256u
#define MAX_ENV     256u
#define MAX_VOLUMES 64u
#define MAX_SEGMENTS 8u
#define HELD_LIMIT  (256u * 1024u)  /* output an init holds for no core */

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
	char            *segment[MAX_SEGMENTS];     /* Appendix D's, by name */
	int              segment_fd[MAX_SEGMENTS];  /* their ports, until the init has them */
	unsigned         segment_count;
	char             address_text[64];          /* the plan's, for each NIC's MAC */
	int              adopted;    /* taken back: the init is not this process's child */
	int              pidfd;      /* the init's, when adopted, else -1 */
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
	free(m->root);
	free(m->workdir);
	free(m->hostname);
	free(m->secrets);
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
	if (m->root == NULL || m->workdir == NULL)
		return "the plan is missing its source or workdir";

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

	/* (network host), or none given, is the host's own, as it always was;
	 * (network none) is a namespace of the machine's own, lo alone. */
	source = hull_access_value(doc, &machine, "network");
	m->no_network = source != NULL && strcmp(source, "none") == 0;
	free(source);

	/* (segments ...): a NIC on each, eth0, eth1, ..., in the machine's own
	 * network namespace, which only (network none) gives it: a tap needs a
	 * namespace the user's own, and the host's is not. So named, a spec
	 * keeps its NICs' names on a vm of (network none), where no passt NIC
	 * comes first. A segment is the core's switch, which goes with it. */
	if (hull_access_find(doc, &machine, "segments", &entry)) {
		hull_sexpr_form top;

		hull_sexpr_child(doc, &entry, &item);
		for (more = hull_sexpr_next(doc, &item, &item); more;
		     more = hull_sexpr_next(doc, &item, &item)) {
			if (m->segment_count == MAX_SEGMENTS)
				return "there are too many segments";
			if ((m->segment[m->segment_count++] = hull_access_text(doc, &item)) == NULL)
				return "a segment's name is not text";
		}
		if (m->segment_count != 0u && !m->no_network)
			return "a chroot machine on a segment has a network of its own: (network none) "
			       "beside (segments ...)";
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

static void child_fail(int report, const char *step)
{
	char   message[200];
	int    n = snprintf(message, sizeof message, "%s: %s", step, strerror(errno));
	size_t length = n < 0 ? 0u : (size_t)n < sizeof message ? (size_t)n : sizeof message - 1u;

	if (write(report, message, length) < 0)
		_exit(127);
	_exit(127);
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
static void make_dev(int report)
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

/*
 * (owners PATH)'s list, read whole from `fd`: each entry `UID`, `GID` and
 * a path of the tree, each ended by a NUL, since a path may hold a
 * newline. Each is given its owner in the merged root, not following a
 * link. Those that cannot be, as where one uid is mapped, are counted
 * and said once on `err`, the workload's stderr, since a build that took
 * them silently would lose owners it seemed to keep.
 */
static void give_owners(int fd, int err)
{
	static char data[8u << 20];
	size_t      length = 0u;
	ssize_t     n;
	unsigned    refused = 0u;
	unsigned    given = 0u;

	while (length < sizeof data && (n = read(fd, data + length, sizeof data - length)) > 0)
		length += (size_t)n;
	close(fd);
	for (size_t at = 0u; at < length; ) {
		const char *fields[3];
		unsigned    k = 0u;
		char        path[4200];

		for (; k < 3u && at < length; k++) {
			fields[k] = data + at;
			while (at < length && data[at] != '\0')
				at++;
			if (at == length)
				break;
			at++;
		}
		if (k < 3u)
			break;
		snprintf(path, sizeof path, "/%s", fields[2]);
		if (fchownat(AT_FDCWD, path, (uid_t)strtoul(fields[0], NULL, 10),
		             (gid_t)strtoul(fields[1], NULL, 10), AT_SYMLINK_NOFOLLOW) == 0)
			given++;
		else
			refused++;
	}
	if (refused != 0u) {
		char said[200];
		int  m = snprintf(said, sizeof said, "hull: owners: %u of %u not given: %s\n", refused,
		                  refused + given, "the machine maps one uid, or a path is not there");

		if (m > 0 && write(err, said, (size_t)m) < 0) {
			/* the workload's stderr is gone; nothing to tell */
		}
	}
}

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
				_exit(0);
			if (idle && listener < 0)
				_exit(0);
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
 * The user's subordinate range in `file`, /etc/subuid or /etc/subgid: the
 * line naming the user, by name or by id, as `name:start:count`. 1 and
 * the range, or 0 where there is none.
 */
static int subordinate(const char *file, const char *name, unsigned long id, unsigned long *start,
                       unsigned long *count)
{
	FILE *f = fopen(file, "re");
	char  line[512];
	char  number[32];
	int   found = 0;

	if (f == NULL)
		return 0;
	snprintf(number, sizeof number, "%lu", id);
	while (!found && fgets(line, sizeof line, f) != NULL) {
		char *first = strtok(line, ":\n");
		char *from  = strtok(NULL, ":\n");
		char *many  = strtok(NULL, ":\n");

		if (first != NULL && from != NULL && many != NULL &&
		    (strcmp(first, name) == 0 || strcmp(first, number) == 0)) {
			*start = strtoul(from, NULL, 10);
			*count = strtoul(many, NULL, 10);
			found  = *count > 0u;
		}
	}
	fclose(f);
	return found;
}

/* `helper` (newuidmap or newgidmap) run on `pid`: 0 the user's own id, 1
 * on the range. Whether it mapped them. */
static int run_mapper(const char *helper, pid_t pid, unsigned long id, unsigned long start,
                      unsigned long count)
{
	char  text[5][32];
	char *argv[9];
	pid_t child;
	int   status = 0;

	snprintf(text[0], sizeof text[0], "%ld", (long)pid);
	snprintf(text[1], sizeof text[1], "%lu", id);
	snprintf(text[2], sizeof text[2], "%lu", start);
	snprintf(text[3], sizeof text[3], "%lu", count);
	argv[0] = (char *)helper;
	argv[1] = text[0];
	argv[2] = "0";
	argv[3] = text[1];
	argv[4] = "1";
	argv[5] = "1";
	argv[6] = text[2];
	argv[7] = text[3];
	argv[8] = NULL;
	child = fork();
	if (child == 0) {
		int null = open("/dev/null", O_RDWR | O_CLOEXEC);

		if (null >= 0) {
			dup2(null, 0);
			dup2(null, 1);
			dup2(null, 2);
		}
		execv(helper, argv);
		_exit(127);
	}
	return child > 0 && waitpid(child, &status, 0) == child && WIFEXITED(status) &&
	       WEXITSTATUS(status) == 0;
}

/*
 * Many uids for the machine `pid`, from outside it: where the user has a
 * subordinate range of each and the helpers that map one, 0 is the user
 * and 1 on the range, so a chown inside to an id the range holds works,
 * as a postinst's to its package's own user must. Whether it did.
 */
static int map_many(pid_t pid, uid_t uid, gid_t gid)
{
	struct passwd *me = getpwuid(uid);
	unsigned long  uid_start;
	unsigned long  uid_count;
	unsigned long  gid_start;
	unsigned long  gid_count;

	if (me == NULL ||
	    !subordinate("/etc/subuid", me->pw_name, (unsigned long)uid, &uid_start, &uid_count) ||
	    !subordinate("/etc/subgid", me->pw_name, (unsigned long)uid, &gid_start, &gid_count))
		return 0;
	return run_mapper("/usr/bin/newuidmap", pid, (unsigned long)uid, uid_start, uid_count) &&
	       run_mapper("/usr/bin/newgidmap", pid, (unsigned long)gid, gid_start, gid_count);
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

	for (unsigned i = 0u; i < m->segment_count; i++) {
		struct ifreq request = { 0 };
		int          fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);

		snprintf(request.ifr_name, sizeof request.ifr_name, "eth%u", i);
		request.ifr_flags = IFF_TAP | IFF_NO_PI;
		if (fd < 0 || (tap[i] = open("/dev/net/tun", O_RDWR | O_CLOEXEC)) < 0 ||
		    ioctl(tap[i], TUNSETIFF, &request) != 0)
			child_fail(report, "a segment's NIC");
		request.ifr_hwaddr.sa_family = 1; /* ARPHRD_ETHER */
		hull_access_segment_mac(m->address_text, m->segment[i], i,
		                        (unsigned char *)request.ifr_hwaddr.sa_data);
		if (ioctl(fd, SIOCSIFHWADDR, &request) != 0)
			child_fail(report, "a segment's NIC's address");
		close(fd);
	}
	relayer = fork();
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

		if (read(mapped, &said, 1u) != 1)
			child_fail(report, "the uid map");
		close(mapped);
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
			/* The user's own groups, unmapped here and so nobody's, go. */
			if (setgroups(0u, NULL) != 0)
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
	if (m->staged && mount("tmpfs", m->dir, "tmpfs", 0u, "mode=0700") != 0)
		child_fail(report, "the volumes' staging");
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
	if (m->hostname != NULL && sethostname(m->hostname, strlen(m->hostname)) != 0)
		child_fail(report, "sethostname");
	/* A namespace of its own starts with lo down: up, so that a dial to a
	 * port nothing holds is refused rather than unreachable. */
	if (m->no_network) {
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
		/* And its devices; a machine of the host's own / keeps the
		 * host's /dev, which is already its tree's. */
		if (!(now.st_dev == new_root.st_dev && now.st_ino == new_root.st_ino) &&
		    stat("dev", &st) == 0 && S_ISDIR(st.st_mode))
			make_dev(report);
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
		give_owners(owners_fd, err[1]);
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
	/* The secrets at /run/hull/secrets, J.3's place, on a tmpfs at /run:
	 * the dir's /run, which FHS has emptied at every boot, is hidden, and
	 * nothing is created in the user's dir. A dir with no /run cannot
	 * have them. */
	if (secrets >= 0) {
		if (mount("tmpfs", "/run", "tmpfs", MS_NOSUID | MS_NODEV, "mode=0755,size=1m") != 0)
			child_fail(report, "the secrets: a tmpfs at /run, which the dir must have");
		if (mkdir("/run/hull", 0755) != 0 || mkdir("/run/hull/secrets", 0700) != 0 ||
		    move_mount(secrets, "", AT_FDCWD, "/run/hull/secrets", MOVE_MOUNT_F_EMPTY_PATH) != 0)
			child_fail(report, "the secrets");
	}
	if (chdir(m->workdir) != 0)
		child_fail(report, "workdir");
	if (m->no_new_privs && prctl(PR_SET_NO_NEW_PRIVS, 1L, 0L, 0L, 0L) != 0)
		child_fail(report, "no_new_privs");

	memset(&action, 0, sizeof action);
	action.sa_handler = hold;
	for (unsigned i = 0u; i < sizeof forwarded / sizeof forwarded[0]; i++)
		sigaction(forwarded[i], &action, NULL);
	action.sa_handler = note_child;
	sigaction(SIGCHLD, &action, NULL);
	action.sa_handler = SIG_IGN;
	sigaction(SIGPIPE, &action, NULL);
	workload = fork();
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

static void *read_workload(void *arg)
{
	static uint8_t buffer[65536];
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

	if (m->started)
		return fail(error, HULL_ERR_BUSY, "the machine has been started already");
	/* A kept machine's directory, for its init.sock. */
	if ((m->keep || m->staged) &&
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
	                        CLONE_NEWPID | (m->no_network ? CLONE_NEWNET : 0) | SIGCHLD, NULL,
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
		m->many = wanted && map_many(m->pid, uid, gid);
	}
	if (write(mapped[1], m->many ? "M" : wanted ? "O" : "S", 1u) != 1) {
		/* The child reads its end closed and says so on the report. */
	}
	close(mapped[1]);
	if (dir_fd >= 0)
		close(dir_fd);
	m->conn = pair[0];

	/* The report pipe closes on exec; anything written on it is why the
	 * child could not get that far. */
	n = read(report[0], m->detail, sizeof m->detail - 1u);
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
static void signal_init(hull_machine *m, int signal)
{
	pthread_mutex_lock(&m->lock);
	if (!m->done && m->adopted)
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
	for (unsigned i = 0u; i < (m->no_network ? 5u : 4u); i++) {
		snprintf(path, sizeof path, "/proc/%ld/ns/%s", (long)m->pid, kinds[i]);
		ns[i] = open(path, O_RDONLY | O_CLOEXEC);
	}
	snprintf(path, sizeof path, "/proc/%ld/root", (long)m->pid);
	root = open(path, O_PATH | O_DIRECTORY | O_CLOEXEC);
	st = calloc(1u, sizeof *st);
	opened = tty ? open_terminal(&size, in, o, e) : pipe2(in, O_CLOEXEC) == 0 &&
	         pipe2(o, O_CLOEXEC) == 0 && pipe2(e, O_CLOEXEC) == 0;
	if (ns[0] < 0 || ns[1] < 0 || ns[2] < 0 || ns[3] < 0 || (m->no_network && ns[4] < 0) ||
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
	if (!st->reaped)
		kill(st->helper, signum);
	pthread_mutex_unlock(&st->lock);
	return HULL_OK;
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
	if (!st->reaped)
		kill(st->helper, SIGKILL);
	pthread_mutex_unlock(&st->lock);
	pthread_join(st->reader, NULL);
	pthread_mutex_destroy(&st->lock);
	free(st);
	return HULL_OK;
}

/* --- the declaration ----------------------------------------------------- */

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
	{ "ports",                  HULL_SUP_REJECTED,
	  "chroot shares the host's network, so a port is the host's own", NULL },
	{ "network.host",           HULL_SUP_NATIVE,   NULL, NULL },
	{ "network.none",           HULL_SUP_NATIVE,
	  "a network namespace of the machine's own, with lo alone", NULL },
	{ "segments",               HULL_SUP_NATIVE,
	  "a tap each, eth0 and on, in the machine's own network namespace, relayed to the core's "
	  "switch; with (network none)", NULL },
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
	{ "init",                   HULL_SUP_REJECTED,
	  "not yet: the machine's PID 1 is the engine's own, which reaps and forwards signals",
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
