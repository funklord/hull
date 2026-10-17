/*
 * agent.c -- the guest agent (project.md Appendix B), PID 1 in a vm or
 * emu machine.
 *
 * Booted as the initramfs's /init. It mounts the kernel's filesystems,
 * loads the modules the engine put beside it, in the order /hull/modules
 * names them, and listens on vsock port 1 for the one host connection
 * (B.1, B.2). What the host sends is held to agent/protocol.situ's
 * host_message before anything reads it, as everything the core reads is.
 *
 * `spec` carries the resolved machine. The agent mounts the machine's
 * directory, shared by the host over 9p with the tag `root`, and each
 * volume, tagged v0, v1, ... in the order the spec lists them; then it
 * moves the directory onto / and chroots into it, as switch_root does.
 * `start` forks the workload, a session of its own, with the spec's
 * environment, workdir and no_new_privs. Its stdout and stderr come back
 * as `data` on channel 0, which is the workload's, as it is for `signal`.
 *
 * The agent is the machine's init: it reaps whatever is orphaned to it,
 * and once the workload has ended and its output has been read it says
 * `exited`, exactly as the workload ended, ends what is left, says `bye`
 * and powers the machine off. A `stop` is SIGTERM, then SIGKILL at its
 * timeout. A host that goes away is (on_disconnect stop): the same, with
 * nobody to tell.
 *
 * One thread, one poll(): the connection, the workload's two pipes, and a
 * signalfd for SIGCHLD.
 */

#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/prctl.h>
#include <sys/reboot.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <linux/vm_sockets.h>

#include "access.h"
#include "wire.h"

#define AGENT_VERSION "0.1"
#define AGENT_PORT    1u
#define MAX_ARGS      256u
#define MAX_ENV       256u
#define MAX_VOLUMES   64u

#ifndef MODULE_INIT_COMPRESSED_FILE
#define MODULE_INIT_COMPRESSED_FILE 4
#endif

/* The workload, once the spec has said what it is. */
static char  *argv_[MAX_ARGS + 1u];
static char  *envp_[MAX_ENV + 1u];
static char  *workdir;
static int    no_new_privs;
static int    applied;     /* the spec is applied: the root is the machine's */

static pid_t  workload = -1;
static int    out_fd   = -1;
static int    err_fd   = -1;
static int    reaped;      /* the workload has ended; `status` is how */
static int    status;
static int    stopping;
static struct timespec kill_at;

static hull_wire host;
static int       connected;

/* Diagnostics go to the console, which the engine keeps in a file. */
static void say(const char *what, const char *detail)
{
	fprintf(stderr, "hull agent: %s%s%s\n", what, detail != NULL ? ": " : "",
	        detail != NULL ? detail : "");
}

