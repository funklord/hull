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
 * The workload's output goes to the core through on_data (hull_engine.h,
 * rule 3), read by one thread per machine that also waits for it and
 * reports on_exit. Every engine-owned allocation is the machine handle's,
 * freed by destroy().
 */

#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include "access.h"
#include "builtin.h"
#include "usage.h"

#define MAX_ARGS    256u
#define MAX_ENV     256u
#define MAX_VOLUMES 64u

struct hull_machine {
	hull_core       *core;       /* what create() was given, for the callbacks */
	char            *root;       /* the dir source, without "dir:" */
	char            *workdir;
	char            *hostname;   /* or NULL */
	char            *argv[MAX_ARGS + 1u];
	char            *envp[MAX_ENV + 1u];
	int              readonly_root;
	int              overlay;    /* (mutable overlay), and the root not read-only */
	int              no_new_privs;
	char            *volume_host[MAX_VOLUMES];
	char            *volume_guest[MAX_VOLUMES];
	int              volume_readonly[MAX_VOLUMES];
	unsigned         volume_count;

	pid_t            pid;        /* the machine's init */
	int              out;        /* the workload's stdout and stderr, read ends */
	int              err;
	int              status;     /* the read end of the init's status pipe */
	int              status_seen;
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
	for (unsigned i = 0u; m->argv[i] != NULL; i++)
		free(m->argv[i]);
	for (unsigned i = 0u; m->envp[i] != NULL; i++)
		free(m->envp[i]);
	for (unsigned i = 0u; i < m->volume_count; i++) {
		free(m->volume_host[i]);
		free(m->volume_guest[i]);
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
		return "chroot has no manifest to supply a command, so the spec needs (command ...)";
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
			char *mode;
			char *host_path;

			if (m->volume_count == MAX_VOLUMES)
				return "there are too many volumes";
			host_path = hull_access_value(doc, &item, "host");
			m->volume_host[m->volume_count] =
			    host_path != NULL ? hull_access_path(doc, host_path) : NULL;
			free(host_path);
			m->volume_guest[m->volume_count] = hull_access_value(doc, &item, "guest");
			mode = hull_access_value(doc, &item, "mode");
			m->volume_readonly[m->volume_count] = mode != NULL && strcmp(mode, "ro") == 0;
			free(mode);
			if (m->volume_host[m->volume_count] == NULL ||
			    m->volume_guest[m->volume_count++] == NULL)
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
	return NULL;
}

/* --- the child ----------------------------------------------------------- */

/* Tell the parent which step failed, and why, then leave. */
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

