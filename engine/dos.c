/*
 * dos.c -- what the dos class's engines share. See dos.h.
 *
 * The machine's directory is drive C:, in place, or under (mutable
 * overlay) a copy of it in the engine's own directory, its writes thrown
 * away with the machine. Each engine writes a
 * batch of its own, HULL.BAT, into a directory of its own under the
 * user's runtime directory, which it gives the emulator as drive H:; the
 * batch runs the command by CALL, its DOS output redirected to
 * H:\OUT.TXT. A reader thread hands OUT.TXT on as it grows and once more
 * when the emulator has gone, then says the workload's end as the engine
 * reads it. A command the shell cannot find sets no ERRORLEVEL, so the
 * end is 127, as a shell's, where the output opens with the engine's
 * shell's word for one.
 *
 * The emulator runs as the user, its HOME the machine's directory so
 * that it writes no configuration into the user's, no_new_privs set, in
 * a session of its own, and killed with the core that started it.
 */

#define _GNU_SOURCE

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <ftw.h>
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
#include "dos.h"
#include "usage.h"

#define POLL_NS 100000000L

const hull_host_ops *hull_dos_host;

static hull_status fail(hull_error *error, hull_status code, const char *detail)
{
	if (error != NULL) {
		error->code           = code;
		error->detail         = detail;
		error->runtime_detail = NULL;
	}
	return code;
}

int hull_dos_plain(const char *text)
{
	return strpbrk(text, "<>|%\r\n\"") == NULL;
}

/* A path the engine may hand its emulator. */
static int usable(const hull_machine *m, const char *path)
{
	return hull_dos_plain(path) && (!m->engine->space_free || strchr(path, ' ') == NULL);
}

static void release(hull_machine *m)
{
	free(m->root);
	free(m->workdir);
	for (unsigned i = 0u; i < HULL_DOS_MAX_ARGS; i++)
		free(m->argv[i]);
	for (unsigned i = 0u; i < HULL_DOS_MAX_ENV; i++)
		free(m->envp[i]);
	for (unsigned i = 0u; i < HULL_DOS_MAX_VOLUMES; i++)
		free(m->volume_host[i]);
	for (unsigned i = 0u; i < HULL_DOS_MAX_MEDIA; i++)
		free(m->media_host[i]);
	pthread_mutex_destroy(&m->lock);
	pthread_cond_destroy(&m->done_cond);
	free(m);
}

/* `path`, freed, as the host names it with nothing between: an emulator
 * that checks a path against a list, as dosemu2 does, compares it whole,
 * so `/a/./b` would not be `/a/b`. NULL where it is not there. */
static char *real(char *path)
{
	char *whole = path != NULL ? realpath(path, NULL) : NULL;

	free(path);
	return whole;
}

/* Whether `drive` is free for a medium: not hull's, not the emulator's,
 * and not a volume's or an earlier medium's. */
static int drive_free(const hull_machine *m, char drive)
{
	if (drive == 'H' || strchr(m->engine->reserved, drive) != NULL)
		return 0;
	for (unsigned k = 0u; k < m->volume_count; k++)
		if (m->volume_drive[k] == drive)
			return 0;
	for (unsigned k = 0u; k < m->media_count; k++)
		if (m->media_drive[k] == drive)
			return 0;
	return 1;
}

/*
 * `(media (medium (type cdrom) (source ...)))`, after the volumes: each
 * disc the first drive from D: nothing else holds, in order, as a DOS
 * PC's CD-ROM follows its disks. A `dir:` collection's `files/` is the
 * disc's tree (section 3.10); an `image:` an ISO the emulator mounts.
 */
