/*
 * display.c -- a screen's local display linked by its address; see
 * display.h. A link left by a core that died names a directory gone,
 * which a client cannot connect to, and the next machine at that address
 * replaces it.
 */

#define _DEFAULT_SOURCE
#include "display.h"

#include <dirent.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

int hull_display_fits(const char *monitor)
{
	return strlen(monitor) < sizeof ((struct sockaddr_un *)0)->sun_path;
}

void hull_display_link(const char *monitor, const char *address, char *link, size_t cap)
{
	const char *runtime = getenv("XDG_RUNTIME_DIR");
	char        temporary[4400];

	link[0] = '\0';
	if (runtime == NULL || runtime[0] != '/' || address[0] == '\0' || strchr(address, '/') != NULL)
		return;
	snprintf(temporary, sizeof temporary, "%s/hull/display", runtime);
	mkdir(temporary, 0700);
	if ((size_t)snprintf(link, cap, "%s/hull/display/%s", runtime, address) >= cap) {
		link[0] = '\0';
		return;
	}
	snprintf(temporary, sizeof temporary, "%s.%ld", link, (long)getpid());
	unlink(temporary);
	if (symlink(monitor, temporary) != 0 || rename(temporary, link) != 0) {
		unlink(temporary);
		link[0] = '\0';
	}
}

const char *hull_audio_backend(void)
{
	const char *runtime = getenv("XDG_RUNTIME_DIR");
	char        path[4400];
	struct stat st;
	DIR        *dir;
	struct dirent *entry;
	const char *found = "none";

	if (runtime != NULL && runtime[0] == '/') {
		snprintf(path, sizeof path, "%s/pipewire-0", runtime);
		if (stat(path, &st) == 0 && S_ISSOCK(st.st_mode))
			return "pipewire";
		snprintf(path, sizeof path, "%s/pulse/native", runtime);
		if (stat(path, &st) == 0 && S_ISSOCK(st.st_mode))
			return "pa";
	}
	if ((dir = opendir("/dev/snd")) == NULL)
		return found;
	while ((entry = readdir(dir)) != NULL) {
		size_t length = strlen(entry->d_name);

		snprintf(path, sizeof path, "/dev/snd/%s", entry->d_name);
		if (strncmp(entry->d_name, "pcmC", 4u) == 0 && length > 0u &&
		    entry->d_name[length - 1u] == 'p' && access(path, W_OK) == 0) {
			found = "alsa";
			break;
		}
	}
	closedir(dir);
	return found;
}

void hull_display_adopt(const char *monitor, const char *address, char *link, size_t cap)
{
	const char *runtime = getenv("XDG_RUNTIME_DIR");
	char        target[4400];
	ssize_t     length;

	link[0] = '\0';
	if (runtime == NULL || runtime[0] != '/' || address[0] == '\0' || strchr(address, '/') != NULL ||
	    (size_t)snprintf(link, cap, "%s/hull/display/%s", runtime, address) >= cap) {
		link[0] = '\0';
		return;
	}
	length = readlink(link, target, sizeof target - 1u);
	if (length <= 0 || (target[length] = '\0', strcmp(target, monitor) != 0))
		link[0] = '\0';
}

void hull_display_unlink(char *link, const char *monitor)
{
	char    target[4400];
	ssize_t length;

	if (link[0] == '\0')
		return;
	length = readlink(link, target, sizeof target - 1u);
	if (length > 0 && (target[length] = '\0', strcmp(target, monitor) == 0))
		unlink(link);
	link[0] = '\0';
}

/*
 * Every process left in the session qemu led, killed: what qemu forked and
 * did not end with it. virglrenderer's Venus render server is one, a fork
 * of qemu in a process group of its own, which went on spinning under init
 * after its machine had ended, measured 2026-11-13 on an RTX 3060 with
 * NVIDIA's 615 driver. Called while qemu is a zombie not yet reaped, so
 * its pid, the session's id, cannot have been given to another.
 */
void hull_display_end_session(pid_t leader)
{
	DIR           *proc = opendir("/proc");
	struct dirent *entry;

	if (proc == NULL)
		return;
	while ((entry = readdir(proc)) != NULL) {
		char  path[300];
		char  stat[1024];
		char *close_paren;
		long  pid = strtol(entry->d_name, NULL, 10);
		int   fd;
		ssize_t got;
		long  ppid, pgrp, session;
		char  state;

		if (pid <= 0 || pid == leader)
			continue;
		snprintf(path, sizeof path, "/proc/%ld/stat", pid);
		if ((fd = open(path, O_RDONLY | O_CLOEXEC)) < 0)
			continue;
		got = read(fd, stat, sizeof stat - 1u);
		close(fd);
		if (got <= 0)
			continue;
		stat[got] = '\0';
		/* After the command's name, which may hold anything, its ')'. */
		if ((close_paren = strrchr(stat, ')')) == NULL ||
		    sscanf(close_paren + 1, " %c %ld %ld %ld", &state, &ppid, &pgrp, &session) != 4)
			continue;
		if (session == (long)leader)
			kill((pid_t)pid, SIGKILL);
	}
	closedir(proc);
}
