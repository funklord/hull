/*
 * hw.c -- the hw class: a machine that is a board (project.md section
 * 4.12), any computer hull reaches by ssh, whose power, console and health
 * are commands the pool's member names. Nothing here knows what a board is
 * made of, so a PDU, a relay and a hull vm standing in for one are the same
 * to it: `(board "a" (address ...) (power "CMD") (console "CMD") (health
 * "CMD"))` in pools.conf, power's given `on`, `off` or `cycle`.
 *
 * A run takes a free board of the machine's pool: power on, wait for its
 * sshd, its health, the source deployed by tar over ssh under the board's
 * `root`, the command run there in that tree, its output and status the
 * machine's; then the tree removed and the power off. No agent runs on the
 * board: its sshd and the user's keys do the authenticating, as section
 * 4.10 defers a protocol of hull's own, and the board's own system is what
 * enters the tree, by `unshare --root`, -r where the login is not root.
 *
 * Recovery is section 4.12's: a board that does not answer is power cycled
 * once, then marked broken, a file under $XDG_DATA_HOME/hull/boards, out of
 * its pool until `hull board fix NAME`, so a core that dies and the next
 * agree on it.
 */

#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "hull_engine.h"
#include "sexpr.h"
#include "access.h"
#include "builtin.h"
#include "hw.h"

#define MAX_ENV        64u
#define MAX_ARGS       64u
#define MAX_BUSY       64u
#define MAX_PORTS      32u
#define SCRIPT_MAX     32768u
#define WAIT_SECONDS   60     /* a board's sshd, from its power on, unless it says */
#define HOOK_SECONDS   60     /* power and health */

struct hull_stream {
	hull_machine *machine;
	hull_core    *core;
	pid_t         pid;      /* the local ssh */
	int           in;
	int           out;
	int           err;
	int           socket;   /* a dial's connection, or -1 */
	pthread_t     thread;
};

struct hull_machine {
	hull_core      *core;
	hull_board      board;
	char           *pool;
	char           *source;       /* the tree here */
	char           *workdir;
	char           *argv[MAX_ARGS];
	unsigned        argc;
	char           *env[MAX_ENV];
	unsigned        env_count;
	int             stop_signal;
	unsigned        port_host[MAX_PORTS];
	unsigned        port_guest[MAX_PORTS];
	unsigned        port_count;
	char            address[16];  /* the machine's, where its ports are */
	pid_t           forward;      /* the ssh holding them, or 0 */
	char            id[80];
	char            tree[640];    /* where it lands on the board */
	pid_t           workload;     /* the local ssh running the command */
	pid_t           console;      /* the console hook, or 0 */
	pthread_t       runner;
	int             running;
	int             stopping;
	int             done;
	pthread_mutex_t lock;
	pthread_cond_t  done_cond;
};

static const hull_host_ops *host;
static pthread_mutex_t      busy_lock = PTHREAD_MUTEX_INITIALIZER;
static char                 busy[MAX_BUSY][64];

static hull_status fail(hull_error *error, hull_status code, const char *detail)
{
	if (error != NULL) {
		error->code   = code;
		error->detail = detail;
	}
	return code;
}

/* --- boards -------------------------------------------------------------- */

static const char *config_path(char *path, size_t cap)
{
	const char *config = getenv("XDG_CONFIG_HOME");
	const char *home   = getenv("HOME");

	if (config != NULL && config[0] == '/')
		snprintf(path, cap, "%s/hull/pools.conf", config);
	else if (home != NULL)
		snprintf(path, cap, "%s/.config/hull/pools.conf", home);
	else
		return NULL;
	return path;
}

static void field(const hull_sexpr_doc *doc, const hull_sexpr_form *record, const char *key,
                  char *out, size_t cap)
{
	hull_sexpr_form found;
	hull_sexpr_form head;
	hull_sexpr_form value;
	size_t          length = 0u;

	out[0] = '\0';
	if (hull_access_find(doc, record, key, &found) && hull_sexpr_child(doc, &found, &head) &&
	    hull_sexpr_next(doc, &head, &value) &&
	    hull_sexpr_value(doc, &value, (uint8_t *)out, cap - 1u, &length) == HULL_SEXPR_OK)
		out[length] = '\0';
}

int hull_hw_boards(const char *pool, hull_board *boards, unsigned cap)
{
	char            path[4200];
	uint8_t        *text;
	size_t          length = 0u;
	hull_sexpr_doc  doc;
	hull_sexpr_form top;
	hull_sexpr_form entry;
	hull_sexpr_form members;
	hull_sexpr_form item;
	FILE           *file;
	int             count = -1;

	if (config_path(path, sizeof path) == NULL || (file = fopen(path, "re")) == NULL)
		return -1;
	if ((text = malloc(65536u)) == NULL) {
		fclose(file);
		return -1;
	}
	length = fread(text, 1u, 65536u, file);
	fclose(file);
	if (hull_sexpr_open(&doc, text, length, HULL_SEXPR_READABLE, NULL) == HULL_SEXPR_OK &&
	    hull_sexpr_first(&doc, &top) && hull_access_find(&doc, &top, pool, &entry) &&
	    hull_access_find(&doc, &entry, "members", &members) &&
	    hull_sexpr_child(&doc, &members, &item)) {
		count = 0;
		while (hull_sexpr_next(&doc, &item, &item) && (unsigned)count < cap) {
			hull_board     *b = &boards[count];
			hull_sexpr_form head;
			hull_sexpr_form name;
			size_t          n = 0u;
			char            port[16];

			memset(b, 0, sizeof *b);
			if (!hull_sexpr_child(&doc, &item, &head) || !hull_sexpr_is(&doc, &head, "board") ||
			    !hull_sexpr_next(&doc, &head, &name) ||
			    hull_sexpr_value(&doc, &name, (uint8_t *)b->name, sizeof b->name - 1u, &n) !=
			        HULL_SEXPR_OK)
				continue;
			b->name[n] = '\0';
			field(&doc, &item, "address", b->address, sizeof b->address);
			field(&doc, &item, "login", b->login, sizeof b->login);
			field(&doc, &item, "power", b->power, sizeof b->power);
			field(&doc, &item, "console", b->console, sizeof b->console);
			field(&doc, &item, "health", b->health, sizeof b->health);
			field(&doc, &item, "root", b->root, sizeof b->root);
			field(&doc, &item, "port", port, sizeof port);
			b->port = port[0] != '\0' ? (unsigned)strtoul(port, NULL, 10) : 22u;
			field(&doc, &item, "wait", port, sizeof port);
			b->wait = port[0] != '\0' ? (unsigned)strtoul(port, NULL, 10) : WAIT_SECONDS;
			if (b->root[0] == '\0')
				snprintf(b->root, sizeof b->root, "/var/tmp/hull");
			if (b->address[0] != '\0')
				count++;
		}
	}
	free(text);
	return count;
}

