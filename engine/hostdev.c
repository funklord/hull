/*
 * hostdev.c -- the host's devices a machine is given (hostdev.h).
 */

#define _GNU_SOURCE

#include <dirent.h>
#include <errno.h>
#include <fnmatch.h>
#include <grp.h>
#include <limits.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include "hostdev.h"

/* One line of `dir/name/file`, its newline gone: 1, or 0 where unread. */
static int read_line(const char *dir, const char *name, const char *file, char *out, size_t cap)
{
	char  path[512];
	FILE *in;
	int   got;

	snprintf(path, sizeof path, "%s/%s/%s", dir, name, file);
	in = fopen(path, "re");
	if (in == NULL)
		return 0;
	got = fgets(out, (int)cap, in) != NULL;
	fclose(in);
	if (got)
		out[strcspn(out, "\n")] = '\0';
	return got;
}

/* `node` the user's to read and write, or why not, naming its owner,
 * group and mode, which is what a udev rule changes. */
static const char *usable(const char *node, const struct stat *st, char *why, size_t cap)
{
	struct passwd *owner;
	struct group  *group;
	char           owner_name[32];
	char           group_name[32];

	if (access(node, R_OK | W_OK) == 0)
		return NULL;
	owner = getpwuid(st->st_uid);
	snprintf(owner_name, sizeof owner_name, "%s", owner != NULL ? owner->pw_name : "?");
	group = getgrgid(st->st_gid);
	snprintf(group_name, sizeof group_name, "%s", group != NULL ? group->gr_name : "?");
	snprintf(why, cap,
	         "%s is %s:%s %03o, which the user may not read and write: a udev rule, or the "
	         "group, gives it to them",
	         node, owner_name, group_name, (unsigned)(st->st_mode & 0777u));
	return why;
}

const char *hull_hostdev_node(const char *path, hull_hostdev *found, char *why, size_t cap)
{
	char        real[PATH_MAX];
	struct stat st;

	memset(found, 0, sizeof *found);
	if (realpath(path, real) == NULL) {
		snprintf(why, cap, "%s: %s", path, strerror(errno));
		return why;
	}
	if (strncmp(real, "/dev/", 5u) != 0 || stat(real, &st) != 0 ||
	    !(S_ISCHR(st.st_mode) || S_ISBLK(st.st_mode))) {
		snprintf(why, cap, "%s is not a device under /dev", path);
		return why;
	}
	if (strlen(real) >= sizeof found->node || strlen(path) >= sizeof found->named) {
		snprintf(why, cap, "%s: the path is too long", path);
		return why;
	}
	if (usable(real, &st, why, cap) != NULL)
		return why;
	memcpy(found->node, real, strlen(real) + 1u);
	if (strcmp(real, path) != 0)
		memcpy(found->named, path, strlen(path) + 1u);
	return NULL;
}

/* A device sysfs lists, not an interface of one ("1-4:1.0") nor a root
 * hub ("usb1"). */
static int is_device(const char *name)
{
	return name[0] >= '0' && name[0] <= '9' && strchr(name, ':') == NULL;
}

int hull_hostdev_usb_isochronous(const char *sysfs, const char *port)
{
	DIR           *dir = opendir(sysfs);
	struct dirent *entry;
	size_t         stem = strlen(port);
	int            found = 0;

	while (dir != NULL && !found && (entry = readdir(dir)) != NULL) {
		char           path[512];
		DIR           *endpoints;
		struct dirent *endpoint;

		if (strncmp(entry->d_name, port, stem) != 0 || entry->d_name[stem] != ':')
			continue;
		snprintf(path, sizeof path, "%s/%s", sysfs, entry->d_name);
		endpoints = opendir(path);
		while (endpoints != NULL && !found && (endpoint = readdir(endpoints)) != NULL) {
			char type[32];

			found = strncmp(endpoint->d_name, "ep_", 3u) == 0 &&
			        read_line(path, endpoint->d_name, "type", type, sizeof type) &&
			        strcmp(type, "Isoc") == 0;
		}
		if (endpoints != NULL)
			closedir(endpoints);
	}
	if (dir != NULL)
		closedir(dir);
	return found;
}

