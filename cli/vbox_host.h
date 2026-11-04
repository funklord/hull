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

#endif
