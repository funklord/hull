/*
 * dos.h -- what the dos class's engines share (project.md section 5.0):
 * the plan read into a machine, its directory under the user's runtime
 * directory, the batch's output handed on as it grows, and the life of
 * the emulator's process. Each engine says what differs: the files it
 * writes for its batch, how it starts its emulator, and how it reads the
 * workload's end.
 */

#ifndef HULL_DOS_H
#define HULL_DOS_H

#include <pthread.h>
#include <sys/types.h>

#include "hull_engine.h"

#ifdef __cplusplus
extern "C" {
#endif

#define HULL_DOS_MAX_ARGS    64u
#define HULL_DOS_MAX_ENV     64u
#define HULL_DOS_MAX_VOLUMES 20u
#define HULL_DOS_COMMAND_CAP 126u   /* what DOS takes on a command line */

typedef struct hull_dos_engine hull_dos_engine;

struct hull_machine {
	const hull_dos_engine *engine;
	hull_core       *core;
	char             dir[4096];        /* the engine's own, under the runtime dir: H: */
	char            *root;             /* drive C: */
	int              overlay;          /* (mutable overlay): C: is a copy in `dir` */
	char            *workdir;
	char            *argv[HULL_DOS_MAX_ARGS];
	char            *envp[HULL_DOS_MAX_ENV];
	char            *volume_host[HULL_DOS_MAX_VOLUMES];
	char             volume_drive[HULL_DOS_MAX_VOLUMES];
	int              volume_readonly[HULL_DOS_MAX_VOLUMES];
	unsigned         volume_count;
	char             detail[512];
	pid_t            pid;
	pthread_t        reader;
	int              reading;
	pthread_mutex_t  lock;
	pthread_cond_t   done_cond;
	int              done;             /* the emulator has been reaped */
	int              paused;
};

struct hull_dos_engine {
	const char *name;          /* "dosbox", for the machine's directory and its messages */
	const char *reserved;      /* drive letters a volume may not take, C and H aside */
	const char *reserved_why;  /* and why */
	const char *not_found;     /* what the shell writes first for a command not there */
	int         space_free;    /* paths may hold no space, the emulator splitting on one */
	/* The batch and anything else the emulator reads, in m->dir: 0, or -1. */
	int (*write_files)(const hull_machine *m);
	/* In the child, after fork: exec the emulator, or return to fail. */
	void (*exec_child)(const hull_machine *m);
	/* The workload's end from what the emulator left and its wait status:
	 * the code, or -1 where it says nothing. */
	int (*end_code)(const hull_machine *m, int wstatus);
};

/* The batch's file `name`, upper case as DOS names it, or lower case as
 * a redirector may have made it on the host: an fd, or -1. */
int hull_dos_open(const hull_machine *m, const char *name);

/* `text` holds nothing a batch line would read as syntax. */
int hull_dos_plain(const char *text);

hull_status hull_dos_create(const hull_dos_engine *engine, hull_core *core, hull_buf plan,
                            hull_machine **out, hull_error *error);
hull_status hull_dos_start(hull_core *core, hull_machine *m, hull_error *error);
hull_status hull_dos_stop(hull_core *core, hull_machine *m, int timeout_ms, hull_error *error);
hull_status hull_dos_destroy(hull_core *core, hull_machine *m, hull_error *error);
hull_status hull_dos_signal(hull_core *core, hull_machine *m, hull_stream *stream, int signum,
                            hull_error *error);
hull_status hull_dos_pause(hull_core *core, hull_machine *m, int paused, hull_error *error);
hull_status hull_dos_stats(hull_core *core, hull_machine *m, hull_buf *out, hull_error *error);

/* The engines' host ops, given at registration. */
extern const hull_host_ops *hull_dos_host;

#ifdef __cplusplus
}
#endif

#endif /* HULL_DOS_H */
