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

#endif
