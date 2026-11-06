/*
 * hw.h -- a pool's members, read from pools.conf (project.md section 4.12),
 * for the hw engine, the core's admission and `hull board`.
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

#define HULL_POOL_MEMBERS 64u

/* An entry of a pool's (members ...): a board, or a (slot ...), a named
 * place for a software machine. What a machine is stays its spec's. */
typedef struct hull_member {
	char name[64];
	int  board;     /* a (board ...), or 0 for a (slot ...) */
	int  priority;  /* the higher fills first; 0 where it says none */
	int  by_name;   /* (by_name true): given only to what names it, and last in the list */
	int  broken;    /* a board marked broken */
} hull_member;

typedef struct hull_pool {
	long long   capacity;  /* (capacity N), unnamed slots at priority 0; or 0 */
	unsigned    count;
	hull_member members[HULL_POOL_MEMBERS];  /* in the order they fill */
	char        wrong[200];                  /* what the file says that cannot be, or "" */
} hull_pool;

/* The pool `name` of pools.conf: 0, or -1 where it does not name it. */
int hull_pool_read(const char *name, hull_pool *pool);

/* The members of `pool`, at most `cap`: their count, or -1 where it has none. */
int hull_hw_boards(const char *pool, hull_board *boards, unsigned cap);

/* Whether `name` is marked broken, out of its pool until fixed. */
int hull_hw_broken(const char *name);

/* The mark taken off: 0, or -1 where there was none. */
int hull_hw_fix(const char *name);

#endif
