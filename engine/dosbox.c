/*
 * dosbox.c -- the dos class's engine: a machine run by DOSBox, for
 * software written for DOS (project.md section 5.0).
 *
 * The machine's directory is drive C:, in place. The engine writes a
 * batch of its own, HULL.BAT, into a directory of its own under the
 * user's runtime directory, mounted as drive H:, and has DOSBox's
 * autoexec run it: SET for each env entry, C: and the workdir, the
 * command by CALL with what it writes through DOS redirected to
 * H:\OUT.TXT, then its ERRORLEVEL written to H:\STATUS.TXT by a cascade
 * of IF ERRORLEVEL, and EXIT. A reader thread hands OUT.TXT on as it
 * grows and once more when DOSBox has gone, then says the workload's end
 * from STATUS.TXT. CALL is what lets a batch for a command return to
 * HULL.BAT; DOS does not redirect a batch's own lines, so only what a
 * program writes through DOS is the machine's output, never what it
 * draws on the screen. A command DOS cannot find sets no ERRORLEVEL, so
 * the end is 127, as a shell's, where the output opens with DOSBox's
 * word for one.
 *
 * DOSBox runs as the user, with no window and no sound (SDL's dummy
 * drivers), its HOME the machine's directory so that it writes no
 * configuration into the user's, no_new_privs set, in a session of its
 * own, and killed with the core that started it. Each volume is a drive
 * of its own, its guest path the letter: /d is D:.
 */

#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "access.h"
#include "builtin.h"
#include "usage.h"

#define MAX_ARGS     64u
#define MAX_ENV      64u
#define MAX_VOLUMES  20u    /* D: to Z:, but for H: */
#define COMMAND_CAP  126u   /* what DOS takes on a command line */
#define POLL_NS      100000000L

static const hull_host_ops *host;

struct hull_machine {
	hull_core       *core;
	char             dir[4096];        /* the engine's own, under the runtime dir: H: */
	char            *root;             /* drive C: */
	char            *workdir;
	char            *argv[MAX_ARGS];
	char            *envp[MAX_ENV];
	char            *volume_host[MAX_VOLUMES];
	char             volume_drive[MAX_VOLUMES];
	unsigned         volume_count;
	char             detail[512];
	pid_t            pid;
	pthread_t        reader;
	int              reading;
	pthread_mutex_t  lock;
	pthread_cond_t   done_cond;
	int              done;             /* DOSBox has been reaped */
	int              paused;
};

static hull_status fail(hull_error *error, hull_status code, const char *detail)
{
	if (error != NULL) {
		error->code           = code;
		error->detail         = detail;
		error->runtime_detail = NULL;
	}
	return code;
}

/* `text` holds nothing DOS would read as syntax on a batch's line. */
static int plain(const char *text)
{
	return strpbrk(text, "<>|%\r\n\"") == NULL;
}

static void release(hull_machine *m)
{
	free(m->root);
	free(m->workdir);
	for (unsigned i = 0u; i < MAX_ARGS; i++)
		free(m->argv[i]);
	for (unsigned i = 0u; i < MAX_ENV; i++)
		free(m->envp[i]);
	for (unsigned i = 0u; i < MAX_VOLUMES; i++)
		free(m->volume_host[i]);
	pthread_mutex_destroy(&m->lock);
	pthread_cond_destroy(&m->done_cond);
	free(m);
}

