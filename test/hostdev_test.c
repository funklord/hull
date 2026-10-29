/*
 * hostdev_test.c -- engine/hostdev.c: a USB device found among sysfs's
 * by its ids, its serial and its port, over a tree of the test's own
 * beneath $BUILD_DIR, as sysfs lays one out; its interfaces' drivers by a
 * modules.alias of its own; and a node followed to the host's /dev.
 */

#define _GNU_SOURCE

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "hostdev.h"

static unsigned cases;
static unsigned failures;

static void fail(const char *what, const char *detail)
{
	failures++;
	printf("FAIL %s: %s\n", what, detail);
}

static void put(const char *root, const char *device, const char *file, const char *text)
{
	char  path[512];
	FILE *out;

	snprintf(path, sizeof path, "%s/%s", root, device);
	mkdir(path, 0755);
	snprintf(path, sizeof path, "%s/%s/%s", root, device, file);
	out = fopen(path, "w");
	if (out == NULL) {
		fail("writing the tree", path);
		return;
	}
	fprintf(out, "%s\n", text);
	fclose(out);
}

/* A device of sysfs's, on bus 250, so that no node of the host's is it. */
static void device(const char *root, const char *port, const char *vendor, const char *product,
                   const char *serial, const char *devnum)
{
	put(root, port, "idVendor", vendor);
	put(root, port, "idProduct", product);
	if (serial != NULL)
		put(root, port, "serial", serial);
	put(root, port, "busnum", "250");
	put(root, port, "devnum", devnum);
}

/* hull_hostdev_usb refusing, with `want` in what it says. */
static void usb_says(const char *what, const char *root, const char *vendor, const char *product,
                     const char *serial, const char *port, const char *want)
{
	hull_hostdev found;
	char         why[512] = "";

	cases++;
	if (hull_hostdev_usb(root, vendor, product, serial, port, &found, why, sizeof why) == NULL)
		fail(what, "found a device");
	else if (strstr(why, want) == NULL)
		fail(what, why);
}

