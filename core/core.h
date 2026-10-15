/*
 * core.h -- the per-user core (project.md section 4.10).
 */

#ifndef HULL_CORE_H
#define HULL_CORE_H

#ifdef __cplusplus
extern "C" {
#endif

/* Serve on $XDG_RUNTIME_DIR/hull/core.sock until idle, or until SIGINT or
 * SIGTERM, then stop every machine still running. Returns 0, including
 * when another core already holds the lock, or 1 if it could not start. */
int hull_core_run(const char *host_arch);

#ifdef __cplusplus
}
#endif

#endif /* HULL_CORE_H */
