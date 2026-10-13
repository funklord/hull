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
 *   nnp         whether no_new_privs is set
 *   session     whether it leads a session of its own
 *   exit N      exit with N           die         end by SIGTERM
 *   wait        wait for a signal, for 20 seconds at most, so that a
 *               test whose stop fails leaves no probe behind
 */

#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <unistd.h>

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
		} else if (strcmp(step, "nnp") == 0) {
			printf("nnp=%d\n", prctl(PR_GET_NO_NEW_PRIVS, 0, 0, 0, 0));
		} else if (strcmp(step, "exit") == 0) {
			return atoi(arg);
		} else if (strcmp(step, "die") == 0) {
			raise(SIGTERM);
		} else if (strcmp(step, "wait") == 0) {
			printf("waiting\n");
			alarm(20);
			pause();
		}
	}
	return 0;
}