/* Where a board's mark is: under the data directory, as a kept machine's. */
static int mark_path(const char *name, char *path, size_t cap)
{
	const char *data = getenv("XDG_DATA_HOME");
	const char *home = getenv("HOME");

	if (strchr(name, '/') != NULL || name[0] == '.')
		return -1;
	if (data != NULL && data[0] == '/')
		snprintf(path, cap, "%s/hull/boards/%s.broken", data, name);
	else if (home != NULL)
		snprintf(path, cap, "%s/.local/share/hull/boards/%s.broken", home, name);
	else
		return -1;
	return 0;
}

int hull_hw_broken(const char *name)
{
	char path[4200];

	return mark_path(name, path, sizeof path) == 0 && access(path, F_OK) == 0;
}

int hull_hw_fix(const char *name)
{
	char path[4200];

	return mark_path(name, path, sizeof path) == 0 && unlink(path) == 0 ? 0 : -1;
}

static void mark_broken(const char *name, const char *why)
{
	char  path[4200];
	char *slash;
	FILE *file;

	if (mark_path(name, path, sizeof path) != 0)
		return;
	/* Each directory of it, the last being boards/. */
	for (slash = strchr(path + 1, '/'); slash != NULL; slash = strchr(slash + 1, '/')) {
		*slash = '\0';
		mkdir(path, 0755);
		*slash = '/';
	}
	if ((file = fopen(path, "we")) != NULL) {
		fprintf(file, "%s\n", why);
		fclose(file);
	}
}

/* A board of the pool neither held nor broken, taken: 0, or -1. */
static int take_board(const char *pool, hull_board *board)
{
	hull_board boards[MAX_BUSY];
	int        count = hull_hw_boards(pool, boards, MAX_BUSY);
	int        taken = -1;

	pthread_mutex_lock(&busy_lock);
	for (int i = 0; i < count && taken < 0; i++) {
		int held = hull_hw_broken(boards[i].name);

		for (unsigned k = 0u; k < MAX_BUSY && !held; k++)
			held = strcmp(busy[k], boards[i].name) == 0;
		if (held)
			continue;
		for (unsigned k = 0u; k < MAX_BUSY; k++)
			if (busy[k][0] == '\0') {
				memcpy(busy[k], boards[i].name, sizeof busy[k]);
				*board = boards[i];
				taken = i;
				break;
			}
	}
	pthread_mutex_unlock(&busy_lock);
	return taken < 0 ? -1 : 0;
}

static void give_board(const hull_board *board)
{
	pthread_mutex_lock(&busy_lock);
	for (unsigned k = 0u; k < MAX_BUSY; k++)
		if (strcmp(busy[k], board->name) == 0)
			busy[k][0] = '\0';
	pthread_mutex_unlock(&busy_lock);
}

/* --- commands ------------------------------------------------------------ */

/* `text`, single-quoted for a POSIX shell, added at `*at`: 0, or -1 full. */
static int quote(char *script, size_t cap, size_t *at, const char *text)
{
	if (*at + 3u > cap)
		return -1;
	script[(*at)++] = '\'';
	for (; *text != '\0'; text++) {
		if (*at + 6u > cap)
			return -1;
		if (*text == '\'') {
			memcpy(script + *at, "'\\''", 4u);
			*at += 4u;
		} else {
			script[(*at)++] = *text;
		}
	}
	script[(*at)++] = '\'';
	script[*at] = '\0';
	return 0;
}

static int add(char *script, size_t cap, size_t *at, const char *text)
{
	size_t length = strlen(text);

	if (*at + length + 1u > cap)
		return -1;
	memcpy(script + *at, text, length + 1u);
	*at += length;
	return 0;
}

/* The local ssh to the board, `script` its remote command, in `argv`. */
static void ssh_words(const hull_board *board, const char *script, const char **argv, char *port,
                      size_t port_cap, char *target, size_t target_cap)
{
	unsigned n = 0u;

	snprintf(port, port_cap, "%u", board->port);
	if (board->login[0] != '\0')
		snprintf(target, target_cap, "%s@%s", board->login, board->address);
	else
		snprintf(target, target_cap, "%s", board->address);
	argv[n++] = "ssh";
	argv[n++] = "-o";
	argv[n++] = "BatchMode=yes";
	argv[n++] = "-o";
	argv[n++] = "ConnectTimeout=5";
	argv[n++] = "-p";
	argv[n++] = port;
	argv[n++] = target;
	argv[n++] = script;
	argv[n]   = NULL;
}