	if (tree < 0 || fstat(tree, &lower) != 0)
		child_fail(report, "the overlay's lower layer");
	if (mount("tmpfs", m->root, "tmpfs", 0u, "mode=0700") != 0 || chdir(m->root) != 0 ||
	    mkdir("lower", 0700) != 0 || mkdir("upper", 0700) != 0 ||
	    mkdir("work", 0700) != 0 || mkdir("merged", 0700) != 0)
		child_fail(report, "the overlay's tmpfs");
	if (move_mount(tree, "", AT_FDCWD, "lower", MOVE_MOUNT_F_EMPTY_PATH) != 0)
		child_fail(report, "the overlay's lower layer");
	close(tree);
	/* No chown: the namespace maps the user and the user's group, to root,
	 * who made upper/, so the directory's owner and group are root's here
	 * already or are not mapped and cannot be given. */
	if (chmod("upper", lower.st_mode & 07777) != 0)
		child_fail(report, "the overlay's upper layer");
	if (mount("overlay", "merged", "overlay", 0u,
	          "lowerdir=lower,upperdir=upper,workdir=work,userxattr") != 0)
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
static void run_init(const hull_machine *m, uid_t uid, gid_t gid, int out, int err, int report,
                     int status_pipe)
{
	static const int forwarded[] = { SIGTERM, SIGINT, SIGHUP, SIGQUIT, SIGUSR1, SIGUSR2 };
	struct sigaction action;
	struct stat      st;
	char             map[64];
	char             step[320];
	int              volumes[MAX_VOLUMES];
	int              null = open("/dev/null", O_RDONLY | O_CLOEXEC);
	int              status = 0;
	pid_t            workload;

	if (null < 0 || dup2(null, 0) < 0 || dup2(out, 1) < 0 || dup2(err, 2) < 0)
		child_fail(report, "stdio");
	{
		int keep[2] = { report, status_pipe };

		close_others(keep, 2u);
	}
	/* The machine dies with the process that runs its engine, the core:
	 * nothing could manage it after, since adopt() is not written, and
	 * nothing is to outlive what started it. */
	if (prctl(PR_SET_PDEATHSIG, SIGKILL, 0L, 0L, 0L) != 0)
		child_fail(report, "the parent-death signal");
	/* A session of its own, so a terminal's Ctrl-C reaches hull and not the
	 * machine behind its back: only stop() signals a machine. */
	if (setsid() < 0)
		child_fail(report, "setsid");
	snprintf(map, sizeof map, "0 %u 1", (unsigned)uid);
	if (write_file("/proc/self/setgroups", "deny") != 0 ||
	    write_file("/proc/self/uid_map", map) != 0)
		child_fail(report, "uid_map");
	snprintf(map, sizeof map, "0 %u 1", (unsigned)gid);
	if (write_file("/proc/self/gid_map", map) != 0)
		child_fail(report, "gid_map");
	if (mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL) != 0)
		child_fail(report, "making mounts private");
	if (!m->overlay && mount(m->root, m->root, NULL, MS_BIND | MS_REC, NULL) != 0)
		child_fail(report, "binding the root");
	if (m->readonly_root &&
	    mount(NULL, m->root, NULL, MS_BIND | MS_REMOUNT | MS_RDONLY | locked_flags(m->root),
	          NULL) != 0)
		child_fail(report, "making the root read-only");
	for (unsigned i = 0u; i < m->volume_count; i++) {
		struct mount_attr readonly = { .attr_set = MOUNT_ATTR_RDONLY };

		snprintf(step, sizeof step, "volume %s", m->volume_host[i]);
		volumes[i] = open_tree(AT_FDCWD, m->volume_host[i],
		                       OPEN_TREE_CLONE | OPEN_TREE_CLOEXEC | AT_RECURSIVE);
		if (volumes[i] < 0)
			child_fail(report, step);
		if (m->volume_readonly[i] &&
		    mount_setattr(volumes[i], "", AT_EMPTY_PATH | AT_RECURSIVE, &readonly,
		                  sizeof readonly) != 0)
			child_fail(report, step);
	}
	if (m->hostname != NULL && sethostname(m->hostname, strlen(m->hostname)) != 0)
		child_fail(report, "sethostname");
	if (m->overlay)
		layer(m, report);
	if (m->overlay ? chroot("merged") != 0 : chroot(m->root) != 0)
		child_fail(report, "chroot");
	for (unsigned i = 0u; i < m->volume_count; i++) {
		snprintf(step, sizeof step, "volume %s", m->volume_guest[i]);
		/* A symlink at the guest path is followed, inside the machine. */
		if (move_mount(volumes[i], "", AT_FDCWD, m->volume_guest[i],
		               MOVE_MOUNT_F_EMPTY_PATH | MOVE_MOUNT_T_SYMLINKS) != 0)
			child_fail(report, step);
	}
	/* This namespace's processes, where the dir has room for them: nothing
	 * is created in the user's dir. */
	if (stat("/proc", &st) == 0 && S_ISDIR(st.st_mode) &&
	    mount("proc", "/proc", "proc", MS_NOSUID | MS_NODEV | MS_NOEXEC, NULL) != 0)
		child_fail(report, "mounting /proc");
	if (chdir(m->workdir) != 0)
		child_fail(report, "workdir");
	if (m->no_new_privs && prctl(PR_SET_NO_NEW_PRIVS, 1L, 0L, 0L, 0L) != 0)
		child_fail(report, "no_new_privs");

