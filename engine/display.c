/*
 * display.c -- a screen's local display linked by its address; see
 * display.h. A link left by a core that died names a directory gone,
 * which a client cannot connect to, and the next machine at that address
 * replaces it.
 */

#define _DEFAULT_SOURCE
#include "display.h"

#include <dirent.h>
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