/* A child that goes with the core: stdio given, the rest closed. */
static pid_t spawn(const char *const *argv, int in, int out, int err)
{
	pid_t parent = getpid();
	pid_t pid = fork();

	if (pid != 0)
		return pid;
	if (dup2(in, 0) < 0 || dup2(out, 1) < 0 || dup2(err, 2) < 0 || setsid() < 0 ||
	    prctl(PR_SET_PDEATHSIG, SIGKILL, 0L, 0L, 0L) != 0 || getppid() != parent)
		_exit(126);
	close_range(3u, ~0u, 0u);
	execvp(argv[0], (char *const *)argv);
	_exit(127);
}

/* `argv` to its end, or killed at `seconds` or a stop: its status, -1 where
 * it was killed or would not start. */
static int run_quietly(hull_machine *m, const char *const *argv, int seconds)
{
	int   null = open("/dev/null", O_RDWR | O_CLOEXEC);
	pid_t pid = null >= 0 ? spawn(argv, null, null, null) : -1;
	int   status = 0;

	if (null >= 0)
		close(null);
	if (pid < 0)
		return -1;
	for (int tenths = 0; tenths < seconds * 10; tenths++) {
		pid_t ended = waitpid(pid, &status, WNOHANG);

		if (ended == pid)
			return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
		if (m != NULL && __atomic_load_n(&m->stopping, __ATOMIC_RELAXED))
			break;
		usleep(100000);
	}
	kill(pid, SIGKILL);
	waitpid(pid, &status, 0);
	return -1;
}

static int board_ssh(hull_machine *m, const char *script, int seconds)
{
	const char *argv[12];
	char        port[16];
	char        target[256];

	ssh_words(&m->board, script, argv, port, sizeof port, target, sizeof target);
	return run_quietly(m, argv, seconds);
}

/* A hook, `word` after it where there is one: 0 for no hook, or its status. */
static int hook(hull_machine *m, const char *command, const char *word)
{
	char        line[1100];
	const char *argv[4] = { "sh", "-c", line, NULL };

	if (command[0] == '\0')
		return 0;
	snprintf(line, sizeof line, "%s%s%s", command, word != NULL ? " " : "",
	         word != NULL ? word : "");
	return run_quietly(m, argv, HOOK_SECONDS);
}

static void say(hull_machine *m, const char *format, const char *a, const char *b)
{
	char line[1200];
	int  n = snprintf(line, sizeof line, format, a, b);

	if (n > 0)
		host->on_data(m->core, NULL, 2, (const uint8_t *)line,
		              (size_t)n < sizeof line ? (size_t)n : sizeof line - 1u);
}

/* Its sshd answers within the wait: 1, or 0. */
static int answers(hull_machine *m)
{
	for (unsigned tries = 0u; tries < m->board.wait; tries++) {
		if (__atomic_load_n(&m->stopping, __ATOMIC_RELAXED))
			return 0;
		if (board_ssh(m, "true", 8) == 0)
			return 1;
		sleep(1);
	}
	return 0;
}

/* The tree onto the board, what was there before gone: 0, or -1. */
static int deploy(hull_machine *m)
{
	char        line[SCRIPT_MAX];
	char        remote[2048];
	char        word[1400];
	size_t      at = 0u;
	size_t      remote_at = 0u;
	const char *argv[4] = { "sh", "-c", line, NULL };

	if (add(remote, sizeof remote, &remote_at, "rm -rf ") != 0 ||
	    quote(remote, sizeof remote, &remote_at, m->tree) != 0 ||
	    add(remote, sizeof remote, &remote_at, " && mkdir -p ") != 0 ||
	    quote(remote, sizeof remote, &remote_at, m->tree) != 0 ||
	    add(remote, sizeof remote, &remote_at, " && tar -C ") != 0 ||
	    quote(remote, sizeof remote, &remote_at, m->tree) != 0 ||
	    add(remote, sizeof remote, &remote_at, " -xf -") != 0)
		return -1;
	if (m->board.login[0] != '\0')
		snprintf(word, sizeof word, "%s@%s", m->board.login, m->board.address);
	else
		snprintf(word, sizeof word, "%s", m->board.address);
	if (add(line, sizeof line, &at, "tar -C ") != 0 ||
	    quote(line, sizeof line, &at, m->source) != 0 ||
	    add(line, sizeof line, &at, " -cf - . | ssh -o BatchMode=yes -o ConnectTimeout=5 -p ") != 0)
		return -1;
	{
		char port[16];

		snprintf(port, sizeof port, "%u ", m->board.port);
		if (add(line, sizeof line, &at, port) != 0)
			return -1;
	}
	if (quote(line, sizeof line, &at, word) != 0 || add(line, sizeof line, &at, " ") != 0 ||
	    quote(line, sizeof line, &at, remote) != 0)
		return -1;
	return run_quietly(m, argv, 600) == 0 ? 0 : -1;
}

/* The remote command that enters the tree and runs `argv` there, its pid in
 * `pid_file` where one is named. */
static int enter_script(const hull_machine *m, char *const *argv, unsigned argc,
                        const char *pid_file, char *script, size_t cap)
{
	size_t at = 0u;

	script[0] = '\0';
	if (pid_file != NULL && (add(script, cap, &at, "echo $$ > ") != 0 ||
	                         quote(script, cap, &at, pid_file) != 0 ||
	                         add(script, cap, &at, "; ") != 0))
		return -1;
	if (add(script, cap, &at, "exec env ") != 0)
		return -1;
	for (unsigned i = 0u; i < m->env_count; i++)
		if (quote(script, cap, &at, m->env[i]) != 0 || add(script, cap, &at, " ") != 0)
			return -1;
	/* A login that is not root enters by a user namespace of its own. */
	if (add(script, cap, &at, "unshare $(test \"$(id -u)\" = 0 || echo -r) --root=") != 0 ||
	    quote(script, cap, &at, m->tree) != 0 || add(script, cap, &at, " --wd=") != 0 ||
	    quote(script, cap, &at, m->workdir) != 0 || add(script, cap, &at, " --") != 0)
		return -1;
	for (unsigned i = 0u; i < argc; i++)
		if (add(script, cap, &at, " ") != 0 || quote(script, cap, &at, argv[i]) != 0)
			return -1;
	return 0;
}