static const char *read_media(hull_machine *m, const hull_sexpr_doc *doc,
                              const hull_sexpr_form *machine)
{
	static char     why[200];
	hull_sexpr_form entry;
	hull_sexpr_form item;

	if (!hull_access_find(doc, machine, "media", &entry))
		return NULL;
	hull_sexpr_child(doc, &entry, &item);
	while (hull_sexpr_next(doc, &item, &item)) {
		char    *type = hull_access_value(doc, &item, "type");
		char    *source = hull_access_value(doc, &item, "source");
		unsigned i = m->media_count;
		int      cdrom = type != NULL && strcmp(type, "cdrom") == 0;
		int      image = source != NULL && strncmp(source, "image:", 6u) == 0;
		int      dir = source != NULL && strncmp(source, "dir:", 4u) == 0;
		char     drive = 'D';
		struct stat st;

		free(type);
		if (i == HULL_DOS_MAX_MEDIA || !cdrom || !(dir || (image && m->engine->images))) {
			free(source);
			return i == HULL_DOS_MAX_MEDIA ? "there are too many media" :
			       "a dos machine's medium is a cdrom, from a dir: collection or, where the "
			       "engine mounts one, an image:";
		}
		{
			char  *path = hull_access_path(doc, source + (image ? 6 : 4));
			char  *files = NULL;

			if (path != NULL && dir && (files = malloc(strlen(path) + 7u)) != NULL)
				sprintf(files, "%s/files", path);
			m->media_host[i] = real(dir ? files : path);
			if (dir)
				free(path);
		}
		free(source);
		m->media_image[i] = image;
		m->media_count++;
		while (drive <= 'Z' && !drive_free(m, drive))
			drive++;
		if (drive > 'Z')
			return "no drive is left for a medium";
		m->media_drive[i] = drive;
		if (m->media_host[i] == NULL || !usable(m, m->media_host[i]) ||
		    stat(m->media_host[i], &st) != 0 ||
		    !(image ? S_ISREG(st.st_mode) : S_ISDIR(st.st_mode))) {
			snprintf(why, sizeof why, "a medium is not there -- a dir: collection holds its "
			         "disc's tree in files/ -- or its path holds a character %s cannot be "
			         "given%s", m->engine->name,
			         m->engine->space_free ? ", a space among them" : "");
			return why;
		}
	}
	return NULL;
}