const char *hull_hostdev_usb(const char *sysfs, const char *vendor, const char *product,
                             const char *serial, const char *port, hull_hostdev *found,
                             char *why, size_t cap)
{
	DIR           *dir = opendir(sysfs);
	struct dirent *entry;
	unsigned       matches = 0u;
	char           which[64];
	char           chosen[sizeof found->port] = "";
	unsigned       bus = 0u;
	unsigned       address = 0u;

	memset(found, 0, sizeof *found);
	if (vendor != NULL)
		snprintf(which, sizeof which, "%s:%s%s%s", vendor, product, serial != NULL ? " " : "",
		         serial != NULL ? serial : "");
	else
		snprintf(which, sizeof which, "on port %s", port);
	if (dir == NULL) {
		snprintf(why, cap, "%s cannot be read, which lists the host's USB devices", sysfs);
		return why;
	}
	while ((entry = readdir(dir)) != NULL) {
		char text[128];
		char busnum[16];
		char devnum[16];

		if (!is_device(entry->d_name) || strlen(entry->d_name) >= sizeof chosen)
			continue;
		if (port != NULL && strcmp(entry->d_name, port) != 0)
			continue;
		if (vendor != NULL &&
		    (!read_line(sysfs, entry->d_name, "idVendor", text, sizeof text) ||
		     strcasecmp(text, vendor) != 0 ||
		     !read_line(sysfs, entry->d_name, "idProduct", text, sizeof text) ||
		     strcasecmp(text, product) != 0))
			continue;
		if (serial != NULL && (!read_line(sysfs, entry->d_name, "serial", text, sizeof text) ||
		                       strcmp(text, serial) != 0))
			continue;
		if (!read_line(sysfs, entry->d_name, "busnum", busnum, sizeof busnum) ||
		    !read_line(sysfs, entry->d_name, "devnum", devnum, sizeof devnum))
			continue;
		if (matches++ == 0u) {
			memcpy(chosen, entry->d_name, strlen(entry->d_name) + 1u);
			bus = (unsigned)strtoul(busnum, NULL, 10);
			address = (unsigned)strtoul(devnum, NULL, 10);
		}
	}
	closedir(dir);
	if (matches == 0u) {
		snprintf(why, cap, "no USB device %s is plugged in", which);
		return why;
	}
	if (matches > 1u) {
		snprintf(why, cap, "%u USB devices are %s: its serial or its port tells which", matches,
		         which);
		return why;
	}
	{
		char        node[64];
		struct stat st;

		snprintf(node, sizeof node, "/dev/bus/usb/%03u/%03u", bus, address);
		if (stat(node, &st) != 0) {
			snprintf(why, cap, "USB device %s is on port %s, and %s is not there", which, chosen,
			         node);
			return why;
		}
		if (usable(node, &st, why, cap) != NULL)
			return why;
		snprintf(found->node, sizeof found->node, "%s", node);
	}
	memcpy(found->port, chosen, sizeof found->port);
	found->bus = bus;
	found->isochronous = hull_hostdev_usb_isochronous(sysfs, chosen);
	{
		char id_vendor[8] = "";
		char id_product[8] = "";

		read_line(sysfs, chosen, "idVendor", id_vendor, sizeof id_vendor);
		read_line(sysfs, chosen, "idProduct", id_product, sizeof id_product);
		snprintf(found->ids, sizeof found->ids, "%.4s:%.4s", id_vendor, id_product);
	}
	return NULL;
}

const char *hull_hostdev_find(hull_device *device, char *why, size_t cap)
{
	if (device->kind != HULL_DEVICE_USB)
		return hull_hostdev_node(device->path, &device->host, why, cap);
	return hull_hostdev_usb(HULL_SYSFS_USB, device->vendor[0] != '\0' ? device->vendor : NULL,
	                        device->vendor[0] != '\0' ? device->product : NULL,
	                        device->serial[0] != '\0' ? device->serial : NULL,
	                        device->port[0] != '\0' ? device->port : NULL, &device->host, why,
	                        cap);
}

int hull_hostdev_usb_drivers(const char *sysfs, const char *port, const char *aliases,
                             char *names, size_t name_cap, unsigned count)
{
	DIR           *dir;
	struct dirent *entry;
	char           wanted[16][256];
	unsigned       interfaces = 0u;
	unsigned       found = 0u;
	size_t         stem = strlen(port);
	FILE          *in = fopen(aliases, "re");
	char           line[1024];

	if (in == NULL)
		return -1;
	/* Each interface's modalias, "1-4.2:1.0/modalias". */
	if ((dir = opendir(sysfs)) != NULL) {
		while ((entry = readdir(dir)) != NULL && interfaces < 16u)
			if (strncmp(entry->d_name, port, stem) == 0 && entry->d_name[stem] == ':' &&
			    read_line(sysfs, entry->d_name, "modalias", wanted[interfaces],
			              sizeof wanted[interfaces]))
				interfaces++;
		closedir(dir);
	}
	/* `alias PATTERN MODULE`, matched as modprobe does, by fnmatch. */
	while (fgets(line, sizeof line, in) != NULL) {
		char *pattern;
		char *module;
		int   match = 0;

		if (strncmp(line, "alias usb:", 10u) != 0)
			continue;
		pattern = line + 6;
		module = strchr(pattern, ' ');
		if (module == NULL)
			continue;
		*module++ = '\0';
		module[strcspn(module, "\n")] = '\0';
		for (unsigned i = 0u; i < interfaces && !match; i++)
			match = fnmatch(pattern, wanted[i], 0) == 0;
		for (unsigned i = 0u; i < found && match; i++)
			match = strcmp(names + i * name_cap, module) != 0;
		if (match && found < count && strlen(module) < name_cap)
			snprintf(names + (found++) * name_cap, name_cap, "%s", module);
	}
	fclose(in);
	return (int)found;
}
