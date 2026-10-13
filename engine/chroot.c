/*
 * chroot.c -- the reference engine: a dir source run in place, rootless
 * (project.md sections 5.0 and 4.8).
 *
 * A machine is one process, started in new user, mount and UTS namespaces
 * with the invoking user mapped to root, bound onto its own directory and
 * chrooted into it. That is enough for the fields declared below and no
 * more: no network namespace, so a port is the host's own, and no PID
 * namespace or /proc.
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
#include <sys/mount.h>
#include <sys/prctl.h>
#include <sys/statvfs.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "builtin.h"
#include "sexpr.h"

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
	int              no_new_privs;
	char            *volume_host[MAX_VOLUMES];
	char            *volume_guest[MAX_VOLUMES];
	int              volume_readonly[MAX_VOLUMES];
	unsigned         volume_count;

	pid_t            pid;
	int              out;        /* the workload's stdout and stderr, read ends */
	int              err;
	pthread_t        reader;
	int              started;
	pthread_mutex_t  lock;
	pthread_cond_t   done_cond;
	int              done;
	char             detail[256];
};

static const hull_host_ops *host;

/* --- reading the plan ---------------------------------------------------- */

static int find(const hull_sexpr_doc *doc, const hull_sexpr_form *list, const char *key,
                hull_sexpr_form *entry)
{
	hull_sexpr_form item;
	hull_sexpr_form name;
	int             more;

	if (!hull_sexpr_child(doc, list, &item))
		return 0;
	for (more = hull_sexpr_next(doc, &item, &item); more;
	     more = hull_sexpr_next(doc, &item, &item)) {
		if (item.kind == HULL_SEXPR_LIST && hull_sexpr_child(doc, &item, &name) &&
		    hull_sexpr_is(doc, &name, key)) {
			*entry = item;
			return 1;
		}
	}
	return 0;
}

/* An atom's bytes as a C string, or NULL if it holds a NUL. */
static char *text_of(const hull_sexpr_doc *doc, const hull_sexpr_form *form)
{
	char *text;

	if (form->kind != HULL_SEXPR_ATOM || memchr(doc->data + form->content, '\0',
	                                            form->content_length) != NULL)
		return NULL;
	text = malloc((size_t)form->content_length + 1u);
	if (text == NULL)
		return NULL;
	memcpy(text, doc->data + form->content, form->content_length);
	text[form->content_length] = '\0';
	return text;
}

/* The value of `(key value)` in `record`, as a string, or NULL. */
static char *value_of(const hull_sexpr_doc *doc, const hull_sexpr_form *record, const char *key)
{
	hull_sexpr_form entry;
	hull_sexpr_form head;
	hull_sexpr_form value;

	if (!find(doc, record, key, &entry) || !hull_sexpr_child(doc, &entry, &head) ||
	    !hull_sexpr_next(doc, &head, &value))
		return NULL;
	return text_of(doc, &value);
}

