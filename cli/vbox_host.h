/*
 * vbox_host.h -- `hull virtualbox host`, which the virtualbox engine runs
 * for each machine: not a command anybody types.
 */

#ifndef HULL_VBOX_HOST_H
#define HULL_VBOX_HOST_H

/* Make the machine at `root`, of `memory` MiB and `cpus` processors, a
 * disk in `work`, run it under VirtualBox inside a qemu vm on a core of
 * its own, and join stdin and stdout to its agent's UART until it ends.
 * `agent` is hull-agent's path, `network` "nat" or "none", and `ssh` "ssh"
 * where the fetched sshd is to be given a disk of its own; `volumes` a
 * host path and "ro" or "rw" for each of `volume_count`, which the agent
 * mounts as v0 on, as a vm's. Its status. */
int hull_vbox_host(const char *work, const char *root, const char *agent, const char *memory,
                   const char *cpus, const char *network, const char *ssh,
                   char *const *volumes, unsigned volume_count);

/* `hull virtualbox pc`, the virtualbox-pc engine's: a PC of its own media
 * under VirtualBox inside a qemu vm, as above but with no agent, its
 * first serial port joined to stdout until it ends. `settings` are the
 * memory in MiB, the processors, the network (nat or none), the firmware
 * (bios or efi), the card's VirtualBox type, the chipset (piix3 or ich9),
 * the TPM (yes or no), the first boot device (disk, dvd or floppy), the
 * screen (none, local or window: VirtualBox's window on X in the outer vm,
 * the outer vm's screen shown as `(display ...)` shows a vm's) and, for
 * local, where to link the outer vm's display monitor, else "-", and the
 * seconds Enter is pressed for at the start, (boot_key N), "0" for none,
 * and the 3D (none, virgl or venus: VMSVGA's 3D on the outer vm's own);
 * `mode` direct or overlay; `media` a type (disk, cdrom, floppy) and an
 * absolute path for each of `media_count`. `work` is made, and removed at
 * the end. Its status. */
int hull_vbox_pc(const char *work, const char *agent, const char *const *settings,
                 const char *mode, char *const *media, unsigned media_count);

#endif
