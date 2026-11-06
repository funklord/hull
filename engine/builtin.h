/*
 * builtin.h -- the engines that ship with the core (project.md section
 * 5.0), as declarations.
 *
 * Each is its column of the matrix, written from what sections 5 and 6
 * say of it; a field the document does not say an engine honours is left
 * out, and so is rejected. docker's and qemu's operations are not written
 * yet: every slot is NULL, which is enough for planning and `hull check`
 * and for nothing that runs a machine. chroot, the reference engine,
 * runs one (section 4.8).
 *
 * Each takes the core's callbacks, as hull_engine_register() does.
 */

#ifndef HULL_BUILTIN_H
#define HULL_BUILTIN_H

#include "hull_engine.h"

#ifdef __cplusplus
extern "C" {
#endif

const hull_engine *hull_engine_docker(const hull_host_ops *host);
const hull_engine *hull_engine_qemu(const hull_host_ops *host);
const hull_engine *hull_engine_virtualbox(const hull_host_ops *host);
const hull_engine *hull_engine_chroot(const hull_host_ops *host);
const hull_engine *hull_engine_dosbox(const hull_host_ops *host);
const hull_engine *hull_engine_dosemu(const hull_host_ops *host);
const hull_engine *hull_engine_qemu_pc(const hull_host_ops *host);
const hull_engine *hull_engine_hw(const hull_host_ops *host);

/* A bootable export's kernel, the host's, its path into `kernel`, and
 * `dir`/initrd, whose /hull/boot is `boot` (engine/qemu.c). NULL, or why
 * in `detail`. */
const char *hull_qemu_boot_files(const char *dir, const char *boot, char *kernel, size_t cap,
                                 char *detail, size_t detail_cap);

#ifdef __cplusplus
}
#endif

#endif /* HULL_BUILTIN_H */
