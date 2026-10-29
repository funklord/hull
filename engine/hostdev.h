/*
 * hostdev.h -- the host's devices a machine is given, (devices ...)
 * (section 4.13): found when the machine starts, not when it is planned,
 * since a board under test is plugged in, reset and plugged in again
 * between the two. Shared by the chroot engine, which binds a device's
 * node into the machine's /dev, and the qemu engine, which gives qemu the
 * device to put on a bus of the guest's.
 */

#ifndef HULL_HOSTDEV_H
#define HULL_HOSTDEV_H

#include <stddef.h>

/* Where sysfs lists USB devices, each named by its port path. */
#define HULL_SYSFS_USB "/sys/bus/usb/devices"

/* At most so many of a machine's devices, of every kind together. */
#define HULL_MAX_DEVICES 16u

/* What the spec asks for, kind by kind (J.1's devices). */
typedef enum hull_device_kind {
	HULL_DEVICE_USB,
	HULL_DEVICE_SERIAL,
	HULL_DEVICE_PARALLEL,
	HULL_DEVICE_NODE,
	HULL_DEVICE_PCI,
} hull_device_kind;

/* Where sysfs lists PCI devices and IOMMU groups, and where VFIO's
 * container node is, which every group's user opens too. */
#define HULL_SYSFS_PCI    "/sys/bus/pci/devices"
#define HULL_SYSFS_GROUPS "/sys/kernel/iommu_groups"
#define HULL_VFIO         "/dev/vfio/vfio"

typedef struct hull_hostdev {
	char     node[256];   /* the host's node: its real path, under /dev */
	char     named[256];  /* the path the spec gave, where it is not `node`, or "" */
	char     port[32];    /* USB: the port path sysfs names it by, "1-4.2"; PCI: its address */
	unsigned bus;         /* USB: its bus number */
	char     ids[12];     /* USB: "0403:6001", as sysfs has them */
	int      isochronous; /* USB: an endpoint of an interface's is, audio or video */
} hull_hostdev;

/* One device of (devices ...): what the spec names, and once the machine
 * starts, what that is on the host. A string not given is "". */
typedef struct hull_device {
	hull_device_kind kind;
	char             vendor[8];
	char             product[8];
	char             serial[128];
	char             port[32];
	char             path[256];  /* serial, parallel, node: the path given; pci: the address */
	hull_hostdev     host;
} hull_device;

/*
 * The USB device below `sysfs` that is `vendor` and `product`, and
 * `serial` where given, or that is plugged into `port`, or both; each of
 * them may be NULL. Its node must be the user's to read and write, which
 * usbfs needs for any transfer. NULL, or why in `why`.
 */
const char *hull_hostdev_usb(const char *sysfs, const char *vendor, const char *product,
                             const char *serial, const char *port, hull_hostdev *found,
                             char *why, size_t cap);

/*
 * The device node `path` is, followed through its links to a character
 * or block device under /dev that the user may read and write: a serial
 * or parallel port, a gpiochip, a disk. NULL, or why in `why`.
 */
const char *hull_hostdev_node(const char *path, hull_hostdev *found, char *why, size_t cap);

/* Whether an endpoint of an interface of the USB device at `port` below
 * `sysfs` is isochronous, sysfs's `PORT:C.I/ep_NN/type` "Isoc": audio or
 * video, whose timing a vm's usb-host keeps less well than the host. */
int hull_hostdev_usb_isochronous(const char *sysfs, const char *port);

/*
 * The PCI device at `address` below `sysfs`, for VFIO: bound to vfio-pci,
 * which an administrator does once, as root, for a device the host is to
 * give away; every other device of its IOMMU group, read below `groups`,
 * bound to vfio-pci too or to nothing a guest could reach through, a
 * bridge's pcieport or pci-stub, since the group is given whole; and its
 * group's node, /dev/vfio/N, the user's to read and write, which a udev
 * rule or chown gives. `found->node` is that node and `found->port` the
 * address. NULL, or why in `why`.
 */
const char *hull_hostdev_pci(const char *sysfs, const char *groups, const char *address,
                             hull_hostdev *found, char *why, size_t cap);

/* `device` found on the host, into its `host`: its node, and a USB
 * device's port. NULL, or why in `why`. */
const char *hull_hostdev_find(hull_device *device, char *why, size_t cap);

/*
 * The modules that drive the interfaces of the USB device at `port`
 * below `sysfs`, by each interface's modalias matched against
 * `aliases`, a kernel's modules.alias: so many names written to `names`,
 * each `name_cap` long, at most `count`. A device no module matches is
 * one the kernel drives built in, or one only a program drives through
 * usbfs. Returns how many, or -1 where `aliases` cannot be read.
 */
int hull_hostdev_usb_drivers(const char *sysfs, const char *port, const char *aliases,
                             char *names, size_t name_cap, unsigned count);

/*
 * `owner`'s devices held for it alone while it runs: a USB device, which
 * qemu takes from the host's driver, and a serial or parallel port, whose
 * bytes two machines would share. A node of (node ...) may be shared,
 * /dev/net/tun or a gpiochip being opened by many at once. All the
 * engines are in the one core, so one table holds them. Taken all or
 * none: NULL, or why in `why`, naming the device another owner holds.
 * Taking again what `owner` holds already is no change.
 */
const char *hull_hostdev_claim(const void *owner, const hull_device *devices, unsigned count,
                               char *why, size_t cap);

/* Everything `owner` holds, let go. */
void hull_hostdev_unclaim(const void *owner);

#endif