	memset(&action, 0, sizeof action);
	action.sa_handler = hold;
	for (unsigned i = 0u; i < sizeof forwarded / sizeof forwarded[0]; i++)
		sigaction(forwarded[i], &action, NULL);
	workload = fork();
	if (workload < 0)
		child_fail(report, "fork");
	if (workload == 0) {
		action.sa_handler = SIG_DFL;
		for (unsigned i = 0u; i < sizeof forwarded / sizeof forwarded[0]; i++)
			sigaction(forwarded[i], &action, NULL);
		if (setsid() < 0)
			child_fail(report, "setsid");
		execve(m->argv[0], (char *const *)m->argv, (char *const *)m->envp);
		child_fail(report, m->argv[0]);
	}
	/* The engine reads `report` to its end: this copy, and the workload's
	 * once it execs. */
	close(report);
	for (;;) {
		int   s;
		pid_t reaped;

		if (pending != 0) {
			kill(workload, (int)pending);
			pending = 0;
		}
		reaped = waitpid(-1, &s, 0);
		if (reaped < 0 && errno == EINTR)
			continue;
		if (reaped < 0)
			break;
		if (reaped == workload) {
			status = s;
			break;
		}
	}
	if (write(status_pipe, &status, sizeof status) != (ssize_t)sizeof status)
		_exit(126);
	_exit(0);
}

/* --- the reader ---------------------------------------------------------- */