/* What the plan asks, read into `m`: NULL, or why it cannot be run. */
static const char *read_plan(hull_machine *m, const hull_sexpr_doc *doc,
                             const hull_sexpr_form *machine)
{
	hull_sexpr_form entry;
	hull_sexpr_form item;
	char           *source = hull_access_value(doc, machine, "source");
	unsigned        n = 0u;
	size_t          line = 0u;

	if (source == NULL || strncmp(source, "dir:", 4u) != 0 || source[4] == '\0') {
		free(source);
		return "dosbox runs only a dir source, as drive C:";
	}
	m->root = hull_access_path(doc, source + 4);
	free(source);
	m->workdir = hull_access_value(doc, machine, "workdir");
	if (m->root == NULL || m->workdir == NULL)
		return "the plan is missing its source or workdir";
	if (!plain(m->root) || !plain(m->workdir))
		return "the directory or the workdir holds a character DOSBox cannot be given";
	if (!hull_access_find(doc, machine, "command", &entry))
		return "the machine names no command, so it needs (command ...)";
	hull_sexpr_child(doc, &entry, &item);
	while (hull_sexpr_next(doc, &item, &item)) {
		if (n == MAX_ARGS)
			return "the command has too many arguments";
		if ((m->argv[n] = hull_access_text(doc, &item)) == NULL)
			return "an argument holds a NUL byte";
		if (!plain(m->argv[n]))
			return "the command holds a character a DOS command line reads as syntax: < > | % or a quote";
		line += strlen(m->argv[n++]) + 1u;
	}
	if (n == 0u)
		return "the command is empty";
	if (line - 1u > COMMAND_CAP)
		return "the command is longer than the 126 characters a DOS command line holds";

	n = 0u;
	if (hull_access_find(doc, machine, "env", &entry)) {
		hull_sexpr_child(doc, &entry, &item);
		while (hull_sexpr_next(doc, &item, &item)) {
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
			if (key != NULL && text != NULL && plain(key) && plain(text) &&
			    strchr(key, '=') == NULL &&
			    (m->envp[n] = malloc(strlen(key) + strlen(text) + 2u)) != NULL)
				sprintf(m->envp[n], "%s=%s", key, text);
			free(key);
			free(text);
			if (m->envp[n++] == NULL)
				return "an env name or value holds a character DOS reads as syntax";
		}
	}

	if (hull_access_find(doc, machine, "volumes", &entry)) {
		hull_sexpr_child(doc, &entry, &item);
		while (hull_sexpr_next(doc, &item, &item)) {
			char    *guest = hull_access_value(doc, &item, "guest");
			char    *host_path = hull_access_value(doc, &item, "host");
			unsigned i = m->volume_count;
			char     drive = guest != NULL && guest[0] == '/' && guest[1] != '\0' &&
			                 guest[2] == '\0' ? (char)(guest[1] & ~0x20) : '\0';

			free(guest);
			if (m->volume_count == MAX_VOLUMES) {
				free(host_path);
				return "there are too many volumes";
			}
			m->volume_host[i] = host_path != NULL ? hull_access_path(doc, host_path) : NULL;
			free(host_path);
			m->volume_count++;
			if (drive < 'D' || drive > 'Z' || drive == 'H')
				return "a volume's guest path is its drive, /d to /z, but for /h, which is hull's";
			for (unsigned k = 0u; k < i; k++)
				if (m->volume_drive[k] == drive)
					return "two volumes name one drive";
			m->volume_drive[i] = drive;
			if (m->volume_host[i] == NULL || !plain(m->volume_host[i]))
				return "a volume's host path is missing, or holds a character DOSBox cannot be given";
		}
	}
	return NULL;
}

/* `text` written whole to `path`, made for the user alone. */
static int write_file(const char *path, const char *text)
{
	int    fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
	size_t length = strlen(text);
	int    ok = fd >= 0 && write(fd, text, length) == (ssize_t)length;

	if (fd >= 0 && close(fd) != 0)
		ok = 0;
	return ok ? 0 : -1;
}