/* What the plan asks, read into `m`: NULL, or why it cannot be run. */
static const char *read_plan(hull_machine *m, const hull_sexpr_doc *doc,
                             const hull_sexpr_form *machine)
{
	static char     why[200];
	hull_sexpr_form entry;
	hull_sexpr_form item;
	char           *source = hull_access_value(doc, machine, "source");
	unsigned        n = 0u;
	size_t          line = 0u;

	if (source == NULL || strncmp(source, "dir:", 4u) != 0 || source[4] == '\0') {
		free(source);
		return "a dos machine runs only a dir source, as drive C:";
	}
	m->root = real(hull_access_path(doc, source + 4));
	free(source);
	m->workdir = hull_access_value(doc, machine, "workdir");
	{
		char *mutability = hull_access_value(doc, machine, "mutable");

		m->overlay = mutability != NULL && strcmp(mutability, "overlay") == 0;
		free(mutability);
	}
	if (m->root == NULL || m->workdir == NULL)
		return "the plan is missing its source or workdir, or the source is not there";
	if (!usable(m, m->root) || !hull_dos_plain(m->workdir)) {
		snprintf(why, sizeof why, "the directory or the workdir holds a character %s cannot "
		         "be given%s", m->engine->name, m->engine->space_free ? ", a space among them" : "");
		return why;
	}
	if (!hull_access_find(doc, machine, "command", &entry))
		return "the machine names no command, so it needs (command ...)";
	hull_sexpr_child(doc, &entry, &item);
	while (hull_sexpr_next(doc, &item, &item)) {
		if (n == HULL_DOS_MAX_ARGS)
			return "the command has too many arguments";
		if ((m->argv[n] = hull_access_text(doc, &item)) == NULL)
			return "an argument holds a NUL byte";
		if (!hull_dos_plain(m->argv[n]))
			return "the command holds a character a DOS command line reads as syntax: < > | % or a quote";
		line += strlen(m->argv[n++]) + 1u;
	}
	if (n == 0u)
		return "the command is empty";
	if (line - 1u > HULL_DOS_COMMAND_CAP)
		return "the command is longer than the 126 characters a DOS command line holds";

	n = 0u;
	if (hull_access_find(doc, machine, "env", &entry)) {
		hull_sexpr_child(doc, &entry, &item);
		while (hull_sexpr_next(doc, &item, &item)) {
			hull_sexpr_form name;
			hull_sexpr_form value;
			char           *key;
			char           *text;

			if (n == HULL_DOS_MAX_ENV)
				return "env has too many names";
			hull_sexpr_child(doc, &item, &name);
			hull_sexpr_next(doc, &name, &value);
			key  = hull_access_text(doc, &name);
			text = hull_access_text(doc, &value);
			if (key != NULL && text != NULL && hull_dos_plain(key) && hull_dos_plain(text) &&
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
			char    *mode = hull_access_value(doc, &item, "mode");
			unsigned i = m->volume_count;
			char     drive = guest != NULL && guest[0] == '/' && guest[1] != '\0' &&
			                 guest[2] == '\0' ? (char)(guest[1] & ~0x20) : '\0';

			free(guest);
			if (m->volume_count == HULL_DOS_MAX_VOLUMES) {
				free(host_path);
				free(mode);
				return "there are too many volumes";
			}
			m->volume_host[i] = host_path != NULL ? real(hull_access_path(doc, host_path)) : NULL;
			m->volume_readonly[i] = mode != NULL && strcmp(mode, "ro") == 0;
			free(host_path);
			free(mode);
			m->volume_count++;
			if (drive < 'D' || drive > 'Z' || drive == 'H')
				return "a volume's guest path is its drive, /d to /z, but for /h, which is hull's";
			if (strchr(m->engine->reserved, drive) != NULL)
				return m->engine->reserved_why;
			for (unsigned k = 0u; k < i; k++)
				if (m->volume_drive[k] == drive)
					return "two volumes name one drive";
			m->volume_drive[i] = drive;
			if (m->volume_host[i] == NULL || !usable(m, m->volume_host[i])) {
				snprintf(why, sizeof why, "a volume's host path is not there, or holds a "
				         "character %s cannot be given%s", m->engine->name,
				         m->engine->space_free ? ", a space among them" : "");
				return why;
			}
		}
	}
	return read_media(m, doc, machine);
}

int hull_dos_said(const hull_machine *m)
{
	char  said[16];
	FILE *file = NULL;
	int   fd = hull_dos_open(m, "STATUS.TXT");
	int   code = -1;

	if (fd >= 0 && (file = fdopen(fd, "r")) == NULL)
		close(fd);
	if (file == NULL)
		return -1;
	if (fgets(said, sizeof said, file) != NULL) {
		if (said[0] == 'S' && said[1] >= '0' && said[1] <= '9') {
			code = atoi(said + 1);
		} else if (said[0] == 'M' && said[1] >= 'A' && said[1] <= 'Z') {
			char message[160];
			int  n = snprintf(message, sizeof message, "%s did not mount drive %c:, so the "
			                  "command was not run\n", m->engine->name, said[1]);

			/* On the machine's stderr, where the run sees it, and in the log. */
			hull_dos_host->on_data(m->core, NULL, 2, (const uint8_t *)message, (size_t)n);
			message[n - 1] = '\0';
			hull_dos_host->log(m->core, 3, message);
			code = 125;
		}
	}
	fclose(file);
	return code;
}

int hull_dos_open(const hull_machine *m, const char *name)
{
	char path[4200];
	char lower[64];
	int  fd;
	size_t i;

	snprintf(path, sizeof path, "%s/%s", m->dir, name);
	if ((fd = open(path, O_RDONLY | O_CLOEXEC)) >= 0)
		return fd;
	for (i = 0u; name[i] != '\0' && i + 1u < sizeof lower; i++)
		lower[i] = (char)tolower((unsigned char)name[i]);
	lower[i] = '\0';
	snprintf(path, sizeof path, "%s/%s", m->dir, lower);
	return open(path, O_RDONLY | O_CLOEXEC);
}

/* An overlay's copy, `dir`/C, removed whole: a tree this engine made by
 * construction, so it may be cleared without naming its files, under
 * the machine's own directory and never anything else. */
static int remove_one(const char *path, const struct stat *st, int kind, struct FTW *at)
{
	(void)st;
	(void)at;
	return kind == FTW_DP ? rmdir(path) : unlink(path);
}

static void remove_copy(const hull_machine *m)
{
	char copy[4200];

	if (!m->overlay || m->dir[0] != '/')
		return;
	snprintf(copy, sizeof copy, "%s/C", m->dir);
	nftw(copy, remove_one, 16, FTW_DEPTH | FTW_PHYS | FTW_MOUNT);
}

/* The directory copied to `dir`/C for an overlay, owners and times kept,
 * and C: pointed at the copy: 0, or -1. */
static int make_copy(hull_machine *m)
{
	char  copy[4200];
	char *source = m->root;
	pid_t child;
	int   status = 0;

	snprintf(copy, sizeof copy, "%s/C", m->dir);
	child = fork();
	if (child == 0) {
		char *const argv[] = { "cp", "-a", "--", source, copy, NULL };
		char *const envp[] = { "PATH=/usr/local/bin:/usr/bin:/bin", NULL };
		int         null = open("/dev/null", O_RDWR);

		if (null >= 0) {
			dup2(null, 0);
			dup2(null, 1);
			dup2(null, 2);
		}
		hull_access_execute(argv, envp);
		_exit(127);
	}
	if (child < 0 || waitpid(child, &status, 0) != child || !WIFEXITED(status) ||
	    WEXITSTATUS(status) != 0)
		return -1;
	if ((m->root = strdup(copy)) == NULL) {
		m->root = source;
		return -1;
	}
	free(source);
	return 0;
}

static void remove_dir(hull_machine *m)
{
	/* The batch's own in either case, as an emulator's redirector may
	 * make a file DOS names upper case lower case on the host, and what
	 * dosemu2 keeps under its HOME. */
	static const char *const files[] = { "HULL.BAT", "dosbox.conf", "OUT.TXT", "out.txt",
	                                     "STATUS.TXT", "status.txt", "emulator.log" };
	char path[4200];

	if (m->dir[0] == '\0')
		return;
	remove_copy(m);
	for (unsigned i = 0u; i < sizeof files / sizeof files[0]; i++) {
		snprintf(path, sizeof path, "%s/%s", m->dir, files[i]);
		unlink(path);
	}
	/* What dosemu2 keeps under the HOME it is given, the machine's own
	 * directory: boot.log, and stderr.log.PID where it was killed, a name
	 * this engine cannot know, so the regular files of that one directory,
	 * which no one but the emulator this machine ran writes. */
	snprintf(path, sizeof path, "%s/.dosemu", m->dir);
	{
		int  fd = open(path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
		DIR *dir = fd >= 0 ? fdopendir(fd) : NULL;

		for (struct dirent *e; dir != NULL && (e = readdir(dir)) != NULL;)
			if (e->d_type == DT_REG)
				unlinkat(dirfd(dir), e->d_name, 0);
		if (dir != NULL)
			closedir(dir);
		else if (fd >= 0)
			close(fd);
	}
	rmdir(path);
	snprintf(path, sizeof path, "%s/local", m->dir);
	rmdir(path);
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
	snprintf(m->dir, sizeof m->dir, "%s/%s-%ld-%u", parent, m->engine->name, (long)getpid(),
	         __atomic_add_fetch(&counter, 1u, __ATOMIC_RELAXED));
	if (mkdir(m->dir, 0700) != 0) {
		m->dir[0] = '\0';
		return "the machine's directory cannot be made";
	}
	return NULL;
}

hull_status hull_dos_create(const hull_dos_engine *engine, hull_core *core, hull_buf plan,
                            hull_machine **out, hull_error *error)
{
	static char     said[512];
	hull_sexpr_doc  doc;
	hull_sexpr_form machine;
	hull_machine   *m;
	const char     *reason;

	*out = NULL;
	if (!hull_access_machine(&doc, plan.data, plan.len, &machine))
		return fail(error, HULL_ERR_UNSUPPORTED, "the plan is not canonical");
	if ((m = calloc(1u, sizeof *m)) == NULL)
		return fail(error, HULL_ERR_INTERNAL, "out of memory");
	m->engine = engine;
	m->core   = core;
	m->pid    = -1;
	pthread_mutex_init(&m->lock, NULL);
	pthread_cond_init(&m->done_cond, NULL);
	if ((reason = read_plan(m, &doc, &machine)) == NULL &&
	    (reason = make_dir(m)) == NULL) {
		if (m->overlay && make_copy(m) != 0)
			reason = "the directory cannot be copied for the overlay";
		else if (engine->write_files(m) != 0)
			reason = "the machine's batch cannot be written";
	}
	if (reason != NULL) {
		/* The detail outlives the machine: the core reads it after. */
		snprintf(said, sizeof said, "%s", reason);
		remove_dir(m);
		release(m);
		return fail(error, HULL_ERR_RUNTIME, said);
	}
	*out = m;
	return HULL_OK;
}

/* What OUT.TXT holds past `*sent`, handed on as the machine's output. */
static void hand_on(hull_machine *m, off_t *sent)
{
	uint8_t buffer[65536];
	int     fd;
	ssize_t n;

	if ((fd = hull_dos_open(m, "OUT.TXT")) < 0)
		return;
	while ((n = pread(fd, buffer, sizeof buffer, *sent)) > 0) {
		hull_dos_host->on_data(m->core, NULL, 1, buffer, (size_t)n);
		*sent += n;
	}
	close(fd);
}

/* Whether the output opens with the shell's word for a command not there. */
static int not_found(const hull_machine *m)
{
	const char *said = m->engine->not_found;
	size_t      length = strlen(said);
	char        head[64];
	int         fd;
	ssize_t     n = 0;

	if (length > sizeof head)
		return 0;
	if ((fd = hull_dos_open(m, "OUT.TXT")) >= 0) {
		n = read(fd, head, length);
		close(fd);
	}
	return n == (ssize_t)length && memcmp(head, said, length) == 0;
}

/* The last line of the emulator's log, for one that ended without a word. */
static void last_line(const hull_machine *m, char *out, size_t cap)
{
	char  path[4200];
	char  line[512];
	FILE *log;

	out[0] = '\0';
	snprintf(path, sizeof path, "%s/emulator.log", m->dir);
	if ((log = fopen(path, "re")) == NULL)
		return;
	while (fgets(line, sizeof line, log) != NULL)
		if (line[0] != '\n' && line[0] != '\r')
			snprintf(out, cap, "%.*s", (int)strcspn(line, "\r\n"), line);
	fclose(log);
}

/* Whether C: holds `name`, in either case, as DOS would find it. */
static int holds(const hull_machine *m, const char *name)
{
	char        path[4200];
	struct stat st;
	size_t      at = (size_t)snprintf(path, sizeof path, "%s/", m->root);

	for (const char *c = name; *c != '\0' && at + 1u < sizeof path; c++)
		path[at++] = (char)toupper((unsigned char)*c);
	path[at] = '\0';
	if (lstat(path, &st) == 0)
		return 1;
	snprintf(path, sizeof path, "%s/%s", m->root, name);
	return lstat(path, &st) == 0;
}

/*
 * The directory the emulator makes in C: for itself -- dosemu2's FreeDOS
 * autoexec makes C:\TMP for its TEMP -- removed once it has gone, where C:
 * had none before and it is still empty: the user's directory is left as
 * the run found it but for what the command wrote. A command that makes
 * that directory itself and leaves it empty loses it the same way.
 */
static void unmake(const hull_machine *m)
{
	char path[4200];

	if (!m->unmade)
		return;
	snprintf(path, sizeof path, "%s/%s", m->root, m->engine->makes);
	rmdir(path);
}

/* The emulator's output while it runs, and its end once it has gone. */
static void *read_workload(void *arg)
{
	hull_machine   *m = arg;
	struct timespec pause = { 0, POLL_NS };
	off_t           sent = 0;
	int             status = 0;
	int             code;

	for (;;) {
		pid_t got = waitpid(m->pid, &status, WNOHANG);

		hand_on(m, &sent);
		if (got == m->pid || (got < 0 && errno != EINTR))
			break;
		nanosleep(&pause, NULL);
	}
	hand_on(m, &sent);
	unmake(m);
	pthread_mutex_lock(&m->lock);
	m->done = 1;
	pthread_cond_broadcast(&m->done_cond);
	pthread_mutex_unlock(&m->lock);

	code = m->engine->end_code(m, status);
	if (code == 0 && not_found(m))
		code = 127;
	if (code >= 0) {
		hull_dos_host->on_exit(m->core, m, NULL, code, 0);
	} else if (WIFSIGNALED(status)) {
		hull_dos_host->on_exit(m->core, m, NULL, -1, WTERMSIG(status));
	} else {
		char line[400];
		char message[512];

		last_line(m, line, sizeof line);
		snprintf(message, sizeof message, "%s ended without the batch's word: %s",
		         m->engine->name, line[0] != '\0' ? line : "its log is empty");
		hull_dos_host->log(m->core, 3, message);
		hull_dos_host->on_exit(m->core, m, NULL, -1, SIGKILL);
	}
	hull_dos_host->on_state(m->core, m, "stopped");
	return NULL;
}

hull_status hull_dos_start(hull_core *core, hull_machine *m, hull_error *error)
{
	char  log[4200];
	pid_t parent = getpid();

	(void)core;
	snprintf(log, sizeof log, "%s/emulator.log", m->dir);
	m->unmade = m->engine->makes != NULL && !holds(m, m->engine->makes);
	m->pid = fork();
	if (m->pid < 0)
		return fail(error, HULL_ERR_RUNTIME, "the emulator could not be started");
	if (m->pid == 0) {
		int null = open("/dev/null", O_RDONLY);
		int out = open(log, O_WRONLY | O_CREAT | O_TRUNC, 0600);

		if (null < 0 || out < 0 || dup2(null, 0) < 0 || dup2(out, 1) < 0 || dup2(out, 2) < 0 ||
		    setsid() < 0 || prctl(PR_SET_PDEATHSIG, SIGKILL, 0L, 0L, 0L) != 0 ||
		    getppid() != parent || prctl(PR_SET_NO_NEW_PRIVS, 1L, 0L, 0L, 0L) != 0)
			_exit(126);
		close_range(3u, ~0u, 0);
		m->engine->exec_child(m);
		dprintf(2, "%s, which a dos machine of that engine needs: %s\n", m->engine->name,
		        strerror(errno));
		_exit(127);
	}
	if (pthread_create(&m->reader, NULL, read_workload, m) != 0) {
		kill(m->pid, SIGKILL);
		waitpid(m->pid, NULL, 0);
		return fail(error, HULL_ERR_INTERNAL, "the reader thread could not start");
	}
	m->reading = 1;
	hull_dos_host->on_state(m->core, m, "running");
	return HULL_OK;
}

/* Until the emulator is reaped or `ms` pass: 1 if it was. */
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
hull_status hull_dos_stop(hull_core *core, hull_machine *m, int timeout_ms, hull_error *error)
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

hull_status hull_dos_destroy(hull_core *core, hull_machine *m, hull_error *error)
{
	if (m == NULL)
		return HULL_OK;
	if (m->reading) {
		hull_dos_stop(core, m, 0, error);
		pthread_join(m->reader, NULL);
	} else if (m->pid > 0) {
		kill(m->pid, SIGKILL);
		waitpid(m->pid, NULL, 0);
	}
	remove_dir(m);
	release(m);
	return HULL_OK;
}

hull_status hull_dos_signal(hull_core *core, hull_machine *m, hull_stream *stream, int signum,
                            hull_error *error)
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

/* The emulator stopped where it is, emulated CPU and all, and let go on. */
hull_status hull_dos_pause(hull_core *core, hull_machine *m, int paused, hull_error *error)
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

hull_status hull_dos_stats(hull_core *core, hull_machine *m, hull_buf *out, hull_error *error)
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
