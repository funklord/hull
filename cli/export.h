#ifndef HULL_EXPORT_H
#define HULL_EXPORT_H

#include <stddef.h>

#include "store.h"

/* `hull export --bootable` (section 3.13): the stored tree `name` as a
 * disk image at `image` that boots under plain QEMU, VirtualBox or any
 * UEFI machine, `init` its PID 1. 0, or -1 with `why`. */
int hull_export_bootable(hull_store *store, const char *name, const char *image,
                         const char *init, char *why, size_t cap);

#endif