/* HULL.BAT and dosbox.conf in the machine's directory: 0, or -1. */
static int write_batch(const hull_machine *m)
{
	char   path[4200];
	char  *text = malloc(64u * 1024u);
	size_t at = 0u;
	int    ok;

#define ADD(...) at += (size_t)snprintf(text + at, 64u * 1024u - at, __VA_ARGS__)
	if (text == NULL)
		return -1;
	ADD("@echo off\r\n");
	for (unsigned i = 0u; i < MAX_ENV && m->envp[i] != NULL; i++)
		ADD("set %s\r\n", m->envp[i]);
	ADD("C:\r\ncd \\");
	for (const char *c = m->workdir + (m->workdir[0] == '/'); *c != '\0'; c++)
		ADD("%c", *c == '/' ? '\\' : *c);
	ADD("\r\ncall");
	for (unsigned i = 0u; i < MAX_ARGS && m->argv[i] != NULL; i++)
		ADD(" %s", m->argv[i]);
	ADD(" > H:\\OUT.TXT\r\n");
	/* ERRORLEVEL n is true from n up, so asked from the top down. */
	for (unsigned n = 255u; n > 0u; n--)
		ADD("if errorlevel %u goto s%u\r\n", n, n);
	ADD("echo S0>H:\\STATUS.TXT\r\ngoto end\r\n");
	for (unsigned n = 255u; n > 0u; n--)
		ADD(":s%u\r\necho S%u>H:\\STATUS.TXT\r\ngoto end\r\n", n, n);
	ADD(":end\r\nexit\r\n");
	snprintf(path, sizeof path, "%s/HULL.BAT", m->dir);
	ok = write_file(path, text) == 0;

	at = 0u;
	ADD("[autoexec]\nmount c \"%s\"\nmount h \"%s\"\n", m->root, m->dir);
	for (unsigned i = 0u; i < m->volume_count; i++)
		ADD("mount %c \"%s\"\n", m->volume_drive[i], m->volume_host[i]);
	ADD("h:\\hull.bat\n");
	snprintf(path, sizeof path, "%s/dosbox.conf", m->dir);
	ok = ok && write_file(path, text) == 0;
#undef ADD
	free(text);
	return ok ? 0 : -1;
}

static void remove_dir(hull_machine *m)
{
	static const char *const files[] = { "HULL.BAT", "dosbox.conf", "OUT.TXT", "STATUS.TXT",
	                                     "dosbox.log" };
	char path[4200];

	if (m->dir[0] == '\0')
		return;
	for (unsigned i = 0u; i < sizeof files / sizeof files[0]; i++) {
		snprintf(path, sizeof path, "%s/%s", m->dir, files[i]);
		unlink(path);
	}
	rmdir(m->dir);
	m->dir[0] = '\0';
}

/* The machine's directory, under the user's runtime directory. */
static const char *make_dir(hull_machine *m)
{
	const char     *runtime = getenv("XDG_RUNTIME_DIR");
	static unsigned counter;
	char            parent[3900];

	if (runtime == NULL || runtime[0] != '/')
		return "XDG_RUNTIME_DIR is not set";
	snprintf(parent, sizeof parent, "%s/hull", runtime);
	mkdir(parent, 0700);
	snprintf(m->dir, sizeof m->dir, "%s/dosbox-%ld-%u", parent, (long)getpid(),
	         __atomic_add_fetch(&counter, 1u, __ATOMIC_RELAXED));
	if (mkdir(m->dir, 0700) != 0) {
		m->dir[0] = '\0';
		return "the machine's directory cannot be made";
	}
	return NULL;
}

static hull_status dosbox_create(hull_core *core, hull_buf plan, hull_machine **out,
                                 hull_error *error)
{
	hull_sexpr_doc  doc;
	hull_sexpr_form machine;
	hull_machine   *m;
	const char     *reason;

	if (!hull_access_machine(&doc, plan.data, plan.len, &machine))
		return fail(error, HULL_ERR_UNSUPPORTED, "the plan is not canonical");
	if ((m = calloc(1u, sizeof *m)) == NULL)
		return fail(error, HULL_ERR_INTERNAL, "out of memory");
	m->core = core;
	m->pid  = -1;
	pthread_mutex_init(&m->lock, NULL);
	pthread_cond_init(&m->done_cond, NULL);
	if ((reason = read_plan(m, &doc, &machine)) != NULL ||
	    (reason = make_dir(m)) != NULL ||
	    (write_batch(m) != 0 && (reason = "the machine's batch cannot be written") != NULL)) {
		snprintf(m->detail, sizeof m->detail, "%s", reason);
		remove_dir(m);
		*out = NULL;
		/* The detail outlives the machine: the core reads it after. */
		{
			static char said[512];

			snprintf(said, sizeof said, "%s", m->detail);
			release(m);
			return fail(error, HULL_ERR_RUNTIME, said);
		}
	}
	*out = m;
	return HULL_OK;
}