static int is_true(const hull_sexpr_doc *doc, const hull_sexpr_form *record, const char *key)
{
	char *value = value_of(doc, record, key);
	int   yes   = value != NULL && strcmp(value, "true") == 0;

	free(value);
	return yes;
}

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
	hull_sexpr_form plan;
	hull_sexpr_form machine;
	hull_sexpr_form entry;
	hull_sexpr_form item;
	char           *source;
	unsigned        n;
	int             more;

	if (!hull_sexpr_first(doc, &plan) || !find(doc, &plan, "machine", &machine))
		return "the plan holds no machine";
	source = value_of(doc, &machine, "source");
	if (source == NULL || strncmp(source, "dir:", 4u) != 0 || source[4] == '\0') {
		free(source);
		return "chroot runs only a dir source";
	}
	m->root = strdup(source + 4);
	free(source);
	m->workdir  = value_of(doc, &machine, "workdir");
	m->hostname = value_of(doc, &machine, "hostname");
	if (m->root == NULL || m->workdir == NULL)
		return "the plan is missing its source or workdir";

	if (!find(doc, &machine, "command", &entry))
		return "chroot has no manifest to supply a command, so the spec needs (command ...)";
	hull_sexpr_child(doc, &entry, &item);
	n = 0u;
	for (more = hull_sexpr_next(doc, &item, &item); more;
	     more = hull_sexpr_next(doc, &item, &item)) {
		if (n == MAX_ARGS)
			return "the command has too many arguments";
		m->argv[n] = text_of(doc, &item);
		if (m->argv[n++] == NULL)
			return "an argument holds a NUL byte";
	}

	n = 0u;
	if (find(doc, &machine, "env", &entry)) {
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
			key  = text_of(doc, &name);
			text = text_of(doc, &value);
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

	if (find(doc, &machine, "volumes", &entry)) {
		hull_sexpr_child(doc, &entry, &item);
		for (more = hull_sexpr_next(doc, &item, &item); more;
		     more = hull_sexpr_next(doc, &item, &item)) {
			char *mode;

			if (m->volume_count == MAX_VOLUMES)
				return "there are too many volumes";
			m->volume_host[m->volume_count]  = value_of(doc, &item, "host");
			m->volume_guest[m->volume_count] = value_of(doc, &item, "guest");
			mode = value_of(doc, &item, "mode");
			m->volume_readonly[m->volume_count] = mode != NULL && strcmp(mode, "ro") == 0;
			free(mode);
			if (m->volume_host[m->volume_count] == NULL ||
			    m->volume_guest[m->volume_count++] == NULL)
				return "a volume is missing its host or guest path";
		}
	}

	if (find(doc, &machine, "security", &entry)) {
		m->readonly_root = is_true(doc, &entry, "readonly_root");
		m->no_new_privs  = is_true(doc, &entry, "no_new_privs");
	}
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

static void run_child(const hull_machine *m, uid_t uid, gid_t gid, int out, int err, int report)
{
	char map[64];
	char step[320];
	int  volumes[MAX_VOLUMES];
	int  null = open("/dev/null", O_RDONLY | O_CLOEXEC);

	if (null < 0 || dup2(null, 0) < 0 || dup2(out, 1) < 0 || dup2(err, 2) < 0)
		child_fail(report, "stdio");
	/* A session of its own, so a terminal's Ctrl-C reaches hull and not the
	 * workload behind its back: only stop() signals a machine. */
	if (setsid() < 0)
		child_fail(report, "setsid");
	if (unshare(CLONE_NEWUSER | CLONE_NEWNS | CLONE_NEWUTS) != 0)
		child_fail(report, "unshare");
	snprintf(map, sizeof map, "0 %u 1", (unsigned)uid);
	if (write_file("/proc/self/setgroups", "deny") != 0 ||
	    write_file("/proc/self/uid_map", map) != 0)
		child_fail(report, "uid_map");
	snprintf(map, sizeof map, "0 %u 1", (unsigned)gid);
	if (write_file("/proc/self/gid_map", map) != 0)
		child_fail(report, "gid_map");
	if (mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL) != 0)
		child_fail(report, "making mounts private");
	if (mount(m->root, m->root, NULL, MS_BIND | MS_REC, NULL) != 0)
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
	if (chroot(m->root) != 0)
		child_fail(report, "chroot");
	for (unsigned i = 0u; i < m->volume_count; i++) {
		snprintf(step, sizeof step, "volume %s", m->volume_guest[i]);
		/* A symlink at the guest path is followed, inside the machine. */
		if (move_mount(volumes[i], "", AT_FDCWD, m->volume_guest[i],
		               MOVE_MOUNT_F_EMPTY_PATH | MOVE_MOUNT_T_SYMLINKS) != 0)
			child_fail(report, step);
	}
	if (chdir(m->workdir) != 0)
		child_fail(report, "workdir");
	if (m->no_new_privs && prctl(PR_SET_NO_NEW_PRIVS, 1L, 0L, 0L, 0L) != 0)
		child_fail(report, "no_new_privs");
	execve(m->argv[0], (char *const *)m->argv, (char *const *)m->envp);
	child_fail(report, m->argv[0]);
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
	while (waitpid(m->pid, &status, 0) < 0 && errno == EINTR)
		;
	pthread_mutex_lock(&m->lock);
	m->done = 1;
	pthread_cond_broadcast(&m->done_cond);
	pthread_mutex_unlock(&m->lock);
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
	ssize_t n;

	if (m->started)
		return fail(error, HULL_ERR_BUSY, "the machine has been started already");
	if (pipe2(out, O_CLOEXEC) != 0 || pipe2(err, O_CLOEXEC) != 0 ||
	    pipe2(report, O_CLOEXEC) != 0)
		return fail(error, HULL_ERR_RUNTIME, "pipe");
	m->pid = fork();
	if (m->pid < 0)
		return fail(error, HULL_ERR_RUNTIME, "fork");
	if (m->pid == 0) {
		close(report[0]);
		run_child(m, getuid(), getgid(), out[1], err[1], report[1]);
	}
	close(out[1]);
	close(err[1]);
	close(report[1]);

	/* The report pipe closes on exec; anything written on it is why the
	 * child could not get that far. */
	n = read(report[0], m->detail, sizeof m->detail - 1u);
	close(report[0]);
	if (n > 0) {
		m->detail[n] = '\0';
		waitpid(m->pid, NULL, 0);
		close(out[0]);
		close(err[0]);
		m->done = 1;
		return fail(error, HULL_ERR_RUNTIME, m->detail);
	}
	m->out = out[0];
	m->err = err[0];
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

/* SIGTERM, and SIGKILL if that is not enough within `timeout_ms`. */
static hull_status chroot_stop(hull_core *core, hull_machine *m, int timeout_ms,
                               hull_error *error)
{
	(void)core;
	(void)error;
	if (!m->started || wait_done(m, 0))
		return HULL_OK;
	host->on_state(core, m, "stopping");
	kill(m->pid, SIGTERM);
	if (!wait_done(m, timeout_ms)) {
		kill(m->pid, SIGKILL);
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

/* --- the declaration ----------------------------------------------------- */

static const hull_cap_entry entries[] = {
	/* The dir itself, bound onto itself and chrooted into. */
	{ "source.dir.direct",      HULL_SUP_NATIVE,   NULL, NULL },
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