/* What comes from the two pipes, as `st`'s or the workload's, to their end. */
static void read_output(hull_machine *m, hull_stream *st, int out, int err)
{
	struct pollfd fds[2] = { { .fd = out, .events = POLLIN }, { .fd = err, .events = POLLIN } };
	uint8_t       buffer[8192];

	while (fds[0].fd >= 0 || fds[1].fd >= 0) {
		if (poll(fds, 2u, -1) < 0) {
			if (errno == EINTR)
				continue;
			break;
		}
		for (int i = 0; i < 2; i++) {
			ssize_t n;

			if (fds[i].fd < 0 || fds[i].revents == 0)
				continue;
			n = read(fds[i].fd, buffer, sizeof buffer);
			if (n > 0)
				host->on_data(m->core, st, i + 1, buffer, (size_t)n);
			else if (n == 0 || errno != EINTR)
				fds[i].fd = -1;
		}
	}
}

/* The console hook's words, as the machine's stderr, for as long as it runs. */
static void *read_console(void *arg)
{
	hull_machine *m = ((void **)arg)[0];
	int           from = (int)(intptr_t)((void **)arg)[1];
	uint8_t       buffer[4096];
	ssize_t       n;

	free(arg);
	while ((n = read(from, buffer, sizeof buffer)) > 0 || (n < 0 && errno == EINTR))
		if (n > 0)
			host->on_data(m->core, NULL, 2, buffer, (size_t)n);
	close(from);
	return NULL;
}

static void start_console(hull_machine *m)
{
	int         ends[2];
	int         null;
	const char *argv[4] = { "sh", "-c", m->board.console, NULL };
	pthread_t   thread;
	void      **arg;

	if (m->board.console[0] == '\0' || pipe2(ends, O_CLOEXEC) != 0)
		return;
	null = open("/dev/null", O_RDONLY | O_CLOEXEC);
	m->console = spawn(argv, null, ends[1], ends[1]);
	close(null);
	close(ends[1]);
	if (m->console < 0 || (arg = malloc(2u * sizeof *arg)) == NULL) {
		close(ends[0]);
		return;
	}
	arg[0] = m;
	arg[1] = (void *)(intptr_t)ends[0];
	if (pthread_create(&thread, NULL, read_console, arg) != 0) {
		free(arg);
		close(ends[0]);
		return;
	}
	pthread_detach(thread);
}

static void end_console(hull_machine *m)
{
	if (m->console > 0) {
		kill(-m->console, SIGKILL);
		kill(m->console, SIGKILL);
		waitpid(m->console, NULL, 0);
		m->console = 0;
	}
}

/* Power up and answering, cycled once where not: 1, or 0 with it marked. */
static int bring_up(hull_machine *m)
{
	if (hook(m, m->board.power, "on") != 0) {
		say(m, "hull: board %s: its power hook failed%s\n", m->board.name, "");
		mark_broken(m->board.name, "its power hook failed");
		return 0;
	}
	start_console(m);
	if (answers(m))
		return 1;
	if (__atomic_load_n(&m->stopping, __ATOMIC_RELAXED))
		return 0;
	say(m, "hull: board %s: no sshd at %s; cycling its power\n", m->board.name, m->board.address);
	if (m->board.power[0] != '\0' && hook(m, m->board.power, "cycle") == 0 && answers(m))
		return 1;
	if (!__atomic_load_n(&m->stopping, __ATOMIC_RELAXED)) {
		say(m, "hull: board %s: marked broken: no sshd at %s\n", m->board.name, m->board.address);
		mark_broken(m->board.name, "no sshd after a power cycle");
	}
	return 0;
}

/* The machine's ports at its own address, goal 7's, by one ssh -L each to the
 * board's loopback: a board's network is its own and hull cannot reach into
 * it otherwise. 0, or -1 with the port said. */
static int start_ports(hull_machine *m)
{
	const char *argv[16 + 2u * MAX_PORTS];
	char        forwards[MAX_PORTS][64];
	char        port[16];
	char        target[256];
	unsigned    n = 0u;
	int         null;

	if (m->port_count == 0u)
		return 0;
	snprintf(port, sizeof port, "%u", m->board.port);
	if (m->board.login[0] != '\0')
		snprintf(target, sizeof target, "%s@%s", m->board.login, m->board.address);
	else
		snprintf(target, sizeof target, "%s", m->board.address);
	argv[n++] = "ssh";
	argv[n++] = "-N";
	argv[n++] = "-o";
	argv[n++] = "BatchMode=yes";
	argv[n++] = "-o";
	argv[n++] = "ExitOnForwardFailure=yes";
	argv[n++] = "-p";
	argv[n++] = port;
	for (unsigned i = 0u; i < m->port_count; i++) {
		snprintf(forwards[i], sizeof forwards[i], "%s:%u:127.0.0.1:%u", m->address,
		         m->port_host[i], m->port_guest[i]);
		argv[n++] = "-L";
		argv[n++] = forwards[i];
	}
	argv[n++] = target;
	argv[n]   = NULL;
	null = open("/dev/null", O_RDWR | O_CLOEXEC);
	m->forward = null >= 0 ? spawn(argv, null, null, null) : -1;
	if (null >= 0)
		close(null);
	if (m->forward <= 0) {
		m->forward = 0;
		return -1;
	}
	return 0;
}

static void end_ports(hull_machine *m)
{
	if (m->forward > 0) {
		kill(m->forward, SIGTERM);
		waitpid(m->forward, NULL, 0);
		m->forward = 0;
	}
}