/* What OUT.TXT holds past `*sent`, handed on as the machine's output. */
static void hand_on(hull_machine *m, off_t *sent)
{
	char    path[4200];
	uint8_t buffer[65536];
	int     fd;
	ssize_t n;

	snprintf(path, sizeof path, "%s/OUT.TXT", m->dir);
	if ((fd = open(path, O_RDONLY | O_CLOEXEC)) < 0)
		return;
	while ((n = pread(fd, buffer, sizeof buffer, *sent)) > 0) {
		host->on_data(m->core, NULL, 1, buffer, (size_t)n);
		*sent += n;
	}
	close(fd);
}

/* The last line of dosbox.log, for a DOSBox that ended without a word. */
static void last_line(const hull_machine *m, char *out, size_t cap)
{
	char  path[4200];
	char  line[512];
	FILE *log;

	out[0] = '\0';
	snprintf(path, sizeof path, "%s/dosbox.log", m->dir);
	if ((log = fopen(path, "re")) == NULL)
		return;
	while (fgets(line, sizeof line, log) != NULL)
		if (line[0] != '\n')
			snprintf(out, cap, "%.*s", (int)strcspn(line, "\n"), line);
	fclose(log);
}

/* Whether the output opens with DOSBox's word for a command not there. */
static int not_found(const hull_machine *m)
{
	static const char said[] = "Illegal command: ";
	char              path[4200];
	char              head[sizeof said];
	int               fd;
	ssize_t           n = 0;

	snprintf(path, sizeof path, "%s/OUT.TXT", m->dir);
	if ((fd = open(path, O_RDONLY | O_CLOEXEC)) >= 0) {
		n = read(fd, head, sizeof said - 1u);
		close(fd);
	}
	return n == (ssize_t)(sizeof said - 1u) && memcmp(head, said, sizeof said - 1u) == 0;
}

/* DOSBox's output while it runs, and its end once it has gone. */
static void *read_workload(void *arg)
{
	hull_machine   *m = arg;
	struct timespec pause = { 0, POLL_NS };
	off_t           sent = 0;
	int             status = 0;
	char            path[4200];
	char            said[16];
	FILE           *file;
	int             code = -1;

	for (;;) {
		pid_t got = waitpid(m->pid, &status, WNOHANG);

		hand_on(m, &sent);
		if (got == m->pid || (got < 0 && errno != EINTR))
			break;
		nanosleep(&pause, NULL);
	}
	hand_on(m, &sent);
	pthread_mutex_lock(&m->lock);
	m->done = 1;
	pthread_cond_broadcast(&m->done_cond);
	pthread_mutex_unlock(&m->lock);

	snprintf(path, sizeof path, "%s/STATUS.TXT", m->dir);
	if ((file = fopen(path, "re")) != NULL) {
		if (fgets(said, sizeof said, file) != NULL && said[0] == 'S')
			code = atoi(said + 1);
		fclose(file);
	}
	/* DOS sets no ERRORLEVEL for a command it cannot find, only says so
	 * where the output goes: 127, as a shell gives, not 0. */
	if (code == 0 && not_found(m))
		code = 127;
	if (code >= 0) {
		host->on_exit(m->core, m, NULL, code, 0);
	} else if (WIFSIGNALED(status)) {
		host->on_exit(m->core, m, NULL, -1, WTERMSIG(status));
	} else {
		char line[400];
		char message[512];

		last_line(m, line, sizeof line);
		snprintf(message, sizeof message, "DOSBox ended without the batch's word: %s",
		         line[0] != '\0' ? line : "its log is empty");
		host->log(m->core, 3, message);
		host->on_exit(m->core, m, NULL, -1, SIGKILL);
	}
	host->on_state(m->core, m, "stopped");
	return NULL;
}

