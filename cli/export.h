#ifndef HULL_EXPORT_H
#define HULL_EXPORT_H

#include <stddef.h>

#include "store.h"

/* `hull export --bootable` (section 3.13): the stored tree `name` as a
 * disk image at `image` that boots under plain QEMU, VirtualBox or any
 * UEFI machine, `init` its PID 1. 0, or -1 with `why`. */
int hull_export_bootable(hull_store *store, const char *name, const char *image,
                         const char *init, char *why, size_t cap);

/* The same image in its hosted form, for an engine whose machine is a
 * disk (the virtualbox engine's, section 5): no /hull/boot, so the agent
 * waits for a host as in a vm, on the UART `host` names (ttyS0), the root
 * partition named to it on the kernel's command line and the console on
 * the next port. 0, or -1 with `why`. */
int hull_export_hosted(hull_store *store, const char *name, const char *image,
                       const char *host, char *why, size_t cap);

/* The same image as an appliance, `ova`: a streamOptimized VMDK and an
 * OVF naming the machine `title`, its memory and processors, a SATA disk
 * and an E1000 on NAT, tarred, the descriptor first. Needs Limine built
 * from source, the image then booting by BIOS, which a plain OVF gives in
 * VirtualBox and VMware alike. 0, or -1 with `why`. */
int hull_export_ova(hull_store *store, const char *name, const char *ova, const char *init,
                    const char *title, unsigned long memory_mib, unsigned cpus, char *why,
                    size_t cap);

/* At most this many disks in an appliance of a PC's. */
#define HULL_OVA_DISKS 8u

/* A PC as an appliance names it: `title` a plain word, its NIC OVF's
 * ResourceSubType (E1000, E1000e, PCNet32, virtio, VmxNet3), its disks on
 * an IDE controller where `ide`, else SATA, and EFI where `uefi`. */
typedef struct hull_ova_machine {
	const char    *title;
	unsigned long  memory_mib;
	unsigned       cpus;
	const char    *nic;
	int            ide;
	int            uefi;
	int            secure;
	const char    *sound;   /* OVF's: ensoniq1371, hdaudio, soundblaster16, or NULL */
	int            usb;     /* a USB controller, for a tablet */
} hull_ova_machine;

/* A PC's own disks, any format qemu-img reads, as an appliance `ova`:
 * each a streamOptimized VMDK, in the order given, the first the one it
 * boots, with an OVF of `machine`. VirtualBox's Export Appliance, the
 * reverse of `hull import --machine`. 0, or -1 with `why`. */
int hull_export_ova_disks(const char *const *disks, unsigned count, const char *ova,
                          const hull_ova_machine *machine, char *why, size_t cap);

#endif