/* The workload to its end: its exit status, 125 for a failure of hull's. */
static int run_workload(hull_machine *m)
{
	const char *argv[12];
	char        port[16];
	char        target[256];
	char        pid_file[700];
	char       *script = malloc(SCRIPT_MAX);
	int         out[2];
	int         err[2];
	int         null;
	int         status = 0;
	pid_t       pid;

	snprintf(pid_file, sizeof pid_file, "%s.pid", m->tree);
	if (script == NULL || enter_script(m, m->argv, m->argc, pid_file, script, SCRIPT_MAX) != 0) {
		free(script);
		say(m, "hull: board %s: the command is too long%s\n", m->board.name, "");
		return 125;
	}
	if (pipe2(out, O_CLOEXEC) != 0 || pipe2(err, O_CLOEXEC) != 0) {
		free(script);
		return 125;
	}
	null = open("/dev/null", O_RDONLY | O_CLOEXEC);
	ssh_words(&m->board, script, argv, port, sizeof port, target, sizeof target);
	pid = spawn(argv, null, out[1], err[1]);
	close(null);
	close(out[1]);
	close(err[1]);
	free(script);
	if (pid < 0) {
		close(out[0]);
		close(err[0]);
		return 125;
	}
	pthread_mutex_lock(&m->lock);
	m->workload = pid;
	pthread_mutex_unlock(&m->lock);
	read_output(m, NULL, out[0], err[0]);
	close(out[0]);
	close(err[0]);
	waitpid(pid, &status, 0);
	pthread_mutex_lock(&m->lock);
	m->workload = 0;
	pthread_mutex_unlock(&m->lock);
	if (WIFSIGNALED(status))
		return 128 + WTERMSIG(status);
	return WEXITSTATUS(status);
}

/* A deploy failed: section 4.12's order, the power cycled and it tried again. */
static int redeployed(hull_machine *m)
{
	if (__atomic_load_n(&m->stopping, __ATOMIC_RELAXED) || m->board.power[0] == '\0')
		return 0;
	say(m, "hull: board %s: the deploy failed; cycling its power%s\n", m->board.name, "");
	return hook(m, m->board.power, "cycle") == 0 && answers(m) && deploy(m) == 0;
}

static void *run_machine(void *arg)
{
	hull_machine *m = arg;
	int           code = 125;
	char          clean[1600];
	size_t        at = 0u;

	if (bring_up(m)) {
		if (hook(m, m->board.health, NULL) != 0) {
			say(m, "hull: board %s: marked broken: its health check failed%s\n", m->board.name, "");
			mark_broken(m->board.name, "its health check failed");
		} else if (deploy(m) != 0 && !redeployed(m)) {
			say(m, "hull: board %s: marked broken: the tree could not be deployed to %s\n",
			    m->board.name, m->tree);
			mark_broken(m->board.name, "a deploy failed after a power cycle");
		} else {
			if (start_ports(m) != 0)
				say(m, "hull: board %s: its ports could not be forwarded%s\n", m->board.name, "");
			host->on_state(m->core, m, "running");
			code = run_workload(m);
			end_ports(m);
			/* ssh's own failure, the board gone from under the command. */
			if (code == 255 && board_ssh(m, "true", 8) != 0) {
				say(m, "hull: board %s: marked broken: it went away during the run%s\n",
				    m->board.name, "");
				mark_broken(m->board.name, "it went away during a run");
				code = 125;
			}
		}
		/* A stop has been honoured; the tree goes all the same. */
		__atomic_store_n(&m->stopping, 0, __ATOMIC_RELAXED);
		if (add(clean, sizeof clean, &at, "rm -rf ") == 0 &&
		    quote(clean, sizeof clean, &at, m->tree) == 0 &&
		    add(clean, sizeof clean, &at, " ") == 0) {
			char pid_file[700];

			snprintf(pid_file, sizeof pid_file, "%s.pid", m->tree);
			if (quote(clean, sizeof clean, &at, pid_file) == 0)
				board_ssh(m, clean, 15);
		}
	}
	__atomic_store_n(&m->stopping, 0, __ATOMIC_RELAXED);
	end_console(m);
	hook(m, m->board.power, "off");
	give_board(&m->board);
	pthread_mutex_lock(&m->lock);
	m->done = 1;
	pthread_cond_broadcast(&m->done_cond);
	pthread_mutex_unlock(&m->lock);
	host->on_exit(m->core, m, NULL, code, 0);
	return NULL;
}

/* --- the plan ------------------------------------------------------------ */