static hull_status dosbox_start(hull_core *core, hull_machine *m, hull_error *error)
{
	char  conf[4200];
	char  log[4200];
	char  home[4200 + 8];
	pid_t parent = getpid();

	(void)core;
	snprintf(conf, sizeof conf, "%s/dosbox.conf", m->dir);
	snprintf(log, sizeof log, "%s/dosbox.log", m->dir);
	snprintf(home, sizeof home, "HOME=%s", m->dir);
	m->pid = fork();
	if (m->pid < 0)
		return fail(error, HULL_ERR_RUNTIME, "DOSBox could not be started");
	if (m->pid == 0) {
		char *const argv[] = { "dosbox", "-conf", conf, NULL };
		char *const envp[] = { home, "SDL_VIDEODRIVER=dummy", "SDL_AUDIODRIVER=dummy",
		                       "PATH=/usr/local/bin:/usr/bin:/bin", NULL };
		int         null = open("/dev/null", O_RDONLY);
		int         out = open(log, O_WRONLY | O_CREAT | O_TRUNC, 0600);

		if (null < 0 || out < 0 || dup2(null, 0) < 0 || dup2(out, 1) < 0 || dup2(out, 2) < 0 ||
		    setsid() < 0 || prctl(PR_SET_PDEATHSIG, SIGKILL, 0L, 0L, 0L) != 0 ||
		    getppid() != parent || prctl(PR_SET_NO_NEW_PRIVS, 1L, 0L, 0L, 0L) != 0)
			_exit(126);
		close_range(3u, ~0u, 0);
		hull_access_execute(argv, envp);
		dprintf(2, "dosbox, which a dos machine needs: %s\n", strerror(errno));
		_exit(127);
	}
	if (pthread_create(&m->reader, NULL, read_workload, m) != 0) {
		kill(m->pid, SIGKILL);
		waitpid(m->pid, NULL, 0);
		return fail(error, HULL_ERR_INTERNAL, "the reader thread could not start");
	}
	m->reading = 1;
	host->on_state(m->core, m, "running");
	return HULL_OK;
}

/* Until DOSBox is reaped or `ms` pass: 1 if it was. */
static int wait_done(hull_machine *m, int ms)
{
	struct timespec until;
	int             done;

	clock_gettime(CLOCK_REALTIME, &until);
	until.tv_sec  += ms / 1000;
	until.tv_nsec += (long)(ms % 1000) * 1000000L;
	if (until.tv_nsec >= 1000000000L) {
		until.tv_sec++;
		until.tv_nsec -= 1000000000L;
	}
	pthread_mutex_lock(&m->lock);
	while (!m->done && pthread_cond_timedwait(&m->done_cond, &m->lock, &until) == 0)
		;
	done = m->done;
	pthread_mutex_unlock(&m->lock);
	return done;
}

/* TERM, then KILL after `timeout_ms`. The reader reaps it, so the pid is
 * signalled only while it is not done, under the lock. */
static hull_status dosbox_stop(hull_core *core, hull_machine *m, int timeout_ms, hull_error *error)
{
	(void)core;
	(void)error;
	pthread_mutex_lock(&m->lock);
	if (!m->done && m->pid > 0) {
		if (m->paused)
			kill(m->pid, SIGCONT);
		kill(m->pid, SIGTERM);
	}
	pthread_mutex_unlock(&m->lock);
	if (!wait_done(m, timeout_ms > 0 ? timeout_ms : 1)) {
		pthread_mutex_lock(&m->lock);
		if (!m->done && m->pid > 0)
			kill(m->pid, SIGKILL);
		pthread_mutex_unlock(&m->lock);
		wait_done(m, 5000);
	}
	return HULL_OK;
}

static hull_status dosbox_destroy(hull_core *core, hull_machine *m, hull_error *error)
{
	if (m == NULL)
		return HULL_OK;
	if (m->reading) {
		dosbox_stop(core, m, 0, error);
		pthread_join(m->reader, NULL);
	} else if (m->pid > 0) {
		kill(m->pid, SIGKILL);
		waitpid(m->pid, NULL, 0);
	}
	remove_dir(m);
	release(m);
	return HULL_OK;
}

static hull_status dosbox_signal(hull_core *core, hull_machine *m, hull_stream *stream,
                                 int signum, hull_error *error)
{
	(void)core;
	if (stream != NULL)
		return fail(error, HULL_ERR_UNSUPPORTED, "a dos machine has no commands but its own");
	pthread_mutex_lock(&m->lock);
	if (!m->done && m->pid > 0)
		kill(m->pid, signum);
	pthread_mutex_unlock(&m->lock);
	return HULL_OK;
}

/* DOSBox stopped where it is, emulated CPU and all, and let go on. */
static hull_status dosbox_pause(hull_core *core, hull_machine *m, int paused, hull_error *error)
{
	int gone;

	(void)core;
	pthread_mutex_lock(&m->lock);
	gone = m->done || m->pid <= 0 || kill(m->pid, paused ? SIGSTOP : SIGCONT) != 0;
	if (!gone)
		m->paused = paused;
	pthread_mutex_unlock(&m->lock);
	return gone ? fail(error, HULL_ERR_GONE, "the machine is not running") : HULL_OK;
}

