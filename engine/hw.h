/*
 * hw.h -- a pool's boards, read from pools.conf (project.md section 4.12),
 * for the hw engine, the core's capacity and `hull board`.
 */

#ifndef HULL_HW_H
#define HULL_HW_H

typedef struct hull_board {
	char     name[64];
	char     address[128];
	char     login[64];
	char     power[512];     /* a command, given on, off or cycle */
	char     console[512];   /* a command whose output is the machine's log */
	char     health[512];    /* a command that is 0 for a board fit to lease */
	char     root[512];      /* where a tree lands on it */
	unsigned port;
	unsigned wait;           /* seconds its sshd has to answer after power on */
} hull_board;

/* The members of `pool`, at most `cap`: their count, or -1 where it has none. */
int hull_hw_boards(const char *pool, hull_board *boards, unsigned cap);

/* Whether `name` is marked broken, out of its pool until fixed. */
int hull_hw_broken(const char *name);

/* The mark taken off: 0, or -1 where there was none. */
int hull_hw_fix(const char *name);

#endif
