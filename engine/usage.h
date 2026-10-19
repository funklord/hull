/*
 * usage.h -- what the host spends on a machine, read from /proc, for an
 * engine's stats() (project.md section 4.10).
 *
 * A process's figures count itself and every child it has reaped: the
 * kernel adds a child's CPU time and storage io to its parent's when the
 * parent waits for it. So summing the processes alive now counts each
 * process that ever ran once, so long as whatever reaped it is counted
 * too. Memory is only what is resident now, by Pss, so that pages two
 * processes share are not counted twice.
 */

#ifndef HULL_USAGE_H
#define HULL_USAGE_H

#include <stddef.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct hull_usage {
	unsigned long long cpu;     /* microseconds, user and system */
	unsigned long long io;      /* bytes read from and written to storage */
	unsigned long long memory;  /* bytes resident, by Pss */
} hull_usage;

/* Add process `pid`'s figures to `usage`: 1, or 0 if it cannot be read,
 * as when it has gone. */
int hull_usage_add(pid_t pid, hull_usage *usage);

/* Add those of every process in `pid`'s pid namespace: 1, or 0 if
 * `pid`'s namespace cannot be read. */
int hull_usage_namespace(pid_t pid, hull_usage *usage);

/* `(5:stats(3:cpu..)(2:io..)(6:memory..))`, hull_engine.h's stats
 * payload, into `out`: its length, or 0 if it does not fit. */
size_t hull_usage_encode(const hull_usage *usage, unsigned char *out, size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* HULL_USAGE_H */