static void *read_workload(void *arg)
{
	hull_machine *m = arg;
	struct pollfd fds[2] = { { m->out, POLLIN, 0 }, { m->err, POLLIN, 0 } };
	uint8_t       buffer[4096];
	int           open_count = 2;
	int           status;

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
				host->on_data(m->core, NULL, i + 1, buffer, (size_t)n);
			} else if (n == 0 || errno != EINTR) {
				close(fds[i].fd);
				fds[i].fd = -1;
				open_count--;
			}
		}
	}
	/* Waited for without reaping, then reaped and marked done under the
	 * lock, so that stop() signals the init's pid only while it is the
	 * init's or its zombie's. */
	{
		siginfo_t info;

		while (waitid(P_PID, (id_t)m->pid, &info, WEXITED | WNOWAIT) < 0 && errno == EINTR)
			;
	}
	pthread_mutex_lock(&m->lock);
	waitpid(m->pid, &status, 0);
	m->done = 1;
	pthread_cond_broadcast(&m->done_cond);
	pthread_mutex_unlock(&m->lock);
	/* The workload's status, as its init reported it; if the init could
	 * not, as when stop killed it, the init's own. */
	if (read(m->status, &m->status_seen, sizeof m->status_seen) == (ssize_t)sizeof m->status_seen)
		status = m->status_seen;
	close(m->status);
	if (WIFSIGNALED(status))
		host->on_exit(m->core, m, NULL, -1, WTERMSIG(status));
	else
		host->on_exit(m->core, m, NULL, WEXITSTATUS(status), 0);
	host->on_state(m->core, m, "stopped");
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
	m->core = core;
	pthread_mutex_init(&m->lock, NULL);
	pthread_cond_init(&m->done_cond, NULL);
	reason = read_plan(m, &doc);
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
	int     out[2];
	int     err[2];
	int     report[2];
	int     status[2];
	uid_t   uid = getuid();
	gid_t   gid = getgid();
	ssize_t n;

	if (m->started)
		return fail(error, HULL_ERR_BUSY, "the machine has been started already");
	if (pipe2(out, O_CLOEXEC) != 0 || pipe2(err, O_CLOEXEC) != 0 ||
	    pipe2(report, O_CLOEXEC) != 0 || pipe2(status, O_CLOEXEC) != 0)
		return fail(error, HULL_ERR_RUNTIME, "pipe");
	/* clone with no stack is fork into new namespaces: the child is PID 1
	 * of its own. The ids are read first, since in the child's new user
	 * namespace they are unmapped until it maps them. */
	m->pid = (pid_t)syscall(SYS_clone, CLONE_NEWUSER | CLONE_NEWNS | CLONE_NEWUTS |
	                        CLONE_NEWPID | SIGCHLD, NULL, NULL, NULL, 0L);
	if (m->pid < 0)
		return fail(error, HULL_ERR_RUNTIME, "clone");
	if (m->pid == 0) {
		close(report[0]);
		close(status[0]);
		run_init(m, uid, gid, out[1], err[1], report[1], status[1]);
	}
	close(out[1]);
	close(err[1]);
	close(report[1]);
	close(status[1]);
	m->status = status[0];

	/* The report pipe closes on exec; anything written on it is why the
	 * child could not get that far. */
	n = read(report[0], m->detail, sizeof m->detail - 1u);
	close(report[0]);
	if (n > 0) {
		m->detail[n] = '\0';
		waitpid(m->pid, NULL, 0);
		close(out[0]);
		close(err[0]);
		close(m->status);
		m->done = 1;
		return fail(error, HULL_ERR_RUNTIME, m->detail);
	}
	m->out = out[0];
	m->err = err[0];
	if (pthread_create(&m->reader, NULL, read_workload, m) != 0) {
		kill(m->pid, SIGKILL);
		waitpid(m->pid, NULL, 0);
		close(m->status);
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
	if (!m->done)
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
	release(m);
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
	static const int kinds[] = { CLONE_NEWUSER, CLONE_NEWNS, CLONE_NEWUTS, CLONE_NEWPID };
	static const int forwarded[] = { SIGTERM, SIGINT, SIGHUP, SIGQUIT, SIGUSR1, SIGUSR2 };
	struct sigaction action;
	int              status = 0;
	pid_t            command;
	int              keep[11] = { ns[0], ns[1], ns[2], ns[3], root, in, out, err, report,
	                              status_pipe, -1 };

	/* Held from the start: a signal before the command exists is
	 * forwarded once it does, rather than ending the helper. */
	memset(&action, 0, sizeof action);
	action.sa_handler = hold;
	for (unsigned i = 0u; i < sizeof forwarded / sizeof forwarded[0]; i++)
		sigaction(forwarded[i], &action, NULL);
	close_others(keep, 10u);
	for (unsigned i = 0u; i < 4u; i++)
		if (setns(ns[i], kinds[i]) != 0)
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
		execve(argv[0], argv, (char *const *)m->envp);
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
	static const char *const kinds[] = { "user", "mnt", "uts", "pid" };
	char        *argv[MAX_ARGS + 1u] = { NULL };
	char         path[64];
	int          ns[4] = { -1, -1, -1, -1 };
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
	for (unsigned i = 0u; i < 4u; i++) {
		snprintf(path, sizeof path, "/proc/%ld/ns/%s", (long)m->pid, kinds[i]);
		ns[i] = open(path, O_RDONLY | O_CLOEXEC);
	}
	snprintf(path, sizeof path, "/proc/%ld/root", (long)m->pid);
	root = open(path, O_PATH | O_DIRECTORY | O_CLOEXEC);
	st = calloc(1u, sizeof *st);
	opened = tty ? open_terminal(&size, in, o, e) : pipe2(in, O_CLOEXEC) == 0 &&
	         pipe2(o, O_CLOEXEC) == 0 && pipe2(e, O_CLOEXEC) == 0;
	if (ns[0] < 0 || ns[1] < 0 || ns[2] < 0 || ns[3] < 0 || root < 0 || st == NULL ||
	    !opened || pipe2(report, O_CLOEXEC) != 0 || pipe2(status, O_CLOEXEC) != 0) {
		const int ends[] = { ns[0], ns[1], ns[2], ns[3], root, in[0], in[1], o[0], o[1],
		                     e[0], e[1] };

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
	for (unsigned i = 0u; i < 4u; i++)
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
	{ "source.dir.direct",      HULL_SUP_NATIVE,   NULL, NULL },
	{ "source.dir.overlay",     HULL_SUP_EMULATED,
	  "an overlayfs in the machine's mount namespace; writes are held in memory and go with it",
	  NULL },
	{ "projection.bind",        HULL_SUP_NATIVE,   NULL, NULL },
	{ "writeback.none",         HULL_SUP_NATIVE,   NULL, NULL },
	{ "command",                HULL_SUP_NATIVE,   "a path: there is no PATH search", NULL },
	{ "workdir",                HULL_SUP_NATIVE,   NULL, NULL },
	{ "env",                    HULL_SUP_NATIVE,   "the environment is only what env names",
	  NULL },
	{ "hostname",               HULL_SUP_NATIVE,   NULL, NULL },
	{ "volumes",                HULL_SUP_NATIVE,
	  "the guest path must exist in the dir; it is not created", NULL },
	{ "security.readonly_root", HULL_SUP_NATIVE,   NULL, NULL },
	{ "security.no_new_privs",  HULL_SUP_NATIVE,   NULL, NULL },
	{ "on_disconnect.stop",     HULL_SUP_NATIVE,   NULL, NULL },
	{ "on_disconnect.keep",     HULL_SUP_NATIVE,
	  "kept while the core runs; the machine dies with the core", NULL },
	{ "ports",                  HULL_SUP_REJECTED,
	  "chroot shares the host's network, so a port is the host's own", NULL },
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