int main(void)
{
	const char  *build = getenv("BUILD_DIR");
	char         root[512];
	char         aliases[600];
	char         link[600];
	char         plain[600];
	hull_hostdev found;
	char         why[512];

	snprintf(root, sizeof root, "%s/test/hostdev", build != NULL ? build : ".");
	{
		char command[1100];

		snprintf(command, sizeof command, "rm -rf '%s' && mkdir -p '%s'", root, root);
		if (system(command) != 0) {
			printf("hostdev: the tree cannot be made at %s\n", root);
			return 1;
		}
	}
	/* Absolute, since the fixture's links name their targets by it, and a
	 * relative target is read from the link's own directory: under make's
	 * BUILD_DIR=build every group member's driver went unread. */
	{
		char *whole = realpath(root, NULL);

		if (whole == NULL || strlen(whole) >= sizeof root) {
			printf("hostdev: %s cannot be made absolute\n", root);
			free(whole);
			return 1;
		}
		snprintf(root, sizeof root, "%s", whole);
		free(whole);
	}
	/* Two of one FTDI part, told apart by their serials, a third part on
	 * a hub's port, and what sysfs lists beside devices: a root hub and an
	 * interface, carrying the same ids, which are not devices. */
	device(root, "1-2", "0403", "6001", "A10K", "11");
	device(root, "1-3", "0403", "6001", "B20M", "12");
	device(root, "1-4.2", "0483", "3748", NULL, "13");
	device(root, "usb1", "0483", "3748", NULL, "1");
	put(root, "1-4.2:1.0", "idVendor", "0483");
	put(root, "1-4.2:1.0", "modalias", "usb:v0483p3748d0100dc00dsc00dp00icFFiscFFipFFin00");
	put(root, "1-4.2:1.1", "modalias", "usb:v0483p3748d0100dc00dsc00dp00ic02isc02ip01in01");

	usb_says("none plugged in", root, "1234", "5678", NULL, NULL,
	         "no USB device 1234:5678 is plugged in");
	usb_says("two of one part", root, "0403", "6001", NULL, NULL,
	         "2 USB devices are 0403:6001: its serial or its port tells which");
	usb_says("one by its serial", root, "0403", "6001", "B20M", NULL,
	         "is on port 1-3, and /dev/bus/usb/250/012 is not there");
	usb_says("a serial no device has", root, "0403", "6001", "C30", NULL,
	         "no USB device 0403:6001 C30 is plugged in");
	usb_says("ids in upper case", root, "0483", "3748", NULL, NULL,
	         "is on port 1-4.2, and /dev/bus/usb/250/013 is not there");
	usb_says("one by its port", root, NULL, NULL, NULL, "1-2",
	         "is on port 1-2, and /dev/bus/usb/250/011 is not there");
	usb_says("its port and ids that are not its", root, "0483", "3748", NULL, "1-2",
	         "no USB device 0483:3748 is plugged in");
	usb_says("a port with nothing in it", root, NULL, NULL, NULL, "1-9",
	         "no USB device on port 1-9 is plugged in");
	usb_says("no sysfs", "/nonexistent", "0403", "6001", NULL, NULL,
	         "/nonexistent cannot be read");
	/* Bus 1's root hub, a node of the host's that only root may write. */
	if (getuid() != 0 && access("/dev/bus/usb/001/001", F_OK) == 0 &&
	    access("/dev/bus/usb/001/001", W_OK) != 0) {
		device(root, "1-7", "1d6b", "0002", NULL, "1");
		put(root, "1-7", "busnum", "1");
		usb_says("a node the user may not write", root, "1d6b", "0002", NULL, "1-7",
		         "/dev/bus/usb/001/001 is root:");
	}

	/* An isochronous endpoint, a webcam's, and a bulk one, a probe's. */
	{
		char path[600];

		snprintf(path, sizeof path, "%s/1-4.2:1.0/ep_81", root);
		mkdir(path, 0755);
		put(root, "1-4.2:1.0/ep_81", "type", "Bulk");
		cases++;
		if (hull_hostdev_usb_isochronous(root, "1-4.2"))
			fail("a bulk endpoint taken for isochronous", "1-4.2");
		snprintf(path, sizeof path, "%s/1-4.2:1.1/ep_82", root);
		mkdir(path, 0755);
		put(root, "1-4.2:1.1/ep_82", "type", "Isoc");
		cases++;
		if (!hull_hostdev_usb_isochronous(root, "1-4.2"))
			fail("an isochronous endpoint missed", "1-4.2");
		cases++;
		if (hull_hostdev_usb_isochronous(root, "1-4"))
			fail("another port's endpoint taken as this one's", "1-4");
	}

	/* Its interfaces' drivers, by modules.alias as modprobe matches it. */
	snprintf(aliases, sizeof aliases, "%s/modules.alias", root);
	{
		FILE *out = fopen(aliases, "w");
		char  names[4][64];
		int   n;

		if (out == NULL) {
			fail("modules.alias", aliases);
		} else {
			fputs("alias usb:v*p*d*dc*dsc*dp*ic02isc02ip01in* cdc_acm\n"
			      "alias usb:v0483p3748d*dc*dsc*dp*ic*isc*ip*in* stlink\n"
			      "alias usb:v0403p6001d*dc*dsc*dp*ic*isc*ip*in* ftdi_sio\n"
			      "alias usb:v*p*d*dc*dsc*dp*ic02isc02ip01in* cdc_acm\n"
			      "alias pci:v00008086d*sv*sd*bc*sc*i* not_usb\n", out);
			fclose(out);
		}
		cases++;
		n = hull_hostdev_usb_drivers(root, "1-4.2", aliases, &names[0][0], sizeof names[0], 4u);
		if (n != 2 || strcmp(names[0], "cdc_acm") != 0 || strcmp(names[1], "stlink") != 0) {
			snprintf(why, sizeof why, "%d: %s %s", n, n > 0 ? names[0] : "",
			         n > 1 ? names[1] : "");
			fail("drivers of each interface, each once", why);
		}
		cases++;
		if (hull_hostdev_usb_drivers(root, "1-4", aliases, &names[0][0], sizeof names[0], 4u) != 0)
			fail("drivers of a port that is a prefix of another's", "some");
		cases++;
		if (hull_hostdev_usb_drivers(root, "1-4.2", "/nonexistent", &names[0][0],
		                             sizeof names[0], 4u) != -1)
			fail("drivers with no modules.alias", "not -1");
	}

	/* A node, and a link to one, as /dev/serial/by-id is. */
	if (access("/dev/net/tun", R_OK | W_OK) == 0) {
		cases++;
		if (hull_hostdev_node("/dev/net/tun", &found, why, sizeof why) != NULL)
			fail("a node", why);
		else if (strcmp(found.node, "/dev/net/tun") != 0 || found.named[0] != '\0')
			fail("a node", found.node);
		snprintf(link, sizeof link, "%s/tun", root);
		cases++;
		if (symlink("/dev/net/tun", link) != 0)
			fail("a link", strerror(errno));
		else if (hull_hostdev_node(link, &found, why, sizeof why) != NULL)
			fail("a link to a node", why);
		else if (strcmp(found.node, "/dev/net/tun") != 0 || strcmp(found.named, link) != 0)
			fail("a link to a node", found.named);
	}
	snprintf(plain, sizeof plain, "%s/1-2/idVendor", root);
	cases++;
	if (hull_hostdev_node(plain, &found, why, sizeof why) == NULL ||
	    strstr(why, "is not a device under /dev") == NULL)
		fail("a file that is not a device", why);
	cases++;
	if (hull_hostdev_node("/dev/hull-none", &found, why, sizeof why) == NULL ||
	    strstr(why, "No such file") == NULL)
		fail("a node that is not there", why);
	if (getuid() != 0 && access("/dev/ttyS0", F_OK) == 0 && access("/dev/ttyS0", W_OK) != 0) {
		cases++;
		if (hull_hostdev_node("/dev/ttyS0", &found, why, sizeof why) == NULL ||
		    strstr(why, "which the user may not read and write") == NULL)
			fail("a node the user may not write", why);
	}

	/* PCI by VFIO, over a sysfs and IOMMU groups of the test's own: a
	 * device not there, in no group, bound to its host driver, sharing its
	 * group with a device that is not given, and one whose group node the
	 * host has not. */
	{
		char pci[600];
		char groups[600];
		char path[1300];
		char target[1300];

		snprintf(pci, sizeof pci, "%s/pci", root);
		snprintf(groups, sizeof groups, "%s/groups", root);
		mkdir(pci, 0755);
		mkdir(groups, 0755);
		snprintf(path, sizeof path, "%s/drivers", root);
		mkdir(path, 0755);
		for (unsigned i = 0u; i < 3u; i++) {
			static const char *const drivers[] = { "vfio-pci", "e1000e", "pcieport" };

			snprintf(path, sizeof path, "%s/drivers/%s", root, drivers[i]);
			mkdir(path, 0755);
		}
		/* 0000:01:00.0, vfio-pci's in group 9998 with a pcieport bridge;
		 * 0000:02:00.0, e1000e's; 0000:03:00.0, vfio-pci's in group 9997
		 * beside 0000:03:00.1, e1000e's; 0000:04:00.0 in no group. */
		static const struct {
			const char *address;
			const char *driver;
			const char *group;
		} devices[] = {
			{ "0000:01:00.0", "vfio-pci", "9998" }, { "0000:00:1c.0", "pcieport", "9998" },
			{ "0000:02:00.0", "e1000e", "9996" },   { "0000:03:00.0", "vfio-pci", "9997" },
			{ "0000:03:00.1", "e1000e", "9997" },   { "0000:04:00.0", "vfio-pci", NULL },
		};
		for (unsigned i = 0u; i < sizeof devices / sizeof devices[0]; i++) {
			snprintf(path, sizeof path, "%s/%s", pci, devices[i].address);
			mkdir(path, 0755);
			snprintf(path, sizeof path, "%s/%s/driver", pci, devices[i].address);
			snprintf(target, sizeof target, "%s/drivers/%s", root, devices[i].driver);
			if (symlink(target, path) != 0)
				fail("a driver link", strerror(errno));
			if (devices[i].group == NULL)
				continue;
			snprintf(path, sizeof path, "%s/%s", groups, devices[i].group);
			mkdir(path, 0755);
			snprintf(path, sizeof path, "%s/%s/devices", groups, devices[i].group);
			mkdir(path, 0755);
			snprintf(path, sizeof path, "%s/%s/devices/%s", groups, devices[i].group,
			         devices[i].address);
			snprintf(target, sizeof target, "%s/%s", pci, devices[i].address);
			if (symlink(target, path) != 0)
				fail("a group's member", strerror(errno));
			snprintf(path, sizeof path, "%s/%s/iommu_group", pci, devices[i].address);
			snprintf(target, sizeof target, "%s/%s", groups, devices[i].group);
			if (symlink(target, path) != 0)
				fail("a group link", strerror(errno));
		}
		static const struct {
			const char *what;
			const char *address;
			const char *want;
		} pci_cases[] = {
			{ "a PCI device not there", "0000:09:00.0", "no PCI device 0000:09:00.0 is on the host" },
			{ "a PCI device in no group", "0000:04:00.0", "is in no IOMMU group" },
			{ "a PCI device of its host driver", "0000:02:00.0",
			  "is bound to e1000e, not vfio-pci, which an administrator binds it to once" },
			{ "a PCI device sharing its group", "0000:03:00.0",
			  "shares IOMMU group 9997 with 0000:03:00.1, bound to e1000e" },
			{ "a PCI device with no group node", "0000:01:00.0",
			  "is vfio-pci's, and /dev/vfio/9998 is not there" },
		};
		for (unsigned i = 0u; i < sizeof pci_cases / sizeof pci_cases[0]; i++) {
			cases++;
			if (hull_hostdev_pci(pci, groups, pci_cases[i].address, &found, why, sizeof why) ==
			        NULL ||
			    strstr(why, pci_cases[i].want) == NULL)
				fail(pci_cases[i].what, why);
		}
	}

	/* Held for one machine at a time: a serial port and a USB device a
	 * second owner is refused, all or none, and a plain node shared. */
	{
		hull_device mine[2];
		hull_device theirs[2];
		int         a = 0;
		int         b = 0;

		memset(mine, 0, sizeof mine);
		memset(theirs, 0, sizeof theirs);
		mine[0].kind = HULL_DEVICE_SERIAL;
		snprintf(mine[0].host.node, sizeof mine[0].host.node, "/dev/ttyUSB0");
		mine[1].kind = HULL_DEVICE_NODE;
		snprintf(mine[1].host.node, sizeof mine[1].host.node, "/dev/net/tun");
		theirs[0].kind = HULL_DEVICE_USB;
		snprintf(theirs[0].host.node, sizeof theirs[0].host.node, "/dev/bus/usb/001/007");
		theirs[1] = mine[0];
		cases++;
		if (hull_hostdev_claim(&a, mine, 2u, why, sizeof why) != NULL)
			fail("a first machine's devices", why);
		cases++;
		if (hull_hostdev_claim(&a, mine, 2u, why, sizeof why) != NULL)
			fail("its own devices taken again", why);
		cases++;
		if (hull_hostdev_claim(&b, theirs, 2u, why, sizeof why) == NULL ||
		    strstr(why, "/dev/ttyUSB0 is another running machine's") == NULL)
			fail("a held serial port given to a second machine", why);
		cases++;
		if (hull_hostdev_claim(&b, &mine[1], 1u, why, sizeof why) != NULL)
			fail("a plain node shared", why);
		cases++;
		if (hull_hostdev_claim(&a, &theirs[0], 1u, why, sizeof why) != NULL)
			fail("a refused claim left its USB device held", why);
		hull_hostdev_unclaim(&a);
		cases++;
		if (hull_hostdev_claim(&b, theirs, 2u, why, sizeof why) != NULL)
			fail("devices let go not taken by another", why);
		hull_hostdev_unclaim(&b);
	}

	{
		char command[1100];

		snprintf(command, sizeof command, "rm -rf '%s'", root);
		if (system(command) != 0)
			fail("removing the tree", root);
	}
	printf("hostdev: %u cases, %u failed\n", cases, failures);
	return failures != 0u;
}
