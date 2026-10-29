/*
 * hostdev.c -- the host's devices a machine is given (hostdev.h).
 */

#define _GNU_SOURCE

#include <dirent.h>
#include <errno.h>
#include <fnmatch.h>
#include <grp.h>
#include <limits.h>
#include <pthread.h>
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

/* The driver `dir/name` is bound to, the last part of its driver link,
 * or "" for none. */
static void driver_of(const char *dir, const char *name, char *out, size_t cap)
{
	char    link[600];
	char    target[600];
	ssize_t n;

	out[0] = '\0';
	snprintf(link, sizeof link, "%s/%s/driver", dir, name);
	n = readlink(link, target, sizeof target - 1u);
	if (n > 0) {
		const char *base;

		target[n] = '\0';
		base = strrchr(target, '/');
		base = base != NULL ? base + 1 : target;
		if (strlen(base) < cap)
			memcpy(out, base, strlen(base) + 1u);
	}
}

const char *hull_hostdev_pci(const char *sysfs, const char *groups, const char *address,
                             hull_hostdev *found, char *why, size_t cap)
{
	char           path[600];
	char           target[600];
	char           driver[64];
	char           members[1300];
	const char    *group;
	ssize_t        n;
	DIR           *dir;
	struct dirent *entry;
	struct stat    st;

	memset(found, 0, sizeof *found);
	snprintf(path, sizeof path, "%s/%s", sysfs, address);
	if (access(path, F_OK) != 0) {
		snprintf(why, cap, "no PCI device %s is on the host", address);
		return why;
	}
	snprintf(path, sizeof path, "%s/%s/iommu_group", sysfs, address);
	n = readlink(path, target, sizeof target - 1u);
	if (n <= 0) {
		snprintf(why, cap, "PCI device %s is in no IOMMU group: VFIO wants the IOMMU on, "
		         "intel_iommu=on or amd_iommu=on", address);
		return why;
	}
	target[n] = '\0';
	group = strrchr(target, '/') != NULL ? strrchr(target, '/') + 1 : target;
	driver_of(sysfs, address, driver, sizeof driver);
	if (strcmp(driver, "vfio-pci") != 0) {
		snprintf(why, cap, "PCI device %s is bound to %s, not vfio-pci, which an administrator "
		         "binds it to once, as root", address, driver[0] != '\0' ? driver : "no driver");
		return why;
	}
	/* The group is given whole: each other member vfio-pci's or no
	 * guest's way to the host. */
	snprintf(members, sizeof members, "%s/%s/devices", groups, group);
	if ((dir = opendir(members)) == NULL) {
		snprintf(why, cap, "%s cannot be read, which lists IOMMU group %s", members, group);
		return why;
	}
	while ((entry = readdir(dir)) != NULL) {
		if (entry->d_name[0] == '.' || strcmp(entry->d_name, address) == 0)
			continue;
		driver_of(members, entry->d_name, driver, sizeof driver);
		if (driver[0] != '\0' && strcmp(driver, "vfio-pci") != 0 &&
		    strcmp(driver, "pci-stub") != 0 && strcmp(driver, "pcieport") != 0) {
			snprintf(why, cap, "PCI device %s shares IOMMU group %s with %s, bound to %s: the "
			         "group is given whole, so it wants vfio-pci too", address, group,
			         entry->d_name, driver);
			closedir(dir);
			return why;
		}
	}
	closedir(dir);
	if (strlen(group) > 16u) {
		snprintf(why, cap, "PCI device %s's IOMMU group is not a number", address);
		return why;
	}
	snprintf(path, sizeof path, "/dev/vfio/%.16s", group);
	if (stat(path, &st) != 0) {
		snprintf(why, cap, "PCI device %s is vfio-pci's, and %s is not there", address, path);
		return why;
	}
	if (usable(path, &st, why, cap) != NULL)
		return why;
	snprintf(found->node, sizeof found->node, "/dev/vfio/%.16s", group);
	snprintf(found->port, sizeof found->port, "%.12s", address);
	return NULL;
}

const char *hull_hostdev_find(hull_device *device, char *why, size_t cap)
{
	if (device->kind == HULL_DEVICE_PCI)
		return hull_hostdev_pci(HULL_SYSFS_PCI, HULL_SYSFS_GROUPS, device->path, &device->host,
		                        why, cap);
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

/* --- held ---------------------------------------------------------------- */

#define MAX_CLAIMS 64u

static struct {
	const void *owner;
	char        node[256];
} claims[MAX_CLAIMS];
static pthread_mutex_t claims_lock = PTHREAD_MUTEX_INITIALIZER;

static int exclusive(const hull_device *device)
{
	return device->kind != HULL_DEVICE_NODE;
}

const char *hull_hostdev_claim(const void *owner, const hull_device *devices, unsigned count,
                               char *why, size_t cap)
{
	unsigned wanted = 0u;
	unsigned free_slots = 0u;

	pthread_mutex_lock(&claims_lock);
	for (unsigned i = 0u; i < count; i++) {
		if (!exclusive(&devices[i]))
			continue;
		for (unsigned c = 0u; c < MAX_CLAIMS; c++) {
			if (claims[c].owner == NULL || strcmp(claims[c].node, devices[i].host.node) != 0)
				continue;
			if (claims[c].owner != owner) {
				pthread_mutex_unlock(&claims_lock);
				snprintf(why, cap,
				         "%s is another running machine's: a USB device, serial or parallel "
				         "port is one machine's at a time",
				         devices[i].host.node);
				return why;
			}
		}
		wanted++;
	}
	for (unsigned c = 0u; c < MAX_CLAIMS; c++)
		free_slots += claims[c].owner == NULL;
	if (wanted > free_slots) {
		pthread_mutex_unlock(&claims_lock);
		snprintf(why, cap, "the core holds %u devices for its machines already", MAX_CLAIMS);
		return why;
	}
	for (unsigned i = 0u; i < count; i++) {
		int held = 0;

		if (!exclusive(&devices[i]))
			continue;
		for (unsigned c = 0u; c < MAX_CLAIMS && !held; c++)
			held = claims[c].owner == owner && strcmp(claims[c].node, devices[i].host.node) == 0;
		for (unsigned c = 0u; c < MAX_CLAIMS && !held; c++)
			if (claims[c].owner == NULL) {
				claims[c].owner = owner;
				memcpy(claims[c].node, devices[i].host.node, sizeof claims[c].node);
				held = 1;
			}
	}
	pthread_mutex_unlock(&claims_lock);
	return NULL;
}

void hull_hostdev_unclaim(const void *owner)
{
	pthread_mutex_lock(&claims_lock);
	for (unsigned c = 0u; c < MAX_CLAIMS; c++)
		if (claims[c].owner == owner)
			claims[c].owner = NULL;
	pthread_mutex_unlock(&claims_lock);
}