static const char *read_plan(hull_machine *m, const hull_sexpr_doc *doc)
{
	hull_sexpr_form machine;
	hull_sexpr_form entry;
	hull_sexpr_form item;
	char           *source;
	char           *name;
	int             more;

	if (!hull_sexpr_first(doc, &machine) || !hull_access_find(doc, &machine, "machine", &machine))
		return "the plan has no machine";
	source = hull_access_value(doc, &machine, "source");
	if (source == NULL || strncmp(source, "dir:", 4u) != 0) {
		free(source);
		return "a board takes a dir: source, deployed to it";
	}
	m->source = hull_access_path(doc, source + 4);
	free(source);
	m->pool    = hull_access_value(doc, &machine, "pool");
	m->workdir = hull_access_value(doc, &machine, "workdir");
	name       = hull_access_value(doc, &machine, "hostname");
	snprintf(m->id, sizeof m->id, "%s", name != NULL ? name : "machine");
	free(name);
	if (m->pool == NULL)
		return "a board comes from a pool: (pool \"NAME\"), its members in pools.conf";
	if (m->source == NULL || m->workdir == NULL)
		return "the plan is missing its source or workdir";
	if (!hull_access_find(doc, &machine, "command", &entry))
		return "the spec has no command";
	hull_sexpr_child(doc, &entry, &item);
	for (more = hull_sexpr_next(doc, &item, &item); more;
	     more = hull_sexpr_next(doc, &item, &item)) {
		if (m->argc == MAX_ARGS - 1u)
			return "the command has too many arguments";
		if ((m->argv[m->argc++] = hull_access_text(doc, &item)) == NULL)
			return "an argument holds a NUL byte";
	}
	if (hull_access_find(doc, &machine, "env", &entry)) {
		hull_sexpr_child(doc, &entry, &item);
		for (more = hull_sexpr_next(doc, &item, &item); more;
		     more = hull_sexpr_next(doc, &item, &item)) {
			hull_sexpr_form key;
			hull_sexpr_form value;
			char           *k;
			char           *v;

			if (m->env_count == MAX_ENV)
				return "the machine has too many environment variables";
			if (!hull_sexpr_child(doc, &item, &key) || !hull_sexpr_next(doc, &key, &value))
				return "an env entry is a name and a value";
			k = hull_access_text(doc, &key);
			v = hull_access_text(doc, &value);
			if (k != NULL && v != NULL && asprintf(&m->env[m->env_count], "%s=%s", k, v) >= 0)
				m->env_count++;
			free(k);
			free(v);
		}
	}
	if (hull_access_find(doc, &machine, "ports", &entry)) {
		hull_sexpr_form top;
		char           *address = NULL;

		if (hull_sexpr_first(doc, &top))
			address = hull_access_value(doc, &top, "address");
		if (address == NULL || strlen(address) >= sizeof m->address) {
			free(address);
			return "the plan gives its ports no address";
		}
		snprintf(m->address, sizeof m->address, "%s", address);
		free(address);
		hull_sexpr_child(doc, &entry, &item);
		for (more = hull_sexpr_next(doc, &item, &item); more;
		     more = hull_sexpr_next(doc, &item, &item)) {
			char *host_port = hull_access_value(doc, &item, "host");
			char *guest     = hull_access_value(doc, &item, "guest");
			char *proto     = hull_access_value(doc, &item, "proto");
			int   udp = proto != NULL && strcmp(proto, "udp") == 0;

			if (m->port_count < MAX_PORTS && host_port != NULL && guest != NULL) {
				m->port_host[m->port_count]  = (unsigned)strtoul(host_port, NULL, 10);
				m->port_guest[m->port_count] = (unsigned)strtoul(guest, NULL, 10);
				m->port_count++;
			}
			free(host_port);
			free(guest);
			free(proto);
			if (udp)
				return "a board's ports are ssh's forwards, which are TCP";
		}
	}
	name = hull_access_value(doc, &machine, "stop_signal");
	m->stop_signal = name != NULL ? hull_access_signal(name) : SIGTERM;
	free(name);
	if (m->stop_signal == 0)
		return "(stop_signal ...) names no signal";
	return NULL;
}

static void release(hull_machine *m)
{
	for (unsigned i = 0u; i < m->argc; i++)
		free(m->argv[i]);
	for (unsigned i = 0u; i < m->env_count; i++)
		free(m->env[i]);
	free(m->pool);
	free(m->source);
	free(m->workdir);
	pthread_mutex_destroy(&m->lock);
	pthread_cond_destroy(&m->done_cond);
	free(m);
}

static hull_status hw_create(hull_core *core, hull_buf plan, hull_machine **out, hull_error *error)
{
	static char     why[400];
	hull_sexpr_doc  doc;
	hull_machine   *m;
	const char     *reason;

	if (hull_sexpr_open(&doc, plan.data, plan.len, HULL_SEXPR_CANONICAL, NULL) != HULL_SEXPR_OK)
		return fail(error, HULL_ERR_UNSUPPORTED, "the plan is not canonical");
	if ((m = calloc(1u, sizeof *m)) == NULL)
		return fail(error, HULL_ERR_INTERNAL, "out of memory");
	m->core = core;
	pthread_mutex_init(&m->lock, NULL);
	pthread_cond_init(&m->done_cond, NULL);
	if ((reason = read_plan(m, &doc)) != NULL) {
		snprintf(why, sizeof why, "%s", reason);
		release(m);
		return fail(error, HULL_ERR_UNSUPPORTED, why);
	}
	if (take_board(m->pool, &m->board) != 0) {
		snprintf(why, sizeof why, "pool %s has no board free and not broken: `hull board ls`",
		         m->pool);
		release(m);
		return fail(error, HULL_ERR_RUNTIME, why);
	}
	snprintf(m->tree, sizeof m->tree, "%s/%s", m->board.root, m->id);
	*out = m;
	return HULL_OK;
}