static hull_status dosbox_stats(hull_core *core, hull_machine *m, hull_buf *out, hull_error *error)
{
	static unsigned char encoded[128];
	hull_usage           usage;
	int                  ok;

	(void)core;
	memset(&usage, 0, sizeof usage);
	pthread_mutex_lock(&m->lock);
	ok = !m->done && m->pid > 0 && hull_usage_add(m->pid, &usage) == 0;
	pthread_mutex_unlock(&m->lock);
	if (!ok)
		return fail(error, HULL_ERR_GONE, "the machine is not running");
	out->data = encoded;
	out->len  = hull_usage_encode(&usage, encoded, sizeof encoded);
	return out->len == 0u ? fail(error, HULL_ERR_INTERNAL, "stats do not fit") : HULL_OK;
}

static const hull_cap_entry entries[] = {
	/* The directory is drive C:, written in place, as a DOS PC's disk is. */
	{ "source.dir.direct",  HULL_SUP_NATIVE,   "the directory is drive C:", NULL },
	{ "source.dir.overlay", HULL_SUP_REJECTED,
	  "not yet: DOSBox writes C: in place, and an overlay would be a copy of the tree", NULL },
	{ "source.ref.overlay", HULL_SUP_REJECTED,
	  "not yet: a stored tree would be checked out to a directory first", NULL },
	{ "projection.bind",    HULL_SUP_NATIVE,   "the directory mounted as a drive", NULL },
	{ "writeback.none",     HULL_SUP_NATIVE,   NULL, NULL },

	{ "command",            HULL_SUP_EMULATED,
	  "a DOS command line, run by CALL from a batch of hull's: what a program writes through "
	  "DOS is its output, redirected, not what it draws on the screen, and a batch's own lines "
	  "are not, DOS redirecting none; a command not found ends 127", NULL },
	{ "workdir",            HULL_SUP_NATIVE,   "a directory of C:", NULL },
	{ "env",                HULL_SUP_NATIVE,   "SET in the batch; DOS makes the names upper case",
	  NULL },
	{ "volumes",            HULL_SUP_NATIVE,
	  "a drive each, its guest path the letter: /d is D:, and /h is hull's", NULL },
	{ "network.none",       HULL_SUP_NATIVE,   "a DOS machine here has no network", NULL },
	{ "security.no_new_privs", HULL_SUP_NATIVE, "set on the DOSBox process", NULL },
	{ "on_disconnect.stop", HULL_SUP_NATIVE,   NULL, NULL },
	{ "on_disconnect.keep", HULL_SUP_REJECTED,
	  "not yet: DOSBox is a child of the core, and dies with it", NULL },
	{ "hostname",           HULL_SUP_REJECTED, "DOS has no hostname", NULL },

	{ NULL,                 HULL_SUP_NATIVE,   NULL, NULL },
};

static const char *const classes[] = { "dos", NULL };

static const hull_capabilities caps = {
	.struct_size    = sizeof (hull_capabilities),
	.engine_name    = "dosbox",
	.engine_version = "0.1",
	.classes        = classes,
	.entries        = entries,
	.migration      = HULL_MIG_PINNED,
	.arches         = NULL,
	.requires_root  = 0,
};

static const hull_engine_ops ops = {
	.struct_size = sizeof (hull_engine_ops),
	.create      = dosbox_create,
	.start       = dosbox_start,
	.stop        = dosbox_stop,
	.destroy     = dosbox_destroy,
	.signal      = dosbox_signal,
	.stats       = dosbox_stats,
	.pause       = dosbox_pause,
};

static const hull_engine engine = {
	.struct_size = sizeof (hull_engine),
	.abi_major   = HULL_ABI_MAJOR,
	.abi_minor   = HULL_ABI_MINOR,
	.caps        = &caps,
	.ops         = &ops,
};

const hull_engine *hull_engine_dosbox(const hull_host_ops *host_ops)
{
	host = host_ops;
	return &engine;
}