static void power_off(void)
{
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

static void send_data(int fd, const uint8_t *bytes, size_t n)
{
	hull_msg m = { 0 };

	hull_msg_raw(&m, "(4:data(3:buf");
	hull_msg_atom(&m, bytes, n);
	hull_msg_raw(&m, ")");
	hull_msg_channel(&m, 0u);
	hull_msg_raw(&m, fd == 2 ? "(2:fd1:2))" : "(2:fd1:1))");
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

/* Mount the 9p share `tag` at `at`. */
static int share(const char *tag, const char *at, int readonly)
{
	return mount(tag, at, "9p", readonly ? MS_RDONLY : 0u,
	             "trans=virtio,version=9p2000.L,msize=262144");
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
static const char *apply(const hull_sexpr_doc *doc, const hull_sexpr_form *machine)
{
	hull_sexpr_form entry;
	hull_sexpr_form item;
	hull_sexpr_form security;
	char           *hostname;
	unsigned        n = 0u;
	unsigned        volumes = 0u;
	int             readonly_root = 0;
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

	mkdir("/machine", 0755);
	if (share("root", "/machine", readonly_root) != 0)
		return "the machine's directory could not be mounted";
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
	/* The kernel's filesystems inside, where the machine has a place for
	 * them; then the directory becomes /. */
	if (mount("proc", "/machine/proc", "proc", 0u, NULL) != 0 && errno != ENOENT)
		say("proc", strerror(errno));
	if (mount("devtmpfs", "/machine/dev", "devtmpfs", 0u, NULL) != 0 && errno != ENOENT)
		say("dev", strerror(errno));
	if (mount("sysfs", "/machine/sys", "sysfs", 0u, NULL) != 0 && errno != ENOENT)
		say("sys", strerror(errno));
	if (chdir("/machine") != 0 || mount(".", "/", NULL, MS_MOVE, NULL) != 0 ||
	    chroot(".") != 0 || chdir("/") != 0)
		return "the machine's directory could not become its root";
	applied = 1;
	return NULL;
}

/* --- the workload -------------------------------------------------------- */

static const char *start(void)
{
	int out[2];
	int err[2];

	if (!applied)
		return "start before the spec";
	if (workload > 0)
		return "the workload has been started already";
	if (pipe2(out, O_CLOEXEC) != 0 || pipe2(err, O_CLOEXEC) != 0)
		return "pipe";
	workload = fork();
	if (workload < 0)
		return "fork";
	if (workload == 0) {
		sigset_t none;
		int      null = open("/dev/null", O_RDONLY);

		sigemptyset(&none);
		sigprocmask(SIG_SETMASK, &none, NULL);
		setsid();
		if (null >= 0)
			dup2(null, 0);
		dup2(out[1], 1);
		dup2(err[1], 2);
		if (workdir != NULL && chdir(workdir) != 0) {
			fprintf(stderr, "hull agent: workdir %s: %s\n", workdir, strerror(errno));
			_exit(127);
		}
		if (no_new_privs)
			prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0);
		execve(argv_[0], argv_, envp_);
		fprintf(stderr, "hull agent: %s: %s\n", argv_[0], strerror(errno));
		_exit(127);
	}
	close(out[1]);
	close(err[1]);
	out_fd = out[0];
	err_fd = err[0];
	{
		hull_msg m = { 0 };

		hull_msg_raw(&m, "(7:started");
		hull_msg_channel(&m, 0u);
		hull_msg_raw(&m, "(3:pid");
		hull_msg_number(&m, workload);
		hull_msg_raw(&m, "))");
		send_msg(&m);
	}
	return NULL;
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

static void signal_workload(int number)
{
	if (workload > 0 && !reaped)
		kill(workload, number);
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
	signal_workload(SIGTERM);
}

/* Milliseconds until the stop's SIGKILL, or -1 for none pending. */
static int until_kill(void)
{
	struct timespec now;
	long long       left;

	if (!stopping || reaped)
		return -1;
	clock_gettime(CLOCK_MONOTONIC, &now);
	left = (long long)(kill_at.tv_sec - now.tv_sec) * 1000 +
	       (kill_at.tv_nsec - now.tv_nsec) / 1000000L;
	if (left <= 0) {
		signal_workload(SIGKILL);
		return -1;
	}
	return left > 1000 ? 1000 : (int)left;
}

/* Read what one of the workload's pipes holds; 0 once it is closed. */
static int drain(int *fd, int number)
{
	uint8_t buffer[16384];
	ssize_t n = read(*fd, buffer, sizeof buffer);

	if (n > 0) {
		send_data(number, buffer, (size_t)n);
		return 1;
	}
	if (n < 0 && errno == EINTR)
		return 1;
	close(*fd);
	*fd = -1;
	return 0;
}

/* --- the host's messages ------------------------------------------------- */

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
	} else if (hull_msg_is(body, "spec")) {
		hull_sexpr_form machine;

		if (applied)
			reason = "the spec has been applied already";
		else if (!hull_access_find(body, &message, "machine", &machine))
			reason = "the spec holds no machine";
		else
			reason = apply(body, &machine);
		if (reason != NULL)
			send_error(0u, reason);
		else
			send_simple("(5:ready");
	} else if (hull_msg_is(body, "start")) {
		if ((reason = start()) != NULL)
			send_error(0u, reason);
	} else if (hull_msg_is(body, "signal")) {
		char name[16];
		int  number;

		if (!hull_msg_find(body, "signal", &value) ||
		    !hull_msg_string(body, &value, name, sizeof name) ||
		    (number = signal_named(name)) == 0)
			send_error((uint32_t)ch, "not a signal the agent forwards");
		else if (ch != 0)
			send_error((uint32_t)ch, "no such channel");
		else
			signal_workload(number);
	} else if (hull_msg_is(body, "stop")) {
		long long timeout = 10000;

		if (hull_msg_find(body, "timeout_ms", &value))
			hull_msg_long(body, &value, &timeout);
		if (workload <= 0) {
			send_simple("(3:bye");
			power_off();
		}
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
	hull_msg m = { 0 };

	hull_msg_raw(&m, "(6:exited");
	hull_msg_channel(&m, 0u);
	hull_msg_raw(&m, "(4:code");
	hull_msg_number(&m, WIFEXITED(status) ? WEXITSTATUS(status) : 0);
	hull_msg_raw(&m, ")(6:signal");
	hull_msg_number(&m, WIFSIGNALED(status) ? WTERMSIG(status) : 0);
	hull_msg_raw(&m, "))");
	send_msg(&m);
	kill(-1, SIGKILL);
	send_simple("(3:bye");
	power_off();
}

int main(void)
{
	sigset_t       mask;
	int            children;
	int            listener;
	hull_wire_status wire_status;

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
		if (fd < 0 || hull_wire_open(&host, fd, HULL_WIRE_FROM_HOST) != 0)
			power_off();
		connected = 1;
	}

	for (;;) {
		struct pollfd fds[4];
		int           timeout;

		/* The host gone is (on_disconnect stop), with nobody to tell. */
		if (!connected && workload <= 0)
			power_off();
		if (!connected && !stopping)
			begin_stop(0);
		if (reaped && out_fd < 0 && err_fd < 0)
			finish();
		fds[0] = (struct pollfd){ connected ? host.fd : -1, POLLIN, 0 };
		fds[1] = (struct pollfd){ out_fd, POLLIN, 0 };
		fds[2] = (struct pollfd){ err_fd, POLLIN, 0 };
		fds[3] = (struct pollfd){ children, POLLIN, 0 };
		timeout = until_kill();
		if (poll(fds, 4u, timeout) < 0 && errno != EINTR)
			power_off();
		if (fds[1].revents != 0)
			drain(&out_fd, 1);
		if (fds[2].revents != 0)
			drain(&err_fd, 2);
		if (fds[3].revents != 0) {
			struct signalfd_siginfo info;
			pid_t                   pid;
			int                     wstatus;

			while (read(children, &info, sizeof info) > 0)
				;
			while ((pid = waitpid(-1, &wstatus, WNOHANG)) > 0) {
				if (pid == workload) {
					reaped = 1;
					status = wstatus;
				}
			}
			/* An orphan of the workload's can hold its pipes open;
			 * what is there is read, and they are not waited on. */
			if (reaped) {
				if (out_fd >= 0)
					fcntl(out_fd, F_SETFL, O_NONBLOCK);
				if (err_fd >= 0)
					fcntl(err_fd, F_SETFL, O_NONBLOCK);
				while (out_fd >= 0 && drain(&out_fd, 1))
					;
				while (err_fd >= 0 && drain(&err_fd, 2))
					;
			}
		}
		if (connected && fds[0].revents != 0) {
			hull_sexpr_doc body;

			wire_status = hull_wire_fill(&host);
			while (wire_status == HULL_WIRE_OK &&
			       (wire_status = hull_wire_next(&host, &body)) == HULL_WIRE_OK)
				handle(&body);
			if (wire_status == HULL_WIRE_CLOSED || wire_status == HULL_WIRE_BROKEN)
				connected = 0;
		}
	}
}