static hull_status hw_start(hull_core *core, hull_machine *m, hull_error *error)
{
	(void)core;
	if (pthread_create(&m->runner, NULL, run_machine, m) != 0)
		return fail(error, HULL_ERR_INTERNAL, "the machine's thread could not start");
	m->running = 1;
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

/* `signum` to the workload on the board, by the pid it left beside its tree. */
static void signal_board(hull_machine *m, int signum)
{
	char   script[900];
	char   pid_file[700];
	size_t at = 0u;
	char   word[32];

	snprintf(pid_file, sizeof pid_file, "%s.pid", m->tree);
	snprintf(word, sizeof word, "kill -%d $(cat ", signum);
	if (add(script, sizeof script, &at, word) == 0 &&
	    quote(script, sizeof script, &at, pid_file) == 0 &&
	    add(script, sizeof script, &at, ")") == 0)
		board_ssh(m, script, 15);
}

static hull_status hw_stop(hull_core *core, hull_machine *m, int timeout_ms, hull_error *error)
{
	pid_t workload;

	(void)error;
	if (!m->running || wait_done(m, 0))
		return HULL_OK;
	host->on_state(core, m, "stopping");
	pthread_mutex_lock(&m->lock);
	workload = m->workload;
	pthread_mutex_unlock(&m->lock);
	if (workload <= 0) {
		/* Not running yet: what it is waiting on gives up. */
		__atomic_store_n(&m->stopping, 1, __ATOMIC_RELAXED);
		wait_done(m, 60000);
		return HULL_OK;
	}
	signal_board(m, m->stop_signal);
	if (!wait_done(m, timeout_ms)) {
		signal_board(m, SIGKILL);
		if (!wait_done(m, 15000)) {
			pthread_mutex_lock(&m->lock);
			if (m->workload > 0)
				kill(m->workload, SIGKILL);
			pthread_mutex_unlock(&m->lock);
			wait_done(m, 60000);
		}
	}
	return HULL_OK;
}

static hull_status hw_destroy(hull_core *core, hull_machine *m, hull_error *error)
{
	if (m->running) {
		hw_stop(core, m, 0, error);
		pthread_join(m->runner, NULL);
	}
	release(m);
	return HULL_OK;
}

static hull_status hw_signal(hull_core *core, hull_machine *m, hull_stream *stream, int signum,
                             hull_error *error)
{
	(void)core;
	(void)error;
	if (stream != NULL) {
		if (stream->pid > 0)
			kill(stream->pid, signum);
		if (stream->socket >= 0)
			shutdown(stream->socket, SHUT_RDWR);
		return HULL_OK;
	}
	signal_board(m, signum);
	return HULL_OK;
}

/* --- exec and dial ------------------------------------------------------- */

static void *deliver(void *arg)
{
	hull_stream *st = arg;
	int          status = 0;

	read_output(st->machine, st, st->out, st->err);
	waitpid(st->pid, &status, 0);
	host->on_exit(st->core, st->machine, st, WIFEXITED(status) ? WEXITSTATUS(status) : 0,
	              WIFSIGNALED(status) ? WTERMSIG(status) : 0);
	return NULL;
}

static hull_status hw_exec(hull_core *core, hull_machine *m, hull_buf spec, hull_stream **out,
                           hull_error *error)
{
	hull_sexpr_doc  doc;
	hull_sexpr_form message;
	hull_sexpr_form argv_form;
	hull_sexpr_form word;
	char           *words[MAX_ARGS];
	unsigned        count = 0u;
	char           *script;
	const char     *argv[12];
	char            port[16];
	char            target[256];
	int             in[2];
	int             outs[2];
	int             errs[2];
	hull_stream    *st;

	if (hull_sexpr_open(&doc, spec.data, spec.len, HULL_SEXPR_CANONICAL, NULL) != HULL_SEXPR_OK ||
	    !hull_sexpr_first(&doc, &message) || !hull_access_find(&doc, &message, "argv", &argv_form))
		return fail(error, HULL_ERR_UNSUPPORTED, "an exec on a board names a command");
	hull_sexpr_child(&doc, &argv_form, &word);
	while (count < MAX_ARGS && hull_sexpr_next(&doc, &word, &word))
		if ((words[count] = hull_access_text(&doc, &word)) != NULL)
			count++;
	if ((script = malloc(SCRIPT_MAX)) == NULL ||
	    enter_script(m, words, count, NULL, script, SCRIPT_MAX) != 0) {
		for (unsigned i = 0u; i < count; i++)
			free(words[i]);
		free(script);
		return fail(error, HULL_ERR_RUNTIME, "the command is too long");
	}
	for (unsigned i = 0u; i < count; i++)
		free(words[i]);
	if ((st = calloc(1u, sizeof *st)) == NULL || pipe2(in, O_CLOEXEC) != 0 ||
	    pipe2(outs, O_CLOEXEC) != 0 || pipe2(errs, O_CLOEXEC) != 0) {
		free(script);
		free(st);
		return fail(error, HULL_ERR_RUNTIME, "pipes for the command could not be made");
	}
	ssh_words(&m->board, script, argv, port, sizeof port, target, sizeof target);
	st->machine = m;
	st->core    = core;
	st->socket  = -1;
	st->pid     = spawn(argv, in[0], outs[1], errs[1]);
	free(script);
	close(in[0]);
	close(outs[1]);
	close(errs[1]);
	st->in  = in[1];
	st->out = outs[0];
	st->err = errs[0];
	if (st->pid < 0 || pthread_create(&st->thread, NULL, deliver, st) != 0) {
		close(st->in);
		close(st->out);
		close(st->err);
		free(st);
		return fail(error, HULL_ERR_RUNTIME, "ssh could not be started");
	}
	*out = st;
	return HULL_OK;
}

static void *deliver_socket(void *arg)
{
	hull_stream *st = arg;
	uint8_t      buffer[16384];
	ssize_t      n;

	while ((n = read(st->socket, buffer, sizeof buffer)) > 0 || (n < 0 && errno == EINTR))
		if (n > 0)
			host->on_data(st->core, st, 1, buffer, (size_t)n);
	host->on_exit(st->core, st->machine, st, 0, 0);
	return NULL;
}

/* A port of the board's own, by its address: nothing of hull's is between. */
static hull_status hw_dial(hull_core *core, hull_machine *m, uint16_t port, hull_stream **out,
                           hull_error *error)
{
	struct addrinfo  hints = { .ai_socktype = SOCK_STREAM };
	struct addrinfo *found = NULL;
	char             service[8];
	hull_stream     *st;
	int              fd = -1;

	snprintf(service, sizeof service, "%u", port == 22u ? m->board.port : (unsigned)port);
	if (getaddrinfo(m->board.address, service, &hints, &found) != 0)
		return fail(error, HULL_ERR_RUNTIME, "the board's address does not resolve");
	for (struct addrinfo *a = found; a != NULL && fd < 0; a = a->ai_next) {
		fd = socket(a->ai_family, a->ai_socktype | SOCK_CLOEXEC, a->ai_protocol);
		if (fd >= 0 && connect(fd, a->ai_addr, a->ai_addrlen) != 0) {
			close(fd);
			fd = -1;
		}
	}
	freeaddrinfo(found);
	if (fd < 0)
		return fail(error, HULL_ERR_RUNTIME, "the board refused the connection");
	if ((st = calloc(1u, sizeof *st)) == NULL) {
		close(fd);
		return fail(error, HULL_ERR_INTERNAL, "out of memory");
	}
	st->machine = m;
	st->core    = core;
	st->socket  = fd;
	st->in = st->out = st->err = -1;
	if (pthread_create(&st->thread, NULL, deliver_socket, st) != 0) {
		close(fd);
		free(st);
		return fail(error, HULL_ERR_INTERNAL, "the connection's thread could not start");
	}
	*out = st;
	return HULL_OK;
}

static hull_status hw_stream_write(hull_core *core, hull_stream *st, const uint8_t *data,
                                   size_t length, hull_error *error)
{
	int to = st->socket >= 0 ? st->socket : st->in;

	(void)core;
	while (length > 0u) {
		ssize_t n = send(to, data, length, MSG_NOSIGNAL);

		if (n < 0 && errno == ENOTSOCK)
			n = write(to, data, length);
		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0)
			return fail(error, HULL_ERR_GONE, "the board has gone");
		data += n;
		length -= (size_t)n;
	}
	return HULL_OK;
}

