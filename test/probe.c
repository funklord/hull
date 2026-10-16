/*
 * probe.c -- the one program in the reference engine's test rootfs.
 *
 * Linked static, since the rootfs holds nothing else. Its arguments are
 * steps, taken in order, each printing what the machine looks like from
 * inside:
 *
 *   say TEXT    TEXT on stdout        err TEXT    TEXT on stderr
 *   env NAME    NAME's value          cwd         the working directory
 *   host        the hostname          uid         the uid
 *   write PATH  whether PATH can be created
 *   read PATH   PATH's first line
 *   stdin       the first line of its stdin
 *   nnp         whether no_new_privs is set
 *   session     whether it leads a session of its own
 *   pid         its pid
 *   procs       how many processes /proc lists, or -1 without one
 *   orphan      leave a grandchild to the machine's init, which ends a
 *               second later, and wait two for the init to reap it
 *   exit N      exit with N           die         end by SIGTERM
 *   wait        wait for a signal, for 20 seconds at most, so that a
 *               test whose stop fails leaves no probe behind
 *   trap        catch SIGHUP, SIGINT or SIGTERM, for 20 seconds at most,
 *               and say which; the probe goes on
 *   tty         whether stdin is a terminal, its size, and whether it is
 *               this process's controlling terminal
 *   winch       wait for the terminal to be resized, as trap waits, and
 *               say its size
 */

#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/ioctl.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

static volatile sig_atomic_t caught;

static void catch(int signal)
{
	caught = signal;
}

/* Wait for one of `signals`, for 20 seconds at most; the one caught, or 0. */
static int await(const int *signals, unsigned n)
{
	struct sigaction action;

	memset(&action, 0, sizeof action);
	action.sa_handler = catch;
	caught = 0;
	for (unsigned i = 0u; i < n; i++)
		sigaction(signals[i], &action, NULL);
	printf("ready\n");
	alarm(20);
	while (caught == 0)
		pause();
	alarm(0);
	return caught;
}

static void print_size(void)
{
	struct winsize size;

	if (ioctl(0, TIOCGWINSZ, &size) == 0)
		printf("size=%ux%u\n", (unsigned)size.ws_col, (unsigned)size.ws_row);
	else
		printf("size=%s\n", strerror(errno));
}

int main(int argc, char **argv)
{
	char text[256];

	setvbuf(stdout, NULL, _IONBF, 0);
	for (int i = 1; i < argc; i++) {
		const char *step = argv[i];
		const char *arg  = i + 1 < argc ? argv[i + 1] : "";

		if (strcmp(step, "say") == 0) {
			printf("%s\n", arg);
			i++;
		} else if (strcmp(step, "err") == 0) {
			fprintf(stderr, "%s\n", arg);
			i++;
		} else if (strcmp(step, "env") == 0) {
			printf("%s=%s\n", arg, getenv(arg) != NULL ? getenv(arg) : "(unset)");
			i++;
		} else if (strcmp(step, "cwd") == 0) {
			printf("cwd=%s\n", getcwd(text, sizeof text) != NULL ? text : "(unknown)");
		} else if (strcmp(step, "host") == 0) {
			printf("host=%s\n", gethostname(text, sizeof text) == 0 ? text : "(unknown)");
		} else if (strcmp(step, "uid") == 0) {
			printf("uid=%u\n", (unsigned)getuid());
		} else if (strcmp(step, "write") == 0) {
			int fd = open(arg, O_WRONLY | O_CREAT | O_TRUNC, 0644);

			printf("write=%s\n", fd >= 0 ? "ok" : strerror(errno));
			if (fd >= 0)
				close(fd);
			i++;
		} else if (strcmp(step, "session") == 0) {
			printf("leader=%d\n", getsid(0) == getpid());
		} else if (strcmp(step, "read") == 0) {
			FILE *file = fopen(arg, "r");

			if (file == NULL || fgets(text, sizeof text, file) == NULL)
				printf("read=%s\n", strerror(errno));
			else
				printf("read=%s", text);
			if (file != NULL)
				fclose(file);
			i++;
		} else if (strcmp(step, "stdin") == 0) {
			ssize_t n = read(0, text, sizeof text - 1u);

			text[n > 0 ? n : 0] = '\0';
			printf("stdin=%s", n > 0 ? text : "(none)\n");
		} else if (strcmp(step, "pid") == 0) {
			printf("pid=%d\n", (int)getpid());
		} else if (strcmp(step, "procs") == 0) {
			DIR           *proc  = opendir("/proc");
			struct dirent *entry;
			int            count = 0;

			while (proc != NULL && (entry = readdir(proc)) != NULL)
				count += entry->d_name[0] >= '1' && entry->d_name[0] <= '9';
			printf("procs=%d\n", proc != NULL ? count : -1);
			if (proc != NULL)
				closedir(proc);
		} else if (strcmp(step, "orphan") == 0) {
			pid_t child = fork();

			if (child == 0) {
				if (fork() == 0) {
					sleep(1);
					_exit(0);
				}
				_exit(0);
			}
			waitpid(child, NULL, 0);
			sleep(2);
		} else if (strcmp(step, "nnp") == 0) {
			printf("nnp=%d\n", prctl(PR_GET_NO_NEW_PRIVS, 0, 0, 0, 0));
		} else if (strcmp(step, "exit") == 0) {
			return atoi(arg);
		} else if (strcmp(step, "die") == 0) {
			raise(SIGTERM);
		} else if (strcmp(step, "trap") == 0) {
			static const int signals[] = { SIGHUP, SIGINT, SIGTERM };

			printf("signal=%d\n", await(signals, 3u));
		} else if (strcmp(step, "tty") == 0) {
			printf("tty=%s\n", isatty(0) ? "yes" : "no");
			print_size();
			printf("controlling=%s\n", tcgetpgrp(0) == getpgrp() ? "yes" : "no");
		} else if (strcmp(step, "winch") == 0) {
			static const int signals[] = { SIGWINCH };

			await(signals, 1u);
			print_size();
		} else if (strcmp(step, "wait") == 0) {
			printf("waiting\n");
			alarm(20);
			pause();
		}
	}
	return 0;
}
