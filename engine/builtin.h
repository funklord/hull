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
const hull_engine *hull_engine_chroot(const hull_host_ops *host);

#ifdef __cplusplus
}
#endif

#endif /* HULL_BUILTIN_H */