static hull_status hw_stream_eof(hull_core *core, hull_stream *st, hull_error *error)
{
	(void)core;
	(void)error;
	if (st->socket >= 0) {
		shutdown(st->socket, SHUT_WR);
	} else if (st->in >= 0) {
		close(st->in);
		st->in = -1;
	}
	return HULL_OK;
}

static hull_status hw_stream_close(hull_core *core, hull_stream *st, hull_error *error)
{
	(void)core;
	(void)error;
	if (st->socket >= 0) {
		shutdown(st->socket, SHUT_RDWR);
		pthread_join(st->thread, NULL);
		close(st->socket);
	} else {
		if (st->pid > 0)
			kill(st->pid, SIGKILL);
		pthread_join(st->thread, NULL);
		if (st->in >= 0)
			close(st->in);
		close(st->out);
		close(st->err);
	}
	free(st);
	return HULL_OK;
}

/* --- the engine ---------------------------------------------------------- */

static const hull_cap_entry entries[] = {
	{ "source.dir.direct",  HULL_SUP_EMULATED,
	  "deployed to the board by tar over ssh: its writes stay there and go with the run", NULL },
	{ "source.dir.overlay", HULL_SUP_NATIVE,
	  "a copy on the board, gone with the run, the directory untouched", NULL },
	{ "projection.bind",    HULL_SUP_EMULATED, "a copy on the board", NULL },
	{ "writeback.none",     HULL_SUP_NATIVE,   NULL, NULL },
	{ "command",            HULL_SUP_EMULATED,
	  "run on the board's own kernel in the tree, by unshare --root, with no /proc or /dev "
	  "in it but what the tree brings", NULL },
	{ "workdir",            HULL_SUP_NATIVE,   NULL, NULL },
	{ "env",                HULL_SUP_NATIVE,   NULL, NULL },
	{ "hostname",           HULL_SUP_EMULATED, "the name of its tree: the board keeps its own", NULL },
	{ "pool",               HULL_SUP_NATIVE,   "a board of the pool's members", NULL },
	{ "network.none",       HULL_SUP_EMULATED, "the board's own network, whatever that is", NULL },
	{ "network.nat",        HULL_SUP_EMULATED, "the board's own network, whatever that is", NULL },
	{ "network.host",       HULL_SUP_NATIVE,   "the board's own network", NULL },
	{ "security.no_new_privs", HULL_SUP_EMULATED,
	  "not set: what the command may do is the board's login's", NULL },
	{ "on_disconnect.stop", HULL_SUP_NATIVE,   NULL, NULL },
	{ "ports",              HULL_SUP_EMULATED,
	  "ssh -L from the machine's address to the board's loopback, TCP alone", NULL },
	{ "stop_signal",        HULL_SUP_NATIVE,   NULL, NULL },
	{ "stop_grace",         HULL_SUP_NATIVE,   NULL, NULL },
	{ NULL, HULL_SUP_NATIVE, NULL, NULL },
};

static const char *const classes[] = { "hw", NULL };

static const hull_capabilities caps = {
	.struct_size    = sizeof (hull_capabilities),
	.engine_name    = "board",
	.engine_version = "0.1",
	.classes        = classes,
	.entries        = entries,
	.migration      = HULL_MIG_PINNED,
	.arches         = NULL,
	.requires_root  = 0,
};

static const hull_engine_ops ops = {
	.struct_size  = sizeof (hull_engine_ops),
	.create       = hw_create,
	.start        = hw_start,
	.stop         = hw_stop,
	.destroy      = hw_destroy,
	.signal       = hw_signal,
	.exec         = hw_exec,
	.stream_write = hw_stream_write,
	.stream_eof   = hw_stream_eof,
	.stream_close = hw_stream_close,
	.dial         = hw_dial,
};

static const hull_engine engine = {
	.struct_size = sizeof (hull_engine),
	.abi_major   = HULL_ABI_MAJOR,
	.abi_minor   = HULL_ABI_MINOR,
	.caps        = &caps,
	.ops         = &ops,
};

const hull_engine *hull_engine_hw(const hull_host_ops *host_ops)
{
	if (host_ops != NULL)
		host = host_ops;
	return &engine;
}
