/*
 * core.c -- the per-user core (project.md section 4.10).
 *
 * One process per user, holding the engines and every machine they run,
 * reached by frontends on $XDG_RUNTIME_DIR/hull/core.sock. One thread
 * runs everything that touches the machine table: a poll() over the
 * socket, the clients and a wake pipe. What engines report from their own
 * threads -- output, an exit, a state -- is queued and the wake pipe
 * written, so the table has one writer.
 *
 * Each machine gets a hull_core of its own to hand its engine, because
 * on_data says which core it is for and not which machine: the core
 * pointer is how an event finds its machine.
 *
 * A stop is carried out on a thread of its own, since an engine's stop()
 * blocks for up to its timeout and the loop must go on serving.
 *
 * Nothing the loop does waits on a frontend. What is sent to one is
 * queued, and written as its socket takes it; a machine's output is not
 * queued at all, but read from the machine's log by each follower at the
 * pace its socket drains, so one that falls behind holds back neither
 * the core nor the machine. It loses only what leaves the log before it
 * is read, and is told how much.
 *
 * Nobody is left to stop the core: it exits once it has had no client
 * and no running machine for HULL_CORE_IDLE seconds, 30 unless the
 * environment says otherwise.
 */

#define _GNU_SOURCE

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <arpa/inet.h>
#include <linux/magic.h>

#include "access.h"
#include "builtin.h"
#include "core.h"
#include "registry.h"
#include "segment.h"
#include "wire.h"

/* The commit this core was built from, so a frontend of another build can
 * see the skew: the core runs the engine, and a core left from an older
 * build serves its old engine to a newer frontend (as cli/hull.c's commit
 * does for --version). fmake's build writes no commit.h and says so. */
#if defined(__has_include)
#if __has_include("commit.h")
#include "commit.h"
#endif
#endif
#ifndef HULL_COMMIT
#define HULL_COMMIT "unknown"
#endif

#define MAX_CLIENTS      32u
#define MAX_MACHINES     512u   /* the user's, every file's together */
#define MAX_ATTACHED     16u
#define MAX_STOPS        16u
#define MAX_EXECS        64u
#define LOG_LIMIT        (1u << 20)
#define DATA_CHUNK       65536u
#define INPUT_WINDOW     65536u  /* an exec's stdin the core holds, at most */
#define OUTPUT_WINDOW    65536u  /* an exec's output a frontend has unasked */
#define RUN_WINDOW       262144u /* a machine's output its own run has not been sent */
#define FEED_LOW         65536u  /* a follower is fed while less than this waits */
#define OUTBOX_LIMIT     (8u << 20)  /* answers a frontend leaves unread, at most */
#define FOLLOW_ON        UINT64_MAX

/* --- the table ----------------------------------------------------------- */

typedef struct client {
	int        used;
	hull_wire  wire;
	int        dead;  /* a send failed: closed at the end of this turn */

	/* Framed messages not yet written; the socket does not block, and
	 * poll() says when it takes more. */
	uint8_t   *out;
	size_t     out_length;
	size_t     out_cap;
	size_t     out_sent;
} client;

/* Someone reading a machine's output, by where it is in the log: offsets
 * count every byte the machine has written. `until` ends a `logs` that
 * does not follow; FOLLOW_ON follows to the exit. */
typedef struct follower {
	client   *client;
	uint32_t  ch;
	uint64_t  at;
	uint64_t  until;
	int       holds;  /* the run that started the machine: it holds it back */
} follower;

/* One record of output: which stream, and its bytes. */
typedef struct record {
	uint8_t        fd;
	uint32_t       length;
	struct record *next;
	uint8_t        bytes[];
} record;

typedef struct slot slot;

struct hull_core {
	slot     *slot;
	unsigned  generation;  /* the slot's, when this machine was made */
};

struct slot {
	int                used;
	unsigned           generation;  /* counts the machines this slot has held */
	char               id[64];
	char               address[16]; /* section 8.1's, from the id */
	const hull_engine *engine;
	char               class_name[8];
	hull_core          core;
	hull_machine      *handle;
	char               state[16];
	int                keep;        /* (on_disconnect keep) */
	int                kept;        /* hull/machines/ID is written for it */
	int                secrets;     /* hull/secrets/ID is written for it */
	int                ssh;         /* hull/ssh/ID is written for it, (ssh true) */
	client            *owner;       /* the frontend that ran it */
	int                paused;      /* hull pause, and no unpause since */
	/* The plan's (impair ...): each segment's link, given its port
	 * as the machine joins it. */
	struct {
		char            segment[64];
		int             given[2];
		hull_impairment way[2];
		int             tampered[2];
		hull_tamper     fault[2];
	}                  links[8];
	unsigned           link_count;
	/* The plan's (names ...): hull's address for it on each segment it
	 * addresses, so that the switch can refuse another file's machine
	 * drawing the same (Appendix D). */
	struct {
		char     segment[64];
		uint32_t address;
		unsigned prefix;
		hull_segment_host *hosts;   /* the plan's, every machine it names there */
		unsigned           host_count;
	}                  named[8];
	unsigned           named_count;
	/* A network out besides its segments, so that hull at .1 forwards
	 * what it does not hold to the host's resolvers: not (network none),
	 * whose machine must reach nothing beyond the segment, DNS included. */
	int                forwards;
	int                exited;
	int                code;
	int                signal;
	follower           followers[MAX_ATTACHED];
	unsigned           follower_count;
	follower           stops[MAX_STOPS];  /* answered `done` at the exit */
	unsigned           stop_count;
	/* The run that started the machine holds it back, as a pipe holds the
	 * command writing into it (section 4.10): under flow_lock, the output
	 * the machine has made and how much of it that run has been sent,
	 * and whether it still holds -- until it is gone, the machine is
	 * stopped, or the core goes. */
	uint64_t           made;
	uint64_t           hold_at;
	int                holding;
	record            *log;
	record            *log_tail;
	size_t             log_size;
	uint64_t           log_start;   /* the offset of the log's first byte */
	uint64_t           log_end;     /* and of the byte after its last */
};

/* A command a frontend runs in a machine, on one of its channels. The
 * engine knows it only by its stream pointer, which is what its events
 * carry; an entry lasts until the stream's exit has been seen, and the
 * queue is drained to the end before any client is served again, so a
 * pointer freed by stream_close() is never mistaken for a new one. */
typedef struct exec_entry {
	int          used;
	int          closed;  /* stream_close() done; waiting for the exit */
	int          tell;    /* closed by its machine's end: the exit is still the
	                         frontend's to hear */
	client      *client;
	uint32_t     ch;
	slot        *machine;
	hull_stream *stream;

	/* stdin. The engine's stream_write() blocks while the command does
	 * not read, so a thread of the exec's own makes it, from a ring the
	 * loop fills. The frontend sends only what it has credit for, and
	 * credit is given back as bytes reach the command, so the ring never
	 * holds more than INPUT_WINDOW. */
	size_t          credit;       /* the frontend's: the loop's alone */
	int             told_closed;  /* the frontend has heard its input closed */
	pthread_t       writer;
	pthread_mutex_t lock;         /* what follows, shared with the writer */
	pthread_cond_t  more;
	uint8_t        *input;
	size_t          head;
	size_t          queued;
	size_t          written;      /* written, and not yet given back */
	int             input_ended;  /* the frontend's eof */
	int             input_closed; /* a write failed: what comes is dropped */
	int             stopping;

	/* Its output, on B.2's credit the other way. The engine's reader
	 * waits in on_data() while the frontend has not taken what it was
	 * sent, so a frontend that falls behind holds its command back, as
	 * a pipe would, rather than stalling the core. These, and `used`
	 * and `stream`, are under flow_lock, being the reader's to read. */
	size_t          held;         /* read, and not yet credited back */
	int             output_open;  /* the reader may wait */
} exec_entry;

static client        clients[MAX_CLIENTS];
/* Bumped as each client takes its place, which a disconnect clears: an
 * answer made off the loop, gc's, reaches only the client that asked. */
static unsigned      client_generation[MAX_CLIENTS];
static slot          machines[MAX_MACHINES];
static exec_entry    execs[MAX_EXECS];
static hull_registry registry;

/* An engine's reader can report a stream's first output before exec()
 * has returned it. The loop makes one exec() at a time, so a stream no
 * entry knows is the one under way, which `adopting` is. */
static pthread_mutex_t flow_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  flow_room = PTHREAD_COND_INITIALIZER;

/* The machine `s` no longer held back by its run: woken if it waits. */
static void release_hold(slot *s)
{
	pthread_mutex_lock(&flow_lock);
	s->holding = 0;
	pthread_cond_broadcast(&flow_room);
	pthread_mutex_unlock(&flow_lock);
}
static exec_entry     *adopting;

static exec_entry *exec_of(hull_stream *stream);

/* --- events from engine threads ------------------------------------------ */

typedef enum event_kind { EVENT_DATA, EVENT_EXIT, EVENT_STATE, EVENT_GC, EVENT_GC_DONE } event_kind;

typedef struct event {
	event_kind     kind;
	slot          *slot;
	unsigned       generation;
	hull_stream   *stream;  /* an exec's, or NULL for the machine's own */
	int            fd;
	int            code;
	int            signal;
	char           state[16];
	unsigned       client;     /* gc's: the client's place, its generation, */
	unsigned       client_generation;
	uint32_t       ch;         /* and the channel it asked on */
	size_t         length;
	struct event  *next;
	uint8_t        bytes[];
} event;

static pthread_mutex_t queue_lock = PTHREAD_MUTEX_INITIALIZER;
static event          *queue_head;
static event          *queue_tail;
static int             wake[2] = { -1, -1 };

static void enqueue(event *e)
{
	char byte = 0;

	pthread_mutex_lock(&queue_lock);
	if (queue_tail != NULL)
		queue_tail->next = e;
	else
		queue_head = e;
	queue_tail = e;
	pthread_mutex_unlock(&queue_lock);
	if (write(wake[1], &byte, 1u) < 0) {
		/* The pipe is full, so a wakeup is already waiting. */
	}
}

static event *new_event(event_kind kind, hull_core *core, size_t length)
{
	event *e = calloc(1u, sizeof *e + length);

	if (e != NULL) {
		e->kind       = kind;
		e->slot       = core != NULL ? core->slot : NULL;
		e->generation = core != NULL ? core->generation : 0u;
		e->length     = length;
	}
	return e;
}

static void on_data(hull_core *core, hull_stream *stream, int fd, const uint8_t *data, size_t n)
{
	event *e = new_event(EVENT_DATA, core, n);

	if (e == NULL || e->slot == NULL) {
		free(e);
		return;
	}
	if (stream != NULL) {
		exec_entry *x;

		pthread_mutex_lock(&flow_lock);
		x = exec_of(stream);
		if (x == NULL && adopting != NULL) {
			x = adopting;
			x->stream = stream;
		}
		while (x != NULL && x->output_open && x->held + n > OUTPUT_WINDOW)
			pthread_cond_wait(&flow_room, &flow_lock);
		if (x != NULL)
			x->held += n;
		pthread_mutex_unlock(&flow_lock);
	}
	/* The machine's own output waits while its run is a window behind,
	 * so a run read slowly holds the machine back as a pipe would, and is
	 * never lapped by the log: the engine's reader stops reading, and the
	 * workload blocks writing. */
	if (stream == NULL && e->generation == e->slot->generation) {
		slot *s = e->slot;

		pthread_mutex_lock(&flow_lock);
		while (s->holding && s->made + n - s->hold_at > RUN_WINDOW)
			pthread_cond_wait(&flow_room, &flow_lock);
		s->made += n;
		pthread_mutex_unlock(&flow_lock);
	}
	e->stream = stream;
	e->fd = fd;
	memcpy(e->bytes, data, n);
	enqueue(e);
}

static void on_eof(hull_core *core, hull_stream *stream, int fd)
{
	(void)core;
	(void)stream;
	(void)fd;
}

static void on_exit_status(hull_core *core, hull_machine *machine, hull_stream *stream,
                           int code, int signal)
{
	event *e = new_event(EVENT_EXIT, core, 0u);

	(void)machine;
	if (e == NULL || e->slot == NULL) {
		free(e);
		return;
	}
	e->stream = stream;
	e->code   = code;
	e->signal = signal;
	enqueue(e);
}

static void on_state(hull_core *core, hull_machine *machine, const char *state)
{
	event *e = new_event(EVENT_STATE, core, 0u);

	(void)machine;
	if (e == NULL || e->slot == NULL) {
		free(e);
		return;
	}
	snprintf(e->state, sizeof e->state, "%s", state);
	enqueue(e);
}

static void on_log(hull_core *core, int level, const char *message)
{
	(void)core;
	if (level >= 2)
		fprintf(stderr, "hull core: %s\n", message);
}

/* The port is labelled with its machine's id, which `hull segment` names
 * it by and an impairment is asked for. */
static int on_segment(hull_core *core, const char *key, char *why, size_t cap)
{
	const slot              *s          = core->slot;
	uint32_t                 address    = 0u;
	unsigned                 prefix     = 0u;
	const hull_segment_host *hosts      = NULL;
	unsigned                 host_count = 0u;
	int                      fd;

	for (unsigned i = 0u; i < s->named_count; i++) {
		if (strcmp(s->named[i].segment, key) == 0) {
			address    = s->named[i].address;
			prefix     = s->named[i].prefix;
			hosts      = s->named[i].hosts;
			host_count = s->named[i].host_count;
		}
	}
	fd = hull_segment_port_named(key, s->id, address, prefix, s->forwards, hosts, host_count,
	                             why, cap);

	/* Before the machine runs, so that its first frame meets its link. */
	for (unsigned i = 0u; fd >= 0 && i < s->link_count; i++) {
		if (strcmp(s->links[i].segment, key) != 0)
			continue;
		for (int direction = 0; direction < 2; direction++) {
			if ((s->links[i].given[direction] &&
			     hull_segment_impair(key, s->id, direction, &s->links[i].way[direction], why,
			                         cap) != 0) ||
			    (s->links[i].tampered[direction] &&
			     hull_segment_tamper(key, s->id, direction, &s->links[i].fault[direction], why,
			                         cap) != 0)) {
				close(fd);
				return -1;
			}
		}
	}
	return fd;
}

static const hull_host_ops host_ops = {
	.struct_size = sizeof (hull_host_ops),
	.on_data     = on_data,
	.on_eof      = on_eof,
	.on_exit     = on_exit_status,
	.on_state    = on_state,
	.log         = on_log,
	.segment     = on_segment,
};

/* --- answering ----------------------------------------------------------- */

/* Queue `m` for `c`. A frontend that leaves OUTBOX_LIMIT of its answers
 * unread is dropped: what is bounded otherwise -- an exec's output by its
 * credit, a machine's by its log -- cannot fill it. */
static void send_to(client *c, hull_msg *m)
{
	size_t   need = (size_t)m->length + HULL_AGENT_LENGTH_SIZE;
	uint32_t wrote = 0u;

	if (!c->used || c->dead) {
		hull_msg_free(m);
		return;
	}
	if (m->failed || m->data == NULL || c->out_length - c->out_sent + need > OUTBOX_LIMIT) {
		c->dead = 1;
		hull_msg_free(m);
		return;
	}
	if (c->out_sent > 0u) {
		memmove(c->out, c->out + c->out_sent, c->out_length - c->out_sent);
		c->out_length -= c->out_sent;
		c->out_sent = 0u;
	}
	if (c->out_length + need > c->out_cap) {
		size_t   cap  = c->out_cap > 0u ? c->out_cap : 65536u;
		uint8_t *grown;

		while (cap < c->out_length + need)
			cap *= 2u;
		grown = realloc(c->out, cap);
		if (grown == NULL) {
			c->dead = 1;
			hull_msg_free(m);
			return;
		}
		c->out     = grown;
		c->out_cap = cap;
	}
	if (hull_agent_frame(c->out + c->out_length, (uint32_t)(c->out_cap - c->out_length), m->data,
	                     m->length, &wrote) != HULL_AGENT_OK)
		c->dead = 1;
	else
		c->out_length += wrote;
	hull_msg_free(m);
}

static void send_error(client *c, uint32_t ch, const char *reason)
{
	hull_msg m = { 0 };

	hull_msg_raw(&m, "(5:error");
	hull_msg_channel(&m, ch);
	hull_msg_raw(&m, "(6:reason");
	hull_msg_text(&m, reason);
	hull_msg_raw(&m, "))");
	send_to(c, &m);
}

static void send_simple(client *c, const char *type, uint32_t ch)
{
	hull_msg m = { 0 };

	hull_msg_raw(&m, type);
	hull_msg_channel(&m, ch);
	hull_msg_raw(&m, ")");
	send_to(c, &m);
}

static void send_data(client *c, uint32_t ch, int fd, const uint8_t *bytes, size_t length)
{
	while (length > 0u) {
		size_t   n = length < DATA_CHUNK ? length : DATA_CHUNK;
		hull_msg m = { 0 };

		hull_msg_raw(&m, "(4:data(3:buf");
		hull_msg_atom(&m, bytes, n);
		hull_msg_raw(&m, ")");
		hull_msg_channel(&m, ch);
		hull_msg_raw(&m, fd == 2 ? "(2:fd1:2))" : "(2:fd1:1))");
		send_to(c, &m);
		bytes += n;
		length -= n;
	}
}

static void send_status(client *c, uint32_t ch, int code, int signal)
{
	hull_msg m = { 0 };

	hull_msg_raw(&m, "(6:exited");
	hull_msg_channel(&m, ch);
	hull_msg_raw(&m, "(4:code");
	hull_msg_number(&m, code);
	hull_msg_raw(&m, ")(6:signal");
	hull_msg_number(&m, signal);
	hull_msg_raw(&m, "))");
	send_to(c, &m);
}

static void send_exited(client *c, uint32_t ch, const slot *s)
{
	send_status(c, ch, s->code, s->signal);
}

/* A follower fell so far behind that `bytes` of the machine's output left
 * the log before it was read. */
static void send_lost(client *c, uint32_t ch, uint64_t bytes)
{
	hull_msg m = { 0 };

	hull_msg_raw(&m, "(4:lost(5:bytes");
	hull_msg_number(&m, (long long)bytes);
	hull_msg_raw(&m, ")");
	hull_msg_channel(&m, ch);
	hull_msg_raw(&m, ")");
	send_to(c, &m);
}

static void send_credit(client *c, uint32_t ch, size_t bytes)
{
	hull_msg m = { 0 };

	hull_msg_raw(&m, "(6:credit(5:bytes");
	hull_msg_number(&m, (long long)bytes);
	hull_msg_raw(&m, ")");
	hull_msg_channel(&m, ch);
	hull_msg_raw(&m, ")");
	send_to(c, &m);
}

/* --- execs --------------------------------------------------------------- */

static exec_entry *exec_of(hull_stream *stream)
{
	for (unsigned i = 0u; i < MAX_EXECS; i++)
		if (execs[i].used && execs[i].stream == stream)
			return &execs[i];
	return NULL;
}

static exec_entry *exec_on(const client *c, uint32_t ch)
{
	for (unsigned i = 0u; i < MAX_EXECS; i++)
		if (execs[i].used && !execs[i].closed && execs[i].client == c && execs[i].ch == ch)
			return &execs[i];
	return NULL;
}

/* End the command; its exit is still to come, and frees the entry. */
static void close_exec(exec_entry *x)
{
	hull_error             error = { .struct_size = sizeof error };
	const hull_engine_ops *ops = x->machine->engine->ops;

	if (x->closed)
		return;
	x->closed = 1;
	/* A reader waiting for credit is let go: what it reads now is
	 * dropped, and stream_close() waits for it. */
	pthread_mutex_lock(&flow_lock);
	x->output_open = 0;
	pthread_cond_broadcast(&flow_room);
	pthread_mutex_unlock(&flow_lock);
	/* The writer first: a write under way is ended by stream_eof(), and
	 * stream_close() comes only once none is. */
	pthread_mutex_lock(&x->lock);
	x->stopping = 1;
	pthread_cond_signal(&x->more);
	pthread_mutex_unlock(&x->lock);
	ops->stream_eof(&x->machine->core, x->stream, &error);
	pthread_join(x->writer, NULL);
	ops->stream_close(&x->machine->core, x->stream, &error);
	pthread_cond_destroy(&x->more);
	pthread_mutex_destroy(&x->lock);
	free(x->input);
	x->input = NULL;
}

/* The writer: the ring to the command, then its end. */
static void *write_input(void *arg)
{
	exec_entry            *x = arg;
	const hull_engine_ops *ops = x->machine->engine->ops;
	hull_error             error = { .struct_size = sizeof error };
	char                   byte = 0;

	pthread_mutex_lock(&x->lock);
	for (;;) {
		size_t      n;
		hull_status status;

		while (!x->stopping && x->queued == 0u && !x->input_ended)
			pthread_cond_wait(&x->more, &x->lock);
		if (x->stopping)
			break;
		if (x->queued == 0u) {
			pthread_mutex_unlock(&x->lock);
			ops->stream_eof(&x->machine->core, x->stream, &error);
			pthread_mutex_lock(&x->lock);
			break;
		}
		/* Only this thread moves the head, and the loop writes only
		 * past the queued bytes, so these are the writer's to read. */
		n = x->queued < INPUT_WINDOW - x->head ? x->queued : INPUT_WINDOW - x->head;
		pthread_mutex_unlock(&x->lock);
		status = ops->stream_write(&x->machine->core, x->stream, x->input + x->head, n,
		                           &error);
		pthread_mutex_lock(&x->lock);
		x->head = (x->head + n) % INPUT_WINDOW;
		x->queued -= n;
		x->written += n;
		if (status != HULL_OK) {
			x->input_closed = 1;
			x->written += x->queued;
			x->queued = 0u;
		}
		if (write(wake[1], &byte, 1u) < 0) {
			/* The pipe is full, so a wakeup is already waiting. */
		}
		if (status != HULL_OK)
			break;
	}
	pthread_mutex_unlock(&x->lock);
	return NULL;
}

/* --- machines ------------------------------------------------------------ */

static slot *find_machine(const char *id)
{
	for (unsigned i = 0u; i < MAX_MACHINES; i++)
		if (machines[i].used && strcmp(machines[i].id, id) == 0)
			return &machines[i];
	return NULL;
}

static void drop_log(slot *s)
{
	while (s->log != NULL) {
		record *next = s->log->next;

		free(s->log);
		s->log = next;
	}
	s->log_tail  = NULL;
	s->log_size  = 0u;
	s->log_start = s->log_end;
}

/* Keep the newest LOG_LIMIT bytes, a record at a time. */
static void keep_log(slot *s, int fd, const uint8_t *bytes, size_t length)
{
	record *r = malloc(sizeof *r + length);

	if (r == NULL)
		return;
	r->fd     = (uint8_t)fd;
	r->length = (uint32_t)length;
	r->next   = NULL;
	memcpy(r->bytes, bytes, length);
	if (s->log_tail != NULL)
		s->log_tail->next = r;
	else
		s->log = r;
	s->log_tail = r;
	s->log_size += length;
	s->log_end  += length;
	while (s->log_size > LOG_LIMIT && s->log != r) {
		record *old = s->log;

		s->log = old->next;
		s->log_size  -= old->length;
		s->log_start += old->length;
		free(old);
	}
}

static void add_follower(slot *s, client *c, uint32_t ch, uint64_t at, uint64_t until)
{
	if (s->follower_count < MAX_ATTACHED)
		s->followers[s->follower_count++] = (follower){ c, ch, at, until, 0 };
	else
		send_error(c, ch, "the machine has as many followers as it can");
}

/* Give `f` the next of what it has to hear, if anything: what it lost, a
 * piece of the log, or its end. 1 if a message was queued; `*done` once
 * the follower has heard all it will. */
static int feed_one(slot *s, follower *f, int *done)
{
	uint64_t end = f->until < s->log_end ? f->until : s->log_end;

	*done = 0;
	if (f->at < s->log_start) {
		send_lost(f->client, f->ch, s->log_start - f->at);
		f->at = s->log_start;
		return 1;
	}
	if (f->at < end) {
		uint64_t base = s->log_start;

		for (record *r = s->log; r != NULL; base += r->length, r = r->next) {
			if (f->at < base + r->length) {
				uint64_t into = f->at - base;
				uint64_t n    = r->length - into;

				if (n > end - f->at)
					n = end - f->at;
				if (n > DATA_CHUNK)
					n = DATA_CHUNK;
				send_data(f->client, f->ch, r->fd, r->bytes + into, (size_t)n);
				f->at += n;
				if (f->holds) {
					pthread_mutex_lock(&flow_lock);
					s->hold_at = f->at;
					pthread_cond_broadcast(&flow_room);
					pthread_mutex_unlock(&flow_lock);
				}
				return 1;
			}
		}
		f->at = end;  /* not reached: the log holds every offset in it */
	}
	if (f->until != FOLLOW_ON) {
		send_simple(f->client, "(3:eof", f->ch);
		*done = 1;
		return 1;
	}
	if (s->exited) {
		send_exited(f->client, f->ch, s);
		*done = 1;
		return 1;
	}
	return 0;
}

/* Feed `c`'s followers while little of its output waits: 1 if any was. */
static int feed(client *c)
{
	int fed = 0;

	for (unsigned i = 0u; i < MAX_MACHINES; i++) {
		slot    *s = &machines[i];
		unsigned kept = 0u;

		if (!s->used)
			continue;
		for (unsigned f = 0u; f < s->follower_count; f++) {
			follower *one  = &s->followers[f];
			int       done = 0;

			if (one->client == c)
				while (!c->dead && c->out_length - c->out_sent < FEED_LOW &&
				       feed_one(s, one, &done) && !done)
					fed = 1;
			fed |= done;
			if (!done)
				s->followers[kept++] = *one;
			else if (one->holds)
				release_hold(s);
		}
		s->follower_count = kept;
	}
	return fed;
}

/* Write what `c` has waiting, as far as its socket takes it, feeding its
 * followers as it empties. */
static void flush(client *c)
{
	while (c->used && !c->dead) {
		while (c->out_sent < c->out_length) {
			ssize_t n = write(c->wire.fd, c->out + c->out_sent, c->out_length - c->out_sent);

			if (n < 0 && errno == EINTR)
				continue;
			if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
				return;
			if (n <= 0) {
				c->dead = 1;
				return;
			}
			c->out_sent += (size_t)n;
		}
		c->out_sent = c->out_length = 0u;
		if (!feed(c))
			return;
	}
}

/* A stop on a thread of its own: an engine's stop() may block for its
 * whole timeout. */
typedef struct stop_job {
	const hull_engine *engine;
	hull_core         *core;
	hull_machine      *handle;
	int                timeout_ms;
} stop_job;

static void *run_stop(void *arg)
{
	stop_job  *job   = arg;
	hull_error error = { .struct_size = sizeof error };

	job->engine->ops->stop(job->core, job->handle, job->timeout_ms, &error);
	free(job);
	return NULL;
}

/* A paused machine let go on, so that it can hear what is asked of it
 * next: a stopped process acts on no signal but SIGKILL. */
static void wake_machine(slot *s)
{
	hull_error error = { .struct_size = sizeof error };

	if (!s->paused || s->handle == NULL)
		return;
	s->engine->ops->pause(&s->core, s->handle, 0, &error);
	s->paused = 0;
	snprintf(s->state, sizeof s->state, "running");
}

static void stop_machine(slot *s, int timeout_ms)
{
	stop_job *job = malloc(sizeof *job);
	pthread_t thread;

	/* A stop is not held up by a run that is not reading: the engine's
	 * reader takes the rest, so the machine can end. */
	release_hold(s);

	wake_machine(s);

	if (job == NULL || s->exited || s->handle == NULL) {
		free(job);
		return;
	}
	*job = (stop_job){ s->engine, &s->core, s->handle, timeout_ms };
	if (pthread_create(&thread, NULL, run_stop, job) != 0) {
		free(job);
		return;
	}
	pthread_detach(thread);
}

static void forget_machine(slot *s);
static void forget_secrets(slot *s);
static void forget_ssh(slot *s);

/* The machine has ended: tell whoever is waiting, and let the engine go. */
static void machine_exited(slot *s, int code, int signal)
{
	hull_error error = { .struct_size = sizeof error };

	forget_machine(s);
	s->exited = 1;
	s->code   = code;
	s->signal = signal;
	snprintf(s->state, sizeof s->state, "exited");
	/* Its followers hear the exit once they have read to the end. */
	for (unsigned i = 0u; i < s->stop_count; i++)
		send_simple(s->stops[i].client, "(4:done", s->stops[i].ch);
	s->stop_count     = 0u;
	s->owner          = NULL;
	/* Its commands end with it; their exits are queued, and seen before
	 * this drain is over. */
	for (unsigned i = 0u; i < MAX_EXECS; i++)
		if (execs[i].used && execs[i].machine == s) {
			execs[i].tell |= !execs[i].closed;
			close_exec(&execs[i]);
		}
	if (s->handle != NULL) {
		s->engine->ops->destroy(&s->core, s->handle, &error);
		s->handle = NULL;
	}
	forget_secrets(s);
	forget_ssh(s);
}

static void drain_list(event *e);
static void give_credit(void);

/* An event about one of a machine's commands. */
static void exec_event(const event *e)
{
	exec_entry *x = exec_of(e->stream);

	if (x == NULL)
		return;
	if (e->kind == EVENT_DATA && !x->closed) {
		send_data(x->client, x->ch, e->fd, e->bytes, e->length);
	} else if (e->kind == EVENT_EXIT) {
		/* A frontend that has gone hears nothing; one whose command went
		 * with its machine hears how it ended. */
		if (!x->closed || x->tell)
			send_status(x->client, x->ch, e->code, e->signal);
		close_exec(x);
		pthread_mutex_lock(&flow_lock);
		x->used = 0;
		pthread_mutex_unlock(&flow_lock);
	}
}

/* Everything queued, to the end, including what handling it queues. */
static void drain_events(void)
{
	char bytes[64];

	while (read(wake[0], bytes, sizeof bytes) > 0)
		;
	for (;;) {
		event *e;

		pthread_mutex_lock(&queue_lock);
		e = queue_head;
		queue_head = queue_tail = NULL;
		pthread_mutex_unlock(&queue_lock);
		if (e == NULL) {
			give_credit();
			return;
		}
		drain_list(e);
	}
}

static void drain_list(event *e)
{
	while (e != NULL) {
		event *next = e->next;
		slot  *s    = e->slot;

		if (e->kind == EVENT_GC || e->kind == EVENT_GC_DONE) {
			client *c = &clients[e->client];

			if (c->used && client_generation[e->client] == e->client_generation) {
				if (e->kind == EVENT_GC)
					send_data(c, e->ch, e->fd, e->bytes, e->length);
				else
					send_simple(c, "(4:done", e->ch);
			}
			free(e);
			e = next;
			continue;
		}
		if (e->stream != NULL) {
			exec_event(e);
			free(e);
			e = next;
			continue;
		}

		/* An engine's thread can report a last state after the exit, by
		 * when the slot may hold another machine: that is not news. */
		if (s->generation != e->generation) {
			free(e);
			e = next;
			continue;
		}
		switch (e->kind) {
		case EVENT_DATA:
			keep_log(s, e->fd, e->bytes, e->length);  /* its followers read it there */
			break;
		case EVENT_EXIT:
			machine_exited(s, e->code, e->signal);
			break;
		case EVENT_STATE:
			if (!s->exited)
				snprintf(s->state, sizeof s->state, "%s", e->state);
			break;
		case EVENT_GC:
		case EVENT_GC_DONE:
			break;  /* answered above */
		}
		free(e);
		e = next;
	}
}

/* --- requests ------------------------------------------------------------ */

/* A machine id is a DNS label (section 4.10), as names are (J.1). */
static int is_label(const char *id)
{
	size_t n = strlen(id);

	if (n == 0u || n > 63u || id[0] == '-' || id[n - 1u] == '-')
		return 0;
	for (size_t i = 0u; i < n; i++)
		if (!(id[i] >= 'a' && id[i] <= 'z') && !(id[i] >= '0' && id[i] <= '9') && id[i] != '-')
			return 0;
	return 1;
}

/* A run's machine, or an exec's command, is running: what the frontend
 * is refused after this is a resize or a signal, not the request. */
static void send_started(client *c, uint32_t ch, const slot *s)
{
	hull_msg m = { 0 };

	hull_msg_raw(&m, "(7:started");
	hull_msg_channel(&m, ch);
	hull_msg_raw(&m, "(7:machine");
	hull_msg_text(&m, s->id);
	hull_msg_raw(&m, "))");
	send_to(c, &m);
}

/*
 * A machine's stable address (section 8.1): 127.107.x.y from its id
 * alone, so it is the same on every start and every engine. FNV-1a of
 * the id's bytes, taken modulo 256 * 254, is x * 254 + y - 1, which keeps
 * y from 1 to 254. Changing this changes every machine's address.
 */
#define ADDRESSES (256u * 254u)

static uint32_t address_slot(const char *id)
{
	uint32_t h = 2166136261u;

	for (const unsigned char *p = (const unsigned char *)id; *p != '\0'; p++)
		h = (h ^ *p) * 16777619u;
	return h % ADDRESSES;
}

static void address_at(uint32_t slot_number, char out[16])
{
	snprintf(out, 16, "127.107.%u.%u", slot_number / 254u, slot_number % 254u + 1u);
}

/* The address `id` is given when it starts or is adopted: the one kept
 * for it, then its own, and else the next free after its own, kept so
 * that it has it again. */
static void address_of(const char *id, char out[16]);

/*
 * The plan the engine gets is the frontend's with the address added. It
 * sorts before every other entry of a plan, so it goes first. A plan that
 * names one already is refused: the address is the core's to give.
 */
static uint8_t *with_address(const hull_buf *plan, const char *address, size_t *length)
{
	static const char opening[] = "(4:plan";
	size_t            n = strlen(address);
	char              entry[40];
	int               written;
	uint8_t          *out;

	if (plan->len < sizeof opening - 1u || memcmp(plan->data, opening, sizeof opening - 1u) != 0)
		return NULL;
	written = snprintf(entry, sizeof entry, "(7:address%zu:%s)", n, address);
	if (written < 0 || (size_t)written >= sizeof entry)
		return NULL;
	*length = plan->len + (size_t)written;
	out = malloc(*length);
	if (out == NULL)
		return NULL;
	memcpy(out, opening, sizeof opening - 1u);
	memcpy(out + sizeof opening - 1u, entry, (size_t)written);
	memcpy(out + sizeof opening - 1u + (size_t)written, plan->data + sizeof opening - 1u,
	       plan->len - (sizeof opening - 1u));
	return out;
}

/* --- secrets ------------------------------------------------------------- */

/*
 * J.3's secrets, as the frontend resolved them: a file each, mode 0400,
 * in hull/secrets/ID beside the socket, whose path the engine is given as
 * the plan's `(secrets DIR)` and binds read-only at /run/hull/secrets.
 * They are kept in memory only, so that directory must be on a tmpfs or a
 * ramfs, or the run is refused. They go with the machine's exit, and a
 * core that starts removes those of every machine it does not hold.
 */
static char secrets_root[4200];

static int secrets_path(const char *id, char *out, size_t cap)
{
	int n = snprintf(out, cap, "%s/%s", secrets_root, id);

	return secrets_root[0] != '\0' && n > 0 && (size_t)n < cap;
}

/* The directory of `id`'s secrets, and each file in it: the core made it,
 * so what is in it is the core's. */
static void remove_secrets(const char *id)
{
	char           path[4300];
	DIR           *dir;
	struct dirent *entry;

	if (!secrets_path(id, path, sizeof path) || (dir = opendir(path)) == NULL)
		return;
	while ((entry = readdir(dir)) != NULL)
		if (strcmp(entry->d_name, ".") != 0 && strcmp(entry->d_name, "..") != 0)
			unlinkat(dirfd(dir), entry->d_name, 0);
	closedir(dir);
	rmdir(path);
}

/* The secrets the plan's machine names, from the values in the run: each
 * one given, once, and no other. Written to a fresh directory, whose path
 * is put in `dir`. NULL, or why not. */
static const char *write_secrets(const char *id, const hull_sexpr_doc *plan_doc,
                                 const hull_sexpr_form *wanted, const hull_sexpr_doc *body,
                                 char *dir, size_t cap)
{
	hull_sexpr_form given;
	hull_sexpr_form item;
	hull_sexpr_form name;
	hull_sexpr_form value;
	struct statfs   where;
	unsigned        named = 0u;
	unsigned        sent  = 0u;
	int             more;
	int             at;

	/* The entry, `(secrets (NAME VALUE) ...)`, and its first pair. */
	if (!hull_msg_find(body, "secrets", &given) || !hull_sexpr_child(body, &given, &item) ||
	    !hull_sexpr_next(body, &item, &given))
		return "the run has none of its secrets' values";
	item = given;
	for (more = 1; more; more = hull_sexpr_next(body, &item, &item))
		sent++;
	if (statfs(secrets_root, &where) != 0 ||
	    (where.f_type != TMPFS_MAGIC && where.f_type != RAMFS_MAGIC))
		return "secrets are kept in memory only, and the runtime directory is not a tmpfs";
	if (!secrets_path(id, dir, cap))
		return "the secrets' directory's path is too long";
	remove_secrets(id);
	if (mkdir(dir, 0700) != 0 || (at = open(dir, O_PATH | O_DIRECTORY | O_CLOEXEC)) < 0)
		return "the secrets' directory cannot be made";
	hull_sexpr_child(plan_doc, wanted, &item);
	for (more = hull_sexpr_next(plan_doc, &item, &item); more;
	     more = hull_sexpr_next(plan_doc, &item, &item)) {
		hull_sexpr_form candidate = given;
		hull_sexpr_form key;
		const char     *why = "a secret the plan names has no value";
		char            file[256];
		int             found = 0;
		int             fd;

		named++;
		if (!hull_sexpr_child(plan_doc, &item, &name) || name.kind != HULL_SEXPR_ATOM ||
		    name.content_length >= sizeof file) {
			why = "the plan's secrets are not a plan's";
			found = -1;
		}
		do {
			if (found >= 0 && candidate.kind == HULL_SEXPR_LIST &&
			    hull_sexpr_child(body, &candidate, &key) && key.kind == HULL_SEXPR_ATOM &&
			    key.content_length == name.content_length &&
			    memcmp(body->data + key.content, plan_doc->data + name.content,
			           name.content_length) == 0 &&
			    hull_sexpr_next(body, &key, &value) && value.kind == HULL_SEXPR_ATOM)
				found++;
		} while (hull_sexpr_next(body, &candidate, &candidate));
		if (found == 1) {
			/* The name passed the spec's check: letters, digits, `_` and
			 * `-`, at most 255, so it is one file in the directory. */
			memcpy(file, plan_doc->data + name.content, name.content_length);
			file[name.content_length] = '\0';
			fd = openat(at, file, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0400);
			if (fd >= 0 && write(fd, body->data + value.content, value.content_length) ==
			                   (ssize_t)value.content_length)
				why = NULL;
			else
				why = "a secret cannot be written";
			if (fd >= 0 && close(fd) != 0)
				why = "a secret cannot be written";
		} else if (found > 1) {
			why = "a secret's value is given twice";
		}
		if (why != NULL) {
			close(at);
			remove_secrets(id);
			return why;
		}
	}
	close(at);
	if (named != sent) {
		remove_secrets(id);
		return "the run gives a secret the plan does not name";
	}
	return NULL;
}

/* The plan with `(secrets DIR)` added: "secrets" sorts after every key a
 * plan has, so it goes last. */
static uint8_t *with_secrets(const hull_buf *plan, const char *dir, size_t *length)
{
	char     entry[4400];
	int      written = snprintf(entry, sizeof entry, "(7:secrets%zu:%s)", strlen(dir), dir);
	uint8_t *out;

	if (written < 0 || (size_t)written >= sizeof entry || plan->len == 0u ||
	    plan->data[plan->len - 1u] != ')')
		return NULL;
	*length = plan->len + (size_t)written;
	out = malloc(*length);
	if (out == NULL)
		return NULL;
	memcpy(out, plan->data, plan->len - 1u);
	memcpy(out + plan->len - 1u, entry, (size_t)written);
	out[*length - 1u] = ')';
	return out;
}

static void forget_secrets(slot *s)
{
	if (s->secrets)
		remove_secrets(s->id);
	s->secrets = 0;
}

/* --- (ssh true) provisioning --------------------------------------------- */

/*
 * Section 8.1's item 3, the image's own sshd: for a machine with (ssh true),
 * the core generates a host key, reads the user's public keys as the
 * authorized_keys, and writes a minimal sshd_config, into hull/ssh/ID beside
 * the socket. The engine is given that path as the plan's `(ssh_dir DIR)`
 * and the agent places the three files on a tmpfs the image's sshd reads by
 * absolute path. The host key is private, so this directory is a tmpfs like
 * secrets', and goes with the machine. The host key itself is machine state,
 * generated once per id under hull/machines and reused, so a machine's SSH
 * identity is stable across runs (write_host_key); only ephemeral where no
 * data home says where to keep it.
 */
static char ssh_root[4200];

/* `path` and the directories above it, each 0700 where it is made: 0, or
 * -1 where one cannot be. */
static int make_dirs(const char *path)
{
	char  copy[4096];
	char *at;

	if ((size_t)snprintf(copy, sizeof copy, "%s", path) >= sizeof copy)
		return -1;
	for (at = copy + 1; *at != '\0'; at++)
		if (*at == '/') {
			*at = '\0';
			if (mkdir(copy, 0700) != 0 && errno != EEXIST)
				return -1;
			*at = '/';
		}
	return mkdir(copy, 0700) != 0 && errno != EEXIST ? -1 : 0;
}

/* The persistent per-machine host keys live here, keyed by the stable id,
 * so a machine's SSH identity survives a restart and (once more than qemu
 * provisions) a backend switch, as section 8.1 item 2 asks. Empty when no
 * data home is set, and then the host key is ephemeral, generated per run. */
static char host_key_root[4200];

static int ssh_path(const char *id, char *out, size_t cap)
{
	int n = snprintf(out, cap, "%s/%s", ssh_root, id);

	return ssh_root[0] != '\0' && n > 0 && (size_t)n < cap;
}

static void remove_ssh(const char *id)
{
	char           path[4300];
	DIR           *dir;
	struct dirent *entry;

	if (!ssh_path(id, path, sizeof path) || (dir = opendir(path)) == NULL)
		return;
	while ((entry = readdir(dir)) != NULL)
		if (strcmp(entry->d_name, ".") != 0 && strcmp(entry->d_name, "..") != 0)
			unlinkat(dirfd(dir), entry->d_name, 0);
	closedir(dir);
	rmdir(path);
}

/* The user's public keys, concatenated, as the machine's authorized_keys:
 * every `.pub` in ~/.ssh, so the key a `hull ssh` or plain ssh offers is
 * authorized. Written to `at`/authorized_keys, 0400. NULL, or why not. */
static const char *write_authorized_keys(int at)
{
	const char    *home = getenv("HOME");
	char           dir[4096];
	DIR           *d;
	struct dirent *entry;
	int            out;
	int            wrote = 0;

	if (home == NULL || home[0] != '/')
		return "(ssh true): HOME is not set, so there is no public key to authorize";
	if ((size_t)snprintf(dir, sizeof dir, "%s/.ssh", home) >= sizeof dir)
		return "(ssh true): the path to ~/.ssh is too long";
	out = openat(at, "authorized_keys", O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0400);
	if (out < 0)
		return "(ssh true): the authorized_keys file cannot be made";
	if ((d = opendir(dir)) != NULL) {
		int dfd = dirfd(d);

		while ((entry = readdir(d)) != NULL) {
			size_t len = strlen(entry->d_name);
			char   buf[8192];
			int    in;
			ssize_t n;

			if (len < 4u || strcmp(entry->d_name + len - 4u, ".pub") != 0)
				continue;
			if ((in = openat(dfd, entry->d_name, O_RDONLY | O_CLOEXEC | O_NOFOLLOW)) < 0)
				continue;
			while ((n = read(in, buf, sizeof buf)) > 0)
				if (write(out, buf, (size_t)n) == n)
					wrote = 1;
			close(in);
		}
		closedir(d);
	}
	close(out);
	if (!wrote)
		return "(ssh true): no public key in ~/.ssh to authorize; make one with ssh-keygen";
	return NULL;
}

/* An ephemeral ed25519 host key, by the host's own ssh-keygen, into
 * `at`/host_key. NULL, or why not. */
/* An ed25519 key pair by the host's own ssh-keygen, into `path`. */
static const char *keygen(const char *path)
{
	pid_t child = fork();
	int   status;

	if (child < 0)
		return "(ssh true): the host key could not be generated";
	if (child == 0) {
		int null = open("/dev/null", O_WRONLY);

		if (null >= 0) {
			dup2(null, 1);
			dup2(null, 2);
		}
		execlp("ssh-keygen", "ssh-keygen", "-q", "-t", "ed25519", "-f", path, "-N", "",
		       "-C", "hull", (char *)NULL);
		_exit(127);
	}
	if (waitpid(child, &status, 0) != child || !WIFEXITED(status) || WEXITSTATUS(status) != 0)
		return "(ssh true): ssh-keygen could not generate a host key; is it installed?";
	return NULL;
}

/* Copy the private key bytes from `from` to `to`, mode 0400. */
static const char *copy_key(const char *from, const char *to)
{
	char    buf[8192];
	ssize_t n;
	int     in = open(from, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
	int     out = -1;

	if (in >= 0)
		out = open(to, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0400);
	if (in < 0 || out < 0) {
		if (in >= 0)
			close(in);
		return "(ssh true): the host key could not be read back";
	}
	while ((n = read(in, buf, sizeof buf)) > 0)
		if (write(out, buf, (size_t)n) != n)
			n = -1;
	close(in);
	if (close(out) != 0 || n < 0)
		return "(ssh true): the host key could not be read back";
	return NULL;
}

/* The host key for `id`, into `dir`/host_key for delivery. Where a data
 * home is known it is machine state: generated once into
 * hull/machines/ID/host_key and reused, so the machine's SSH identity is
 * stable across runs; the private key is 0600 there and copied 0400 into
 * the per-run tmpfs. Without a data home it is ephemeral, generated per
 * run straight into `dir`. */
static const char *write_host_key(const char *id, const char *dir)
{
	char per_run[4300];
	char persist_dir[4200];
	char persist[4300];

	if ((size_t)snprintf(per_run, sizeof per_run, "%s/host_key", dir) >= sizeof per_run)
		return "(ssh true): the host key's path is too long";
	if (host_key_root[0] == '\0')
		return keygen(per_run);
	if ((size_t)snprintf(persist_dir, sizeof persist_dir, "%s/%s", host_key_root, id) >=
	        sizeof persist_dir ||
	    (size_t)snprintf(persist, sizeof persist, "%s/host_key", persist_dir) >= sizeof persist)
		return "(ssh true): the host key's path is too long";
	if (access(persist, F_OK) != 0) {
		const char *why;

		if ((mkdir(host_key_root, 0700) != 0 && errno != EEXIST) ||
		    (mkdir(persist_dir, 0700) != 0 && errno != EEXIST))
			return "(ssh true): the machine's key directory cannot be made";
		if ((why = keygen(persist)) != NULL)
			return why;
	}
	return copy_key(persist, per_run);
}

/* The ssh material for `id`, into a fresh hull/ssh/ID: a host key, the
 * user's authorized_keys, and a config that points the image's sshd at
 * both. Its path is put in `dir`. NULL, or why not. */
static const char *write_ssh(const char *id, char *dir, size_t cap)
{
	static const char config[] =
	    "Port 22\n"
	    "HostKey /run/hull/ssh/host_key\n"
	    "AuthorizedKeysFile /run/hull/ssh/authorized_keys\n"
	    "PasswordAuthentication no\n"
	    "PermitRootLogin prohibit-password\n"
	    "UsePAM no\n";
	struct statfs where;
	const char   *why;
	int           at;
	int           fd;

	if (ssh_root[0] == '\0')
		return "(ssh true): the runtime directory has no place for ssh material";
	if (statfs(ssh_root, &where) != 0 ||
	    (where.f_type != TMPFS_MAGIC && where.f_type != RAMFS_MAGIC))
		return "(ssh true): the host key is kept in memory only, and the runtime directory is "
		       "not a tmpfs";
	if (!ssh_path(id, dir, cap))
		return "(ssh true): the ssh directory's path is too long";
	remove_ssh(id);
	if (mkdir(dir, 0700) != 0 || (at = open(dir, O_PATH | O_DIRECTORY | O_CLOEXEC)) < 0)
		return "(ssh true): the ssh directory cannot be made";
	why = write_host_key(id, dir);
	if (why == NULL)
		why = write_authorized_keys(at);
	if (why == NULL) {
		fd = openat(at, "config", O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0400);
		if (fd < 0 || write(fd, config, sizeof config - 1u) != (ssize_t)(sizeof config - 1u))
			why = "(ssh true): the sshd config cannot be written";
		if (fd >= 0)
			close(fd);
	}
	close(at);
	if (why != NULL)
		remove_ssh(id);
	return why;
}

/* The plan with `(ssh_dir DIR)` added: "ssh_dir" sorts after "secrets" and
 * every other key a plan has, so it goes last, as with_secrets' does. */
static uint8_t *with_ssh(const hull_buf *plan, const char *dir, size_t *length)
{
	char     entry[4400];
	int      written = snprintf(entry, sizeof entry, "(7:ssh_dir%zu:%s)", strlen(dir), dir);
	uint8_t *out;

	if (written < 0 || (size_t)written >= sizeof entry || plan->len == 0u ||
	    plan->data[plan->len - 1u] != ')')
		return NULL;
	*length = plan->len + (size_t)written;
	out = malloc(*length);
	if (out == NULL)
		return NULL;
	memcpy(out, plan->data, plan->len - 1u);
	memcpy(out + plan->len - 1u, entry, (size_t)written);
	out[*length - 1u] = ')';
	return out;
}

static void forget_ssh(slot *s)
{
	if (s->ssh)
		remove_ssh(s->id);
	s->ssh = 0;
}

/* --- machines that outlive the core -------------------------------------- */

/*
 * hull/machines/ID, for each machine under (on_disconnect keep) whose
 * engine can take one back: what a core that comes after this one needs,
 * should this one go without stopping it, killed or crashed.
 *
 *     (4:kept (7:machine ID) (6:engine NAME) (5:class CLASS) (5:state S))
 *
 * canonical, S the engine's persist(). It is written whole, by a rename,
 * once the machine has started, and removed once it has exited. A core
 * reads them all as it starts, holding the lock, and adopts each machine;
 * one its engine says is gone, or is not one, is forgotten, and one it
 * could not reach for now is left for the next core.
 */
#define KEPT_LIMIT 65536u

static char kept_dir[4200];

static int kept_path(const char *id, char *out, size_t cap, const char *suffix)
{
	int n = snprintf(out, cap, "%s/%s%s", kept_dir, id, suffix);

	return kept_dir[0] != '\0' && n > 0 && (size_t)n < cap;
}

static void keep_machine(slot *s)
{
	hull_msg   m = { 0 };
	hull_buf   state = { 0 };
	hull_error error = { .struct_size = sizeof error };
	char       path[4300];
	char       fresh[4300];
	int        fd;

	if (!s->keep || s->engine->ops->adopt == NULL || s->engine->ops->persist == NULL ||
	    !kept_path(s->id, path, sizeof path, "") || !kept_path(s->id, fresh, sizeof fresh, ".new") ||
	    s->engine->ops->persist(&s->core, s->handle, &state, &error) != HULL_OK)
		return;
	hull_msg_raw(&m, "(4:kept(7:machine");
	hull_msg_text(&m, s->id);
	hull_msg_raw(&m, ")(6:engine");
	hull_msg_text(&m, s->engine->caps->engine_name);
	hull_msg_raw(&m, ")(5:class");
	hull_msg_text(&m, s->class_name);
	hull_msg_raw(&m, ")(5:state");
	hull_msg_atom(&m, state.data, state.len);
	hull_msg_raw(&m, "))");
	fd = m.failed ? -1 : open(fresh, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0600);
	if (fd >= 0 && write(fd, m.data, m.length) == (ssize_t)m.length && close(fd) == 0 &&
	    rename(fresh, path) == 0) {
		s->kept = 1;
	} else {
		if (fd >= 0)
			close(fd);
		unlink(fresh);
		fprintf(stderr, "hull core: %s cannot be kept for the next core\n", path);
	}
	hull_msg_free(&m);
}

static void forget_machine(slot *s)
{
	char path[4300];

	if (s->kept && kept_path(s->id, path, sizeof path, ""))
		unlink(path);
	s->kept = 0;
}

/* The engine named, if this core has it and it can adopt. */
static const hull_engine *adopter(const char *name)
{
	for (uint32_t i = 0u; i < registry.count; i++)
		if (strcmp(registry.engines[i]->caps->engine_name, name) == 0)
			return registry.engines[i]->ops->adopt != NULL ? registry.engines[i] : NULL;
	return NULL;
}

/* Take back the machine kept as `id`: 1 if it is held now. */
static int adopt_one(const char *id)
{
	static uint8_t     data[KEPT_LIMIT];
	char               path[4300];
	char               engine_name[32];
	char               class_name[sizeof machines[0].class_name];
	char               named[64];
	const char        *why = NULL;
	const hull_engine *engine = NULL;
	hull_sexpr_doc     doc;
	hull_sexpr_form    value;
	hull_error         error = { .struct_size = sizeof error };
	hull_buf           state;
	hull_status        status;
	slot              *s = NULL;
	ssize_t            n;
	int                fd;

	if (!kept_path(id, path, sizeof path, ""))
		return 0;
	fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
	n  = fd >= 0 ? read(fd, data, sizeof data) : -1;
	if (fd >= 0)
		close(fd);
	if (n <= 0 || (size_t)n == sizeof data ||
	    hull_sexpr_open(&doc, data, (size_t)n, HULL_SEXPR_CANONICAL, NULL) != HULL_SEXPR_OK ||
	    !hull_msg_is(&doc, "kept") || !hull_msg_find(&doc, "machine", &value) ||
	    !hull_msg_string(&doc, &value, named, sizeof named) || strcmp(named, id) != 0 ||
	    !hull_msg_find(&doc, "engine", &value) ||
	    !hull_msg_string(&doc, &value, engine_name, sizeof engine_name) ||
	    !hull_msg_find(&doc, "class", &value) ||
	    !hull_msg_string(&doc, &value, class_name, sizeof class_name) ||
	    !hull_msg_find(&doc, "state", &value) || value.kind != HULL_SEXPR_ATOM)
		why = "it is not what a core writes";
	else if ((engine = adopter(engine_name)) == NULL)
		why = "its engine cannot take it back";
	for (unsigned i = 0u; why == NULL && i < MAX_MACHINES && s == NULL; i++)
		if (!machines[i].used)
			s = &machines[i];
	if (why == NULL && s == NULL) {
		fprintf(stderr, "hull core: %s: the core holds as many machines as it can\n", id);
		return 0;
	}
	if (why != NULL) {
		fprintf(stderr, "hull core: machine %s is forgotten: %s\n", id, why);
		unlink(path);
		return 0;
	}
	state.data = data + value.content;
	state.len  = value.content_length;
	s->used            = 1;
	s->generation++;
	s->engine          = engine;
	s->core.slot       = s;
	s->core.generation = s->generation;
	s->keep            = 1;
	s->kept            = 1;
	snprintf(s->id, sizeof s->id, "%.63s", id);  /* a label, so whole */
	address_of(id, s->address);
	snprintf(s->class_name, sizeof s->class_name, "%s", class_name);
	snprintf(s->state, sizeof s->state, "adopting");
	status = engine->ops->adopt(&s->core, state, &s->handle, &error);
	if (status == HULL_OK) {
		char dir[4300];

		/* Its secrets are still bound, and go with it as they would have. */
		s->secrets = secrets_path(id, dir, sizeof dir) && access(dir, F_OK) == 0;
		fprintf(stderr, "hull core: machine %s is taken back\n", id);
		return 1;
	}
	why = error.detail != NULL ? error.detail : "adopt failed";
	s->used = 0;
	s->kept = 0;
	s->handle = NULL;
	if (status == HULL_ERR_GONE || status == HULL_ERR_UNSUPPORTED) {
		fprintf(stderr, "hull core: machine %s is forgotten: %s\n", id, why);
		unlink(path);
	} else {
		fprintf(stderr, "hull core: machine %s is left for the next core: %s\n", id, why);
	}
	return 0;
}

/* The secrets of machines a core that went held and this one does not:
 * one that died with it, or one not taken back. */
static void remove_stale_secrets(void)
{
	DIR           *dir = opendir(secrets_root);
	struct dirent *entry;

	while (dir != NULL && (entry = readdir(dir)) != NULL)
		if (is_label(entry->d_name) && find_machine(entry->d_name) == NULL)
			remove_secrets(entry->d_name);
	if (dir != NULL)
		closedir(dir);
}

static void adopt_kept(void)
{
	DIR           *dir = opendir(kept_dir);
	struct dirent *entry;

	/* An id is a label, which neither `.`, `..` nor a half-written
	 * ID.new is; that is a core's that went while writing, and this one
	 * holds the lock, so it is removed. */
	while (dir != NULL && (entry = readdir(dir)) != NULL) {
		size_t n = strlen(entry->d_name);

		if (is_label(entry->d_name))
			adopt_one(entry->d_name);
		else if (n > 4u && strcmp(entry->d_name + n - 4u, ".new") == 0)
			unlinkat(dirfd(dir), entry->d_name, 0);
	}
	if (dir != NULL)
		closedir(dir);
}

static const char *impairment_of(const hull_sexpr_doc *body, const hull_sexpr_form *entry,
                                 hull_impairment *m);
static const char *tamper_of(const hull_sexpr_doc *body, const hull_sexpr_form *entry,
                             hull_tamper *t);

/* The plan's (impair ...) and (tamper ...) into `s`: NULL, or why it
 * cannot be. A segment the machine is not on is refused. */
static const char *links_of(slot *s, const hull_sexpr_doc *doc, const hull_sexpr_form *machine)
{
	static const char *const which[2] = { "impair", "tamper" };
	hull_sexpr_form          segments;
	int                      has_segments = hull_access_find(doc, machine, "segments", &segments);

	for (int kind = 0; kind < 2; kind++) {
		hull_sexpr_form group;
		hull_sexpr_form link;

		if (!hull_access_find(doc, machine, which[kind], &group))
			continue;
		hull_sexpr_child(doc, &group, &link);
		while (hull_sexpr_next(doc, &link, &link)) {
			hull_sexpr_form name;
			hull_sexpr_form way;
			hull_sexpr_form joined;
			char            segment[64];
			unsigned        at;
			int             on = 0;
			size_t          length;

			if (link.kind != HULL_SEXPR_LIST || !hull_sexpr_child(doc, &link, &name) ||
			    hull_sexpr_value(doc, &name, (uint8_t *)segment, sizeof segment - 1u,
			                     &length) != HULL_SEXPR_OK)
				return "impair and tamper are (SEGMENT (out ...) (in ...))";
			segment[length] = '\0';
			if (has_segments) {
				hull_sexpr_child(doc, &segments, &joined);
				while (!on && hull_sexpr_next(doc, &joined, &joined)) {
					hull_sexpr_form named = joined;

					/* A segment hull addresses is (NAME (subnet ...)). */
					if (joined.kind == HULL_SEXPR_LIST)
						hull_sexpr_child(doc, &joined, &named);
					on = hull_sexpr_is(doc, &named, segment);
				}
			}
			if (!on)
				return kind == 0 ? "impair names a segment the machine is not on"
				                 : "tamper names a segment the machine is not on";
			/* One slot per segment, shared by its impair and tamper. */
			for (at = 0u; at < s->link_count && strcmp(s->links[at].segment, segment) != 0; at++)
				;
			if (at == s->link_count) {
				if (s->link_count == sizeof s->links / sizeof s->links[0])
					return "more segments impaired than a machine joins";
				snprintf(s->links[at].segment, sizeof s->links[at].segment, "%s", segment);
				s->link_count++;
			}
			for (way = name; hull_sexpr_next(doc, &way, &way);) {
				hull_sexpr_form head;
				const char     *wrong;
				int             direction;

				if (way.kind != HULL_SEXPR_LIST || !hull_sexpr_child(doc, &way, &head))
					return "impair and tamper are (SEGMENT (out ...) (in ...))";
				direction = hull_sexpr_is(doc, &head, "in") ? HULL_SEGMENT_IN : HULL_SEGMENT_OUT;
				if (kind == 0) {
					if ((wrong = impairment_of(doc, &way, &s->links[at].way[direction])) != NULL)
						return wrong;
					s->links[at].given[direction] = 1;
				} else {
					if ((wrong = tamper_of(doc, &way, &s->links[at].fault[direction])) != NULL)
						return wrong;
					s->links[at].tampered[direction] = 1;
				}
			}
		}
	}
	return NULL;
}

/* The `(hosts (ID A) ...)` the plan's `names` gives `segment`, onto the
 * heap: 0, or -1 where one cannot be read or memory ran out. */
static int hosts_of(const hull_sexpr_doc *doc, const hull_sexpr_form *names, const char *segment,
                    hull_segment_host **out, unsigned *count)
{
	hull_sexpr_form entry;
	hull_sexpr_form hosts;
	hull_sexpr_form host;
	unsigned        n = 0u;
	int             found = 0;

	*out   = NULL;
	*count = 0u;
	hull_sexpr_child(doc, names, &entry);
	while (!found && hull_sexpr_next(doc, &entry, &entry)) {
		hull_sexpr_form head;
		char           *name;

		hull_sexpr_child(doc, &entry, &head);
		name = hull_access_text(doc, &head);
		found = name != NULL && strcmp(name, segment) == 0;
		free(name);
	}
	if (!found || !hull_access_find(doc, &entry, "hosts", &hosts))
		return 0;
	hull_sexpr_child(doc, &hosts, &host);
	for (hull_sexpr_form h = host; hull_sexpr_next(doc, &h, &h);)
		n++;
	if (n == 0u || (*out = calloc(n, sizeof **out)) == NULL)
		return n == 0u ? 0 : -1;
	while (hull_sexpr_next(doc, &host, &host)) {
		hull_sexpr_form id_form;
		hull_sexpr_form address_form;
		char           *id = NULL;
		char           *address = NULL;
		struct in_addr  parsed;
		int             ok;

		if (hull_sexpr_child(doc, &host, &id_form) &&
		    hull_sexpr_next(doc, &id_form, &address_form)) {
			id      = hull_access_text(doc, &id_form);
			address = hull_access_text(doc, &address_form);
		}
		ok = id != NULL && address != NULL && strlen(id) < sizeof (*out)->id &&
		     inet_pton(AF_INET, address, &parsed) == 1;
		if (ok) {
			snprintf((*out)[*count].id, sizeof (*out)->id, "%s", id);
			(*out)[(*count)++].address = ntohl(parsed.s_addr);
		}
		free(id);
		free(address);
		if (!ok)
			return -1;
	}
	/* And each one's IPv6 address, where the segment has an IPv6 network,
	 * matched by its id. */
	if (hull_access_find(doc, &entry, "hosts6", &hosts) && hull_sexpr_child(doc, &hosts, &host))
		while (hull_sexpr_next(doc, &host, &host)) {
			hull_sexpr_form id_form;
			hull_sexpr_form address_form;
			char           *id = NULL;
			char           *address = NULL;
			struct in6_addr parsed;
			int             ok;

			if (hull_sexpr_child(doc, &host, &id_form) &&
			    hull_sexpr_next(doc, &id_form, &address_form)) {
				id      = hull_access_text(doc, &id_form);
				address = hull_access_text(doc, &address_form);
			}
			ok = id != NULL && address != NULL && inet_pton(AF_INET6, address, &parsed) == 1;
			for (unsigned k = 0u; ok && k < *count; k++)
				if (strcmp((*out)[k].id, id) == 0) {
					(*out)[k].has6 = 1;
					memcpy((*out)[k].address6, &parsed, 16u);
				}
			free(id);
			free(address);
			if (!ok)
				return -1;
		}
	return 0;
}

/* The plan's (names ...) into `s`, for each segment the machine is on:
 * NULL, or why it cannot be read. */
static const char *named_of(slot *s, const hull_sexpr_doc *doc, const hull_sexpr_form *machine)
{
	hull_sexpr_form top;
	hull_sexpr_form names;
	hull_sexpr_form segments;
	hull_sexpr_form item;
	char           *network = hull_access_value(doc, machine, "network");

	s->forwards = network == NULL || strcmp(network, "none") != 0;
	free(network);
	hull_sexpr_first(doc, &top);
	if (!hull_access_find(doc, &top, "names", &names) ||
	    !hull_access_find(doc, machine, "segments", &segments))
		return NULL;
	hull_sexpr_child(doc, &segments, &item);
	while (hull_sexpr_next(doc, &item, &item)) {
		char          *segment = hull_access_segment_name(doc, &item);
		char           address[16];
		unsigned       prefix;
		struct in_addr parsed;

		if (segment == NULL)
			return "a segment is a name";
		if (!hull_access_named(doc, &names, segment, address, &prefix)) {
			free(segment);
			continue;
		}
		if (s->named_count == sizeof s->named / sizeof s->named[0] ||
		    strlen(segment) >= sizeof s->named[0].segment ||
		    inet_pton(AF_INET, address, &parsed) != 1 || prefix == 0u || prefix > 32u) {
			free(segment);
			return "the plan's names cannot be read";
		}
		snprintf(s->named[s->named_count].segment, sizeof s->named[0].segment, "%s", segment);
		s->named[s->named_count].address = ntohl(parsed.s_addr);
		s->named[s->named_count].prefix  = prefix;
		if (hosts_of(doc, &names, segment, &s->named[s->named_count].hosts,
		             &s->named[s->named_count].host_count) != 0) {
			free(s->named[s->named_count].hosts);
			free(segment);
			return "the plan's names cannot be read";
		}
		s->named_count++;
		free(segment);
	}
	return NULL;
}

/* Where `id`'s address is kept when a clash moved it: hull/machines/ID/
 * address, beside its host key. 0 where no data home says where. */
static int address_path(const char *id, char *out, size_t cap)
{
	return host_key_root[0] != '\0' &&
	       (size_t)snprintf(out, cap, "%s/%s/address", host_key_root, id) < cap;
}

/* The address kept for `id`, into `out`: 1, or 0 where none is. */
static int address_kept(const char *id, char out[16])
{
	char     path[4300];
	char     text[32] = "";
	unsigned x;
	unsigned y;
	char     tail;
	FILE    *file;

	if (!address_path(id, path, sizeof path) || (file = fopen(path, "re")) == NULL)
		return 0;
	if (fgets(text, sizeof text, file) == NULL)
		text[0] = '\0';
	fclose(file);
	if (sscanf(text, "127.107.%u.%u%c", &x, &y, &tail) < 2 || x > 255u || y < 1u || y > 254u)
		return 0;
	snprintf(out, 16, "127.107.%u.%u", x, y);
	return 1;
}

/* Whether a live machine of another id has `address`: who, or NULL. */
static const slot *holding(const char *id, const char *address)
{
	for (unsigned i = 0u; i < MAX_MACHINES; i++) {
		const slot *other = &machines[i];

		if (other->used && !other->exited && strcmp(other->id, id) != 0 &&
		    strcmp(other->address, address) == 0)
			return other;
	}
	return NULL;
}

/*
 * Two ids may come to one address -- FNV-1a's modulus maps ids that
 * differ regularly onto each other, as a89 and b55 -- and the second
 * takes the next address free after its own, decided by the holder
 * 2026-10-29 over refusing it, which a network of hundreds met. So that
 * the moved address is as stable as an id's own, it is kept in
 * hull/machines/ID/address and taken first while free, and the file goes
 * once the id's own address is what it gets. Into `out`; `moved_from` is
 * the machine whose address it would have had, or "".
 */
static void address_choose(const char *id, char out[16], char moved_from[64])
{
	uint32_t own = address_slot(id);
	char     path[4300];
	char     kept[16];

	moved_from[0] = '\0';
	if (address_kept(id, kept) && holding(id, kept) == NULL) {
		snprintf(out, 16, "%s", kept);
		return;
	}
	for (uint32_t step = 0u; step < ADDRESSES; step++) {
		const slot *other;

		address_at((own + step) % ADDRESSES, out);
		if ((other = holding(id, out)) == NULL)
			break;
		if (step == 0u)
			snprintf(moved_from, 64, "%s", other->id);
	}
	if (!address_path(id, path, sizeof path))
		return;
	if (moved_from[0] == '\0') {
		unlink(path);
	} else {
		char  dir[4300];
		FILE *file;

		snprintf(dir, sizeof dir, "%s/%s", host_key_root, id);
		if ((mkdir(dir, 0700) == 0 || errno == EEXIST) && (file = fopen(path, "we")) != NULL) {
			fprintf(file, "%s\n", out);
			fclose(file);
		}
	}
}

static void address_of(const char *id, char out[16])
{
	if (!address_kept(id, out))
		address_at(address_slot(id), out);
}

static void handle_run(client *c, uint32_t ch, const hull_sexpr_doc *body)
{
	hull_sexpr_form    value;
	hull_sexpr_form    plan_form;
	hull_sexpr_doc     plan_doc;
	hull_sexpr_form    machine;
	hull_error         error = { .struct_size = sizeof error };
	const hull_engine *engine = NULL;
	char               id[64];
	char               address[16];
	char               moved_from[64];
	char              *engine_name;
	char              *class_name;
	char              *given;
	char              *dir_given;
	char              *on_disconnect;
	unsigned           generation;
	slot              *s;
	hull_buf           plan;
	hull_sexpr_form    secrets;
	uint8_t           *addressed;
	size_t             addressed_length;
	hull_status        created;

	if (!hull_msg_find(body, "machine", &value) || !hull_msg_string(body, &value, id, sizeof id) ||
	    !is_label(id)) {
		send_error(c, ch, "a machine id is a DNS label");
		return;
	}
	s = find_machine(id);
	if (s != NULL && !s->exited) {
		send_error(c, ch, "a machine of that name is running");
		return;
	}
	if (!hull_msg_find(body, "plan", &plan_form)) {
		send_error(c, ch, "the run has no plan");
		return;
	}
	plan.data = body->data + plan_form.offset;
	plan.len  = plan_form.extent;
	if (!hull_access_machine(&plan_doc, plan.data, plan.len, &machine)) {
		send_error(c, ch, "the plan is not a plan");
		return;
	}
	{
		hull_sexpr_form top;

		hull_sexpr_first(&plan_doc, &top);
		engine_name = hull_access_value(&plan_doc, &top, "engine");
		class_name  = hull_access_value(&plan_doc, &top, "class");
		given       = hull_access_value(&plan_doc, &top, "address");
		dir_given   = hull_access_value(&plan_doc, &top, "secrets");
	}
	if (given != NULL || dir_given != NULL) {
		send_error(c, ch, given != NULL ? "a plan's address is the core's to give" :
		                                  "a plan's secrets' directory is the core's to give");
		free(given);
		free(dir_given);
		free(engine_name);
		free(class_name);
		return;
	}
	address_choose(id, address, moved_from);
	for (uint32_t i = 0u; engine_name != NULL && i < registry.count; i++)
		if (strcmp(registry.engines[i]->caps->engine_name, engine_name) == 0)
			engine = registry.engines[i];
	free(engine_name);
	if (engine == NULL || class_name == NULL || strlen(class_name) >= sizeof s->class_name) {
		free(class_name);
		send_error(c, ch, "the plan names no engine this core has");
		return;
	}
	if (engine->ops->create == NULL || engine->ops->start == NULL || engine->ops->stop == NULL ||
	    engine->ops->destroy == NULL) {
		free(class_name);
		send_error(c, ch, "that engine cannot run a machine yet");
		return;
	}

	if (s == NULL) {
		for (unsigned i = 0u; i < MAX_MACHINES && s == NULL; i++)
			if (!machines[i].used)
				s = &machines[i];
		/* Full of live machines, or room made by forgetting one that
		 * has exited. */
		for (unsigned i = 0u; i < MAX_MACHINES && s == NULL; i++)
			if (machines[i].exited)
				s = &machines[i];
		if (s == NULL) {
			free(class_name);
			send_error(c, ch, "the core holds as many machines as it can");
			return;
		}
	}
	/* Followers still reading the machine that held the slot hear what
	 * they had not read as lost, and its exit. */
	for (unsigned i = 0u; s->used && i < s->follower_count; i++) {
		follower *f = &s->followers[i];

		if (f->at < s->log_end)
			send_lost(f->client, f->ch, s->log_end - f->at);
		if (f->until != FOLLOW_ON)
			send_simple(f->client, "(3:eof", f->ch);
		else
			send_exited(f->client, f->ch, s);
	}
	drop_log(s);
	for (unsigned i = 0u; i < s->named_count; i++)
		free(s->named[i].hosts);
	generation = s->generation + 1u;
	memset(s, 0, sizeof *s);
	s->used            = 1;
	s->generation      = generation;
	s->engine          = engine;
	s->core.slot       = s;
	s->core.generation = generation;
	s->owner     = c;
	snprintf(s->id, sizeof s->id, "%s", id);
	snprintf(s->address, sizeof s->address, "%s", address);
	snprintf(s->class_name, sizeof s->class_name, "%s", class_name);
	snprintf(s->state, sizeof s->state, "creating");
	free(class_name);
	on_disconnect = hull_access_value(&plan_doc, &machine, "on_disconnect");
	s->keep = on_disconnect != NULL && strcmp(on_disconnect, "keep") == 0;
	free(on_disconnect);
	{
		const char *wrong = links_of(s, &plan_doc, &machine);

		if (wrong == NULL)
			wrong = named_of(s, &plan_doc, &machine);

		if (wrong != NULL) {
			send_error(c, ch, wrong);
			s->used = 0;
			return;
		}
	}

	addressed = with_address(&plan, address, &addressed_length);
	if (addressed == NULL) {
		send_error(c, ch, "the plan is not a plan");
		s->used = 0;
		return;
	}
	plan.data = addressed;
	plan.len  = addressed_length;
	if (hull_access_find(&plan_doc, &machine, "secrets", &secrets)) {
		char        dir[4300];
		const char *why = write_secrets(id, &plan_doc, &secrets, body, dir, sizeof dir);
		uint8_t    *with = why == NULL ? with_secrets(&plan, dir, &addressed_length) : NULL;

		if (why == NULL && with == NULL) {
			why = "the plan is not a plan";
			remove_secrets(id);
		}
		free(addressed);
		if (why != NULL) {
			send_error(c, ch, why);
			s->used = 0;
			return;
		}
		s->secrets = 1;
		addressed = with;
		plan.data = with;
		plan.len  = addressed_length;
	} else if (hull_msg_find(body, "secrets", &value)) {
		free(addressed);
		send_error(c, ch, "the run gives a secret the plan does not name");
		s->used = 0;
		return;
	}
	if (hull_access_true(&plan_doc, &machine, "ssh")) {
		char        dir[4300];
		const char *why = write_ssh(id, dir, sizeof dir);
		uint8_t    *with = why == NULL ? with_ssh(&plan, dir, &addressed_length) : NULL;

		if (why == NULL && with == NULL) {
			why = "the plan is not a plan";
			remove_ssh(id);
		}
		free(addressed);
		if (why != NULL) {
			send_error(c, ch, why);
			forget_secrets(s);
			s->used = 0;
			return;
		}
		s->ssh    = 1;
		addressed = with;
		plan.data = with;
		plan.len  = addressed_length;
	}
	created = engine->ops->create(&s->core, plan, &s->handle, &error);
	free(addressed);
	if (created != HULL_OK) {
		send_error(c, ch, error.detail != NULL ? error.detail : "create failed");
		forget_secrets(s);
		forget_ssh(s);
		s->used = 0;
		return;
	}
	add_follower(s, c, ch, 0u, FOLLOW_ON);
	if (s->follower_count != 0u && s->followers[s->follower_count - 1u].client == c) {
		s->followers[s->follower_count - 1u].holds = 1;
		pthread_mutex_lock(&flow_lock);
		s->made    = 0u;
		s->hold_at = 0u;
		s->holding = 1;
		pthread_mutex_unlock(&flow_lock);
	}
	if (engine->ops->start(&s->core, s->handle, &error) != HULL_OK) {
		send_error(c, ch, error.detail != NULL ? error.detail : "start failed");
		engine->ops->destroy(&s->core, s->handle, &error);
		forget_secrets(s);
		forget_ssh(s);
		s->handle         = NULL;
		s->used           = 0;
		s->follower_count = 0u;
		return;
	}
	keep_machine(s);
	send_started(c, ch, s);
	if (moved_from[0] != '\0') {
		char notice[200];
		int  n = snprintf(notice, sizeof notice, "hull: %s's address is machine %s's, so it "
		                  "takes %s, and keeps it\n", s->id, moved_from, s->address);

		/* On the run's stderr, said once, and in the log. */
		send_data(c, ch, 2, (const uint8_t *)notice, (size_t)n);
		notice[n - 1] = '\0';
		on_log(NULL, 2, notice + 6);
	}
}

static slot *named(client *c, uint32_t ch, const hull_sexpr_doc *body)
{
	hull_sexpr_form value;
	char            id[64];
	slot           *s;

	if (!hull_msg_find(body, "machine", &value) || !hull_msg_string(body, &value, id, sizeof id) ||
	    (s = find_machine(id)) == NULL) {
		send_error(c, ch, "no such machine");
		return NULL;
	}
	return s;
}

static void handle_attach(client *c, uint32_t ch, const hull_sexpr_doc *body)
{
	slot *s = named(c, ch, body);

	if (s == NULL)
		return;
	if (s->exited)
		send_exited(c, ch, s);
	else
		add_follower(s, c, ch, s->log_end, FOLLOW_ON);
}

static void handle_logs(client *c, uint32_t ch, const hull_sexpr_doc *body)
{
	hull_sexpr_form value;
	slot           *s = named(c, ch, body);
	int             follow;

	if (s == NULL)
		return;
	follow = hull_msg_find(body, "follow", &value) && hull_sexpr_is(body, &value, "true");
	/* What the log holds, read as a follower reads it; to its end now,
	 * or on to the exit. */
	add_follower(s, c, ch, s->log_start, follow ? FOLLOW_ON : s->log_end);
}

static void handle_stop(client *c, uint32_t ch, const hull_sexpr_doc *body)
{
	hull_sexpr_form value;
	long long       timeout = 10000;
	slot           *s = named(c, ch, body);

	if (s == NULL)
		return;
	if (hull_msg_find(body, "timeout_ms", &value) && !hull_msg_long(body, &value, &timeout)) {
		send_error(c, ch, "timeout_ms is a number");
		return;
	}
	if (s->exited) {
		send_simple(c, "(4:done", ch);
		return;
	}
	if (s->stop_count < MAX_STOPS)
		s->stops[s->stop_count++] = (follower){ c, ch, 0u, 0u, 0 };
	stop_machine(s, timeout < 0 ? 0 : timeout > 600000 ? 600000 : (int)timeout);
}

/* A command in a running machine; the request itself is the engine's
 * exec_spec, being canonical already. */
/* `(key n)` of `record`, as a size: 1 to 65535, minimal decimal. */
static int dimension(const hull_sexpr_doc *body, const hull_sexpr_form *record, const char *key,
                     long long *out)
{
	hull_sexpr_form entry;
	hull_sexpr_form name;
	hull_sexpr_form value;

	return hull_access_find(body, record, key, &entry) &&
	       hull_sexpr_child(body, &entry, &name) && hull_sexpr_next(body, &name, &value) &&
	       hull_msg_long(body, &value, out) && *out >= 1 && *out <= 65535;
}

/* An exec's `(tty false)`, or a terminal of a size it can be. */
static int terminal_ok(const hull_sexpr_doc *body)
{
	hull_sexpr_form tty;
	long long       n;

	if (!hull_msg_find(body, "tty", &tty))
		return 0;
	return tty.kind == HULL_SEXPR_ATOM ||
	       (dimension(body, &tty, "cols", &n) && dimension(body, &tty, "rows", &n));
}

/* An exec, or with `dial` a connection to one of the machine's ports,
 * which is a stream as an exec is: from here on the two are one. */
static void handle_exec(client *c, uint32_t ch, const hull_sexpr_doc *body, int dial)
{
	hull_error             error = { .struct_size = sizeof error };
	hull_stream           *stream = NULL;
	exec_entry            *x = NULL;
	slot                  *s = named(c, ch, body);
	const hull_engine_ops *ops;
	hull_sexpr_form        value;
	long long              port = 0;
	hull_buf               spec;
	hull_status            status;

	if (s == NULL)
		return;
	ops = s->engine->ops;
	if (s->exited || s->handle == NULL) {
		send_error(c, ch, "the machine is not running");
		return;
	}
	if (!dial && !terminal_ok(body)) {
		send_error(c, ch, "a window is 1 to 65535 in each direction");
		return;
	}
	if (dial && (!hull_msg_find(body, "port", &value) || !hull_msg_long(body, &value, &port) ||
	             port < 1 || port > 65535)) {
		send_error(c, ch, "a port is 1 to 65535");
		return;
	}
	if (dial && (ops->struct_size < offsetof(hull_engine_ops, dial) + sizeof ops->dial ||
	             ops->dial == NULL)) {
		send_error(c, ch, "that engine's machines have no ports of their own");
		return;
	}
	if ((!dial && ops->exec == NULL) || ops->stream_write == NULL || ops->stream_eof == NULL ||
	    ops->stream_close == NULL) {
		send_error(c, ch, "that engine cannot exec yet");
		return;
	}
	if (exec_on(c, ch) != NULL) {
		send_error(c, ch, "that channel is in use");
		return;
	}
	for (unsigned i = 0u; i < MAX_EXECS && x == NULL; i++)
		if (!execs[i].used)
			x = &execs[i];
	if (x == NULL) {
		send_error(c, ch, "the core runs as many commands as it can");
		return;
	}
	spec.data = body->data;
	spec.len  = body->length;
	pthread_mutex_lock(&flow_lock);
	*x = (exec_entry){ .used = 1, .client = c, .ch = ch, .machine = s };
	adopting = x;
	pthread_mutex_unlock(&flow_lock);
	if (dial)
		status = ops->dial(&s->core, s->handle, (uint16_t)port, &stream, &error);
	else
		status = ops->exec(&s->core, s->handle, spec, &stream, &error);
	pthread_mutex_lock(&flow_lock);
	adopting = NULL;
	/* Held to the window from here, as the exec is had: opened only
	 * once its input's writer ran, what the reader read in between was
	 * never held back, 167936 bytes on 65536 of credit at a load of 199.
	 * Not during exec() itself, where a reader held for credit would be
	 * one a failing engine joins. */
	if (status == HULL_OK) {
		x->stream      = stream;
		x->output_open = 1;
	} else
		*x = (exec_entry){ 0 };
	pthread_mutex_unlock(&flow_lock);
	if (status != HULL_OK) {
		send_error(c, ch, error.detail != NULL ? error.detail : "exec failed");
		return;
	}
	x->credit = INPUT_WINDOW;
	x->input  = malloc(INPUT_WINDOW);
	pthread_mutex_init(&x->lock, NULL);
	pthread_cond_init(&x->more, NULL);
	if (x->input == NULL || pthread_create(&x->writer, NULL, write_input, x) != 0) {
		/* A reader waiting for credit is let go first, as close_exec()
		 * lets it, since stream_close() waits for it. */
		pthread_mutex_lock(&flow_lock);
		x->output_open = 0;
		pthread_cond_broadcast(&flow_room);
		pthread_mutex_unlock(&flow_lock);
		s->engine->ops->stream_close(&s->core, stream, &error);
		pthread_cond_destroy(&x->more);
		pthread_mutex_destroy(&x->lock);
		free(x->input);
		pthread_mutex_lock(&flow_lock);
		*x = (exec_entry){ 0 };
		pthread_mutex_unlock(&flow_lock);
		send_error(c, ch, "the core cannot take the command's input");
		return;
	}
	send_started(c, ch, s);
	send_credit(c, ch, INPUT_WINDOW);
}

/* An exec's stdin, and its end, for the writer. More than the frontend
 * has credit for is refused; once the command's input has closed, what
 * comes is dropped and its credit given straight back. */
static void handle_input(client *c, uint32_t ch, const hull_sexpr_doc *body, int eof)
{
	hull_sexpr_form buf;
	exec_entry     *x = exec_on(c, ch);
	const char     *refusal = NULL;
	size_t          n;

	if (x == NULL) {
		send_error(c, ch, "no command on that channel");
		return;
	}
	if (!eof && !hull_msg_find(body, "buf", &buf))
		return;
	n = eof ? 0u : buf.content_length;
	if (n > x->credit) {
		send_error(c, ch, "more input than its credit");
		return;
	}
	x->credit -= n;
	pthread_mutex_lock(&x->lock);
	if (x->input_ended) {
		refusal = "input after its end";
	} else if (eof) {
		x->input_ended = 1;
	} else if (x->input_closed) {
		x->written += n;
	} else {
		for (size_t i = 0u, at = (x->head + x->queued) % INPUT_WINDOW; i < n; i++) {
			x->input[at] = body->data[buf.content + i];
			at = at + 1u == INPUT_WINDOW ? 0u : at + 1u;
		}
		x->queued += n;
	}
	pthread_cond_signal(&x->more);
	pthread_mutex_unlock(&x->lock);
	if (refusal != NULL)
		send_error(c, ch, refusal);
}

/* More of an exec's output the frontend will take. Credit for a command
 * that has ended is not news: it crossed the exit on its way. */
static void handle_credit(client *c, uint32_t ch, const hull_sexpr_doc *body)
{
	hull_sexpr_form value;
	exec_entry     *x = exec_on(c, ch);
	long long       n = 0;
	int             over = 0;

	if (!hull_msg_find(body, "bytes", &value) || !hull_msg_long(body, &value, &n) || n <= 0) {
		send_error(c, ch, "credit is a number of bytes");
		return;
	}
	if (x == NULL)
		return;
	pthread_mutex_lock(&flow_lock);
	if ((unsigned long long)n > x->held) {
		over = 1;
	} else {
		x->held -= (size_t)n;
		pthread_cond_broadcast(&flow_room);
	}
	pthread_mutex_unlock(&flow_lock);
	if (over)
		send_error(c, ch, "more credit than it was sent");
}

/* Credit for what the writers have written, and word of an input that
 * has closed, once. */
static void give_credit(void)
{
	for (unsigned i = 0u; i < MAX_EXECS; i++) {
		exec_entry *x = &execs[i];
		size_t      n;
		int         input_closed;

		if (!x->used || x->closed)
			continue;
		pthread_mutex_lock(&x->lock);
		n = x->written;
		x->written = 0u;
		input_closed = x->input_closed;
		pthread_mutex_unlock(&x->lock);
		if (input_closed && !x->told_closed) {
			x->told_closed = 1;
			send_error(x->client, x->ch, "the command's input has closed");
		}
		if (n > 0u) {
			x->credit += n;
			send_credit(x->client, x->ch, n);
		}
	}
}

/* The signals a frontend may forward, by the names B.3 uses. */
static const struct {
	const char *name;
	int         number;
} forwardable[] = {
	{ "HUP", SIGHUP }, { "INT", SIGINT }, { "QUIT", SIGQUIT }, { "TERM", SIGTERM },
	{ "USR1", SIGUSR1 }, { "USR2", SIGUSR2 }, { "KILL", SIGKILL },
};

/* The `(signal NAME)` of a request as a number, or 0 for one the core
 * does not forward. */
static int signal_number(const hull_sexpr_doc *body)
{
	hull_sexpr_form value;

	if (hull_msg_find(body, "signal", &value))
		for (unsigned i = 0u; i < sizeof forwardable / sizeof forwardable[0]; i++)
			if (hull_sexpr_is(body, &value, forwardable[i].name))
				return forwardable[i].number;
	return 0;
}

/* An exec's terminal resized, or a signal for it. Neither is answered
 * unless it is refused. */
static void handle_control(client *c, uint32_t ch, const hull_sexpr_doc *body, int winch)
{
	hull_error      error = { .struct_size = sizeof error };
	hull_sexpr_form value;
	exec_entry     *x = exec_on(c, ch);
	const hull_engine_ops *ops;
	hull_status     status;

	if (x == NULL) {
		send_error(c, ch, "no command on that channel");
		return;
	}
	ops = x->machine->engine->ops;
	if (winch) {
		long long cols = 0;
		long long rows = 0;

		if (!hull_msg_find(body, "cols", &value) || !hull_msg_long(body, &value, &cols) ||
		    !hull_msg_find(body, "rows", &value) || !hull_msg_long(body, &value, &rows) ||
		    cols < 1 || cols > 65535 || rows < 1 || rows > 65535) {
			send_error(c, ch, "a window is 1 to 65535 in each direction");
			return;
		}
		if (ops->resize == NULL) {
			send_error(c, ch, "that engine cannot resize a terminal");
			return;
		}
		status = ops->resize(&x->machine->core, x->stream, (int)rows, (int)cols, &error);
	} else {
		int number = signal_number(body);

		if (number == 0) {
			send_error(c, ch, "not a signal the core forwards");
			return;
		}
		if (ops->signal == NULL) {
			send_error(c, ch, "that engine cannot signal a command");
			return;
		}
		status = ops->signal(&x->machine->core, x->machine->handle, x->stream, number, &error);
	}
	if (status != HULL_OK)
		send_error(c, ch, error.detail != NULL ? error.detail : "refused");
}

/* A signal for a machine's workload, through its engine; `done` once it
 * is sent. What the workload does with it is the workload's, and an exit
 * reaches whoever follows the machine, as any exit does. */
static void handle_kill(client *c, uint32_t ch, const hull_sexpr_doc *body)
{
	hull_error error = { .struct_size = sizeof error };
	slot      *s = named(c, ch, body);
	int        number;

	if (s == NULL)
		return;
	number = signal_number(body);
	if (number == 0) {
		send_error(c, ch, "not a signal the core forwards");
	} else if (s->exited || s->handle == NULL) {
		send_error(c, ch, "the machine is not running");
	} else if (s->engine->ops->signal == NULL) {
		send_error(c, ch, "that engine cannot signal a machine");
	} else if (wake_machine(s), s->engine->ops->signal(&s->core, s->handle, NULL, number,
	                                                   &error) != HULL_OK) {
		send_error(c, ch, error.detail != NULL ? error.detail : "refused");
	} else {
		send_simple(c, "(4:done", ch);
	}
}

/* `hull pause` and `hull unpause`: every process of a running machine
 * stopped where it is, or let go on; answered `done`. */
static void handle_pause(client *c, uint32_t ch, const hull_sexpr_doc *body, int paused)
{
	hull_error             error = { .struct_size = sizeof error };
	slot                  *s = named(c, ch, body);
	const hull_engine_ops *ops;

	if (s == NULL)
		return;
	ops = s->engine->ops;
	if (s->exited || s->handle == NULL) {
		send_error(c, ch, "the machine is not running");
	} else if (ops->struct_size < offsetof(hull_engine_ops, pause) + sizeof ops->pause ||
	           ops->pause == NULL) {
		send_error(c, ch, "that engine cannot pause a machine");
	} else if (s->paused == paused) {
		send_error(c, ch, paused ? "the machine is paused already" : "the machine is not paused");
	} else if (ops->pause(&s->core, s->handle, paused, &error) != HULL_OK) {
		send_error(c, ch, error.detail != NULL ? error.detail : "refused");
	} else {
		s->paused = paused;
		snprintf(s->state, sizeof s->state, "%s", paused ? "paused" : "running");
		send_simple(c, "(4:done", ch);
	}
}

/* What the host spends on a running machine, as its engine measures it:
 * the engine's `(stats (cpu n) (io n) (memory n))`, on this channel. */
static void handle_stats(client *c, uint32_t ch, const hull_sexpr_doc *body)
{
	static const char *const keys[] = { "cpu", "io", "memory" };
	static const char *const opens[] = { "(3:cpu", ")(2:io", ")(6:memory" };
	hull_error     error = { .struct_size = sizeof error };
	slot          *s = named(c, ch, body);
	hull_buf       out = { NULL, 0u };
	hull_sexpr_doc stats;
	hull_msg       m = { 0 };
	long long      values[3];

	if (s == NULL)
		return;
	if (s->exited || s->handle == NULL) {
		send_error(c, ch, "the machine is not running");
		return;
	}
	{
		hull_sexpr_form value;
		int             guest = hull_msg_find(body, "guest", &value) &&
		                        hull_sexpr_is(body, &value, "true");
		hull_status (*measure)(hull_core *, hull_machine *, hull_buf *, hull_error *);

		measure = guest ? s->engine->ops->guest_stats : s->engine->ops->stats;
		if (measure == NULL) {
			send_error(c, ch, guest ? "that engine has no view of a machine from inside" :
			                          "that engine does not measure a machine");
			return;
		}
		if (measure(&s->core, s->handle, &out, &error) != HULL_OK) {
			send_error(c, ch, error.detail != NULL ? error.detail : "refused");
			return;
		}
	}
	if (hull_sexpr_open(&stats, out.data, out.len, HULL_SEXPR_CANONICAL, NULL) != HULL_SEXPR_OK ||
	    !hull_msg_is(&stats, "stats")) {
		send_error(c, ch, "the engine's stats are not (stats ...)");
		return;
	}
	for (unsigned i = 0u; i < 3u; i++) {
		hull_sexpr_form value;

		if (!hull_msg_find(&stats, keys[i], &value) ||
		    !hull_msg_long(&stats, &value, &values[i])) {
			send_error(c, ch, "the engine's stats lack a figure");
			return;
		}
	}
	hull_msg_raw(&m, "(5:stats");
	hull_msg_channel(&m, ch);
	for (unsigned i = 0u; i < 3u; i++) {
		hull_msg_raw(&m, opens[i]);
		hull_msg_number(&m, values[i]);
	}
	hull_msg_raw(&m, "))");
	send_to(c, &m);
}

/* --- segments (Appendix D) ---------------------------------------------------- */

/* A probability, 0 to 1, as a decimal atom. */
static int probability(const hull_sexpr_doc *body, const hull_sexpr_form *value, double *out)
{
	char  text[32];
	char *end;

	if (!hull_msg_string(body, value, text, sizeof text) || text[0] < '0' || text[0] > '9')
		return 0;
	errno = 0;
	*out = strtod(text, &end);
	return errno == 0 && *end == '\0' && *out >= 0.0 && *out <= 1.0;
}

/* A count from 0 to `most`, as a minimal decimal atom. */
static int whole_count(const hull_sexpr_doc *body, const hull_sexpr_form *value, long long most,
                 uint64_t *out)
{
	long long n;

	if (!hull_msg_long(body, value, &n) || n < 0 || n > most)
		return 0;
	*out = (uint64_t)n;
	return 1;
}

/* `impair`'s `(impairment ...)` entry as the switch takes it: NULL, or
 * what is wrong with it. */
static const char *impairment_of(const hull_sexpr_doc *body, const hull_sexpr_form *entry,
                                 hull_impairment *m)
{
	hull_sexpr_form item;
	int             more;

	memset(m, 0, sizeof *m);
	if (entry->kind != HULL_SEXPR_LIST || !hull_sexpr_child(body, entry, &item))
		return "an impairment is a list";
	for (more = hull_sexpr_next(body, &item, &item); more;
	     more = hull_sexpr_next(body, &item, &item)) {
		hull_sexpr_form key;
		hull_sexpr_form value;
		uint64_t        n = 0u;
		char            name[16];

		if (item.kind != HULL_SEXPR_LIST || !hull_sexpr_child(body, &item, &key) ||
		    !hull_msg_string(body, &key, name, sizeof name) ||
		    !hull_sexpr_next(body, &key, &value))
			return "an impairment's entry is (key value)";
		if (strcmp(name, "down") == 0) {
			if (!hull_sexpr_is(body, &value, "true"))
				return "down is true";
			m->down = 1;
		} else if (strcmp(name, "loss") == 0 || strcmp(name, "duplicate") == 0 ||
		           strcmp(name, "corrupt") == 0 || strcmp(name, "reorder") == 0) {
			double *to = name[0] == 'l' ? &m->loss : name[0] == 'd' ? &m->duplicate :
			             name[0] == 'c' ? &m->corrupt : &m->reorder;

			if (!probability(body, &value, to))
				return "a probability is from 0 to 1";
		} else if (strcmp(name, "gemodel") == 0) {
			/* (gemodel (bad_loss 1-h)? (good_loss 1-k)? (p p) (r r)?),
			 * with netem's defaults: r is 1-p, 1-h is 1, 1-k is 0. */
			static const char *const parts[] = { "bad_loss", "good_loss", "p", "r" };
			double *to[4] = { &m->bad_loss, &m->good_loss, &m->p, &m->r };
			int     given[4] = { 0, 0, 0, 0 };

			for (int go = 1; go; go = hull_sexpr_next(body, &value, &value)) {
				hull_sexpr_form part;
				hull_sexpr_form number;
				unsigned        k;

				if (value.kind != HULL_SEXPR_LIST || !hull_sexpr_child(body, &value, &part) ||
				    !hull_sexpr_next(body, &part, &number))
					return "gemodel is (p n) and maybe (r n), (bad_loss n) and (good_loss n)";
				for (k = 0u; k < 4u && !hull_sexpr_is(body, &part, parts[k]); k++)
					;
				if (k == 4u || given[k] || !probability(body, &number, to[k]))
					return "gemodel is (p n) and maybe (r n), (bad_loss n) and (good_loss n), "
					       "each from 0 to 1";
				given[k] = 1;
			}
			if (!given[2])
				return "gemodel needs its p";
			if (!given[0])
				m->bad_loss = 1.0;
			if (!given[3])
				m->r = 1.0 - m->p;
			m->gemodel = 1;
		} else if (strcmp(name, "delay") == 0 || strcmp(name, "jitter") == 0 ||
		           strcmp(name, "mtu") == 0 || strcmp(name, "limit") == 0) {
			/* A minute's delay, a jumbo frame, netem's own bound. */
			long long most = name[0] == 'm' ? 65535 : name[0] == 'l' ? 65536 : 60000;

			if (!whole_count(body, &value, most, &n))
				return name[0] == 'm' ? "an mtu is at most 65535" :
				       name[0] == 'l' ? "a limit is at most 65536 frames" :
				                        "a delay or a jitter is at most 60000 ms";
			if (name[0] == 'd')
				m->delay_ms = (unsigned)n;
			else if (name[0] == 'j')
				m->jitter_ms = (unsigned)n;
			else if (name[0] == 'm')
				m->mtu = (unsigned)n;
			else
				m->limit = (unsigned)n;
		} else if (strcmp(name, "rate") == 0 || strcmp(name, "seed") == 0) {
			if (!whole_count(body, &value, LLONG_MAX, name[0] == 'r' ? &m->rate : &m->seed))
				return "a rate in bits a second, or a seed, is a whole number";
		} else {
			return "an impairment has no such key";
		}
		if (hull_sexpr_next(body, &value, &value) && strcmp(name, "gemodel") != 0)
			return "an impairment's entry is (key value)";
	}
	if (m->jitter_ms > m->delay_ms)
		return "a jitter is at most its delay";
	return NULL;
}

/* "OFFSET:HEX" into an offset and bytes: a byte offset from 0 to 65535,
 * a colon, and an even run of hex. NULL, or what is wrong. */
static const char *offset_hex(const char *text, unsigned *offset, unsigned char *out,
                              unsigned *len)
{
	const char *colon = strchr(text, ':');
	char       *end;
	long        off;
	size_t      n;
	size_t      k;

	if (colon == NULL)
		return "match and set are \"OFFSET:HEX\"";
	off = strtol(text, &end, 10);
	if (end != colon || off < 0 || off > 65535)
		return "match and set offsets are 0 to 65535";
	n = strlen(colon + 1);
	if (n == 0u || n % 2u != 0u || n / 2u > 32u)
		return "match and set take 1 to 32 bytes of hex";
	for (k = 0u; k < n; k++)
		if (!isxdigit((unsigned char)colon[1 + k]))
			return "match and set hex is hex digits";
	for (k = 0u; k < n; k += 2u) {
		unsigned byte;

		sscanf(colon + 1 + k, "%2x", &byte);
		out[k / 2u] = (unsigned char)byte;
	}
	*offset = (unsigned)off;
	*len    = (unsigned)(n / 2u);
	return NULL;
}

/* `tamper`'s `(fault ...)` entry as the switch takes it: NULL, or what
 * is wrong with it. Every value is one atom; match and set are
 * "OFFSET:HEX". */
static const char *tamper_of(const hull_sexpr_doc *body, const hull_sexpr_form *entry,
                             hull_tamper *t)
{
	hull_sexpr_form item;
	int             more;

	memset(t, 0, sizeof *t);
	if (entry->kind != HULL_SEXPR_LIST || !hull_sexpr_child(body, entry, &item))
		return "a fault is a list";
	for (more = hull_sexpr_next(body, &item, &item); more;
	     more = hull_sexpr_next(body, &item, &item)) {
		hull_sexpr_form key;
		hull_sexpr_form value;
		uint64_t        n = 0u;
		char            name[12];

		if (item.kind != HULL_SEXPR_LIST || !hull_sexpr_child(body, &item, &key) ||
		    !hull_msg_string(body, &key, name, sizeof name) ||
		    !hull_sexpr_next(body, &key, &value))
			return "a fault's entry is (key value)";
		if (strcmp(name, "match") == 0 || strcmp(name, "set") == 0) {
			char        text[80];
			const char *wrong;

			if (!hull_msg_string(body, &value, text, sizeof text))
				return "match and set are \"OFFSET:HEX\"";
			wrong = name[0] == 'm' ? offset_hex(text, &t->match_offset, t->match, &t->match_length)
			                       : offset_hex(text, &t->set_offset, t->set, &t->set_length);
			if (wrong != NULL)
				return wrong;
		} else if (strcmp(name, "drop") == 0 || strcmp(name, "rewrite") == 0 ||
		           strcmp(name, "flip") == 0 || strcmp(name, "replay") == 0) {
			double *to = name[0] == 'd' ? &t->drop : name[0] == 'r' && name[1] == 'e' &&
			             name[2] == 'w' ? &t->rewrite : name[0] == 'f' ? &t->flip : &t->replay;

			if (!probability(body, &value, to))
				return "a probability is from 0 to 1";
		} else if (strcmp(name, "copies") == 0 || strcmp(name, "every") == 0) {
			if (!whole_count(body, &value, name[0] == 'c' ? 1000 : 60000, &n))
				return "copies is at most 1000, every at most 60000 ms";
			if (name[0] == 'c')
				t->copies = (unsigned)n;
			else
				t->every_ms = (unsigned)n;
		} else {
			return "a fault has no such key";
		}
		if (hull_sexpr_next(body, &value, &value))
			return "a fault's entry is (key value)";
	}
	if (t->set_length != 0u && t->rewrite == 0.0)
		return "a set needs a rewrite probability";
	return NULL;
}

static void handle_impair(client *c, uint32_t ch, const hull_sexpr_doc *body)
{
	hull_sexpr_form value;
	hull_impairment impairment;
	const char     *wrong = NULL;
	char            name[512];
	char            machine[64];
	char            direction[8];
	char            why[256];

	if (!hull_msg_find(body, "name", &value) || !hull_msg_string(body, &value, name, sizeof name) ||
	    !hull_msg_find(body, "machine", &value) ||
	    !hull_msg_string(body, &value, machine, sizeof machine) ||
	    (strcmp(machine, "*") != 0 && !is_label(machine)) ||
	    !hull_msg_find(body, "direction", &value) ||
	    !hull_msg_string(body, &value, direction, sizeof direction) ||
	    (strcmp(direction, "in") != 0 && strcmp(direction, "out") != 0)) {
		send_error(c, ch, "impair names a segment, a machine or *, and in or out");
		return;
	}
	if (!hull_msg_find(body, "impairment", &value) ||
	    (wrong = impairment_of(body, &value, &impairment)) != NULL) {
		send_error(c, ch, wrong != NULL ? wrong : "impair carries an impairment");
		return;
	}
	if (hull_segment_impair(name, strcmp(machine, "*") == 0 ? NULL : machine,
	                        strcmp(direction, "in") == 0 ? HULL_SEGMENT_IN : HULL_SEGMENT_OUT,
	                        &impairment, why, sizeof why) != 0) {
		send_error(c, ch, why);
		return;
	}
	send_simple(c, "(4:done", ch);
}

static void handle_tamper(client *c, uint32_t ch, const hull_sexpr_doc *body)
{
	hull_sexpr_form value;
	hull_tamper     fault;
	const char     *wrong = NULL;
	char            name[512];
	char            machine[64];
	char            direction[8];
	char            why[256];

	if (!hull_msg_find(body, "name", &value) || !hull_msg_string(body, &value, name, sizeof name) ||
	    !hull_msg_find(body, "machine", &value) ||
	    !hull_msg_string(body, &value, machine, sizeof machine) ||
	    (strcmp(machine, "*") != 0 && !is_label(machine)) ||
	    !hull_msg_find(body, "direction", &value) ||
	    !hull_msg_string(body, &value, direction, sizeof direction) ||
	    (strcmp(direction, "in") != 0 && strcmp(direction, "out") != 0)) {
		send_error(c, ch, "tamper names a segment, a machine or *, and in or out");
		return;
	}
	if (!hull_msg_find(body, "fault", &value) ||
	    (wrong = tamper_of(body, &value, &fault)) != NULL) {
		send_error(c, ch, wrong != NULL ? wrong : "tamper carries a fault");
		return;
	}
	if (hull_segment_tamper(name, strcmp(machine, "*") == 0 ? NULL : machine,
	                        strcmp(direction, "in") == 0 ? HULL_SEGMENT_IN : HULL_SEGMENT_OUT,
	                        &fault, why, sizeof why) != 0) {
		send_error(c, ch, why);
		return;
	}
	send_simple(c, "(4:done", ch);
}

static void handle_inject(client *c, uint32_t ch, const hull_sexpr_doc *body)
{
	hull_sexpr_form value;
	unsigned char   frame[65600];
	char            name[512];
	char            machine[64];
	char            hex[131200];
	long long       count = 1;
	long long       every = 0;
	long long       size = 0;
	long long       seed = 0;
	size_t          length = 0u;
	int             randomize;
	char            why[256];

	if (!hull_msg_find(body, "name", &value) || !hull_msg_string(body, &value, name, sizeof name)) {
		send_error(c, ch, "inject names a segment");
		return;
	}
	machine[0] = '*';
	machine[1] = '\0';
	if (hull_msg_find(body, "machine", &value) &&
	    (!hull_msg_string(body, &value, machine, sizeof machine) ||
	     (strcmp(machine, "*") != 0 && !is_label(machine)))) {
		send_error(c, ch, "inject names a machine or *");
		return;
	}
	if ((hull_msg_find(body, "count", &value) && !hull_msg_long(body, &value, &count)) ||
	    (hull_msg_find(body, "every", &value) && !hull_msg_long(body, &value, &every)) ||
	    (hull_msg_find(body, "seed", &value) && !hull_msg_long(body, &value, &seed)) ||
	    count < 0 || count > 1000000000 || every < 0 || every > 60000) {
		send_error(c, ch, "inject's count, every and seed are whole numbers");
		return;
	}
	randomize = hull_msg_find(body, "size", &value);
	if (randomize) {
		if (!hull_msg_long(body, &value, &size) || size < 14 || size > 65600) {
			send_error(c, ch, "a flood's size is 14 to 65600 bytes");
			return;
		}
	} else if (count != 0 && hull_msg_find(body, "frame", &value)) {
		size_t n;
		size_t k;

		if (!hull_msg_string(body, &value, hex, sizeof hex) || (n = strlen(hex)) < 28u ||
		    n % 2u != 0u || n / 2u > sizeof frame) {
			send_error(c, ch, "a frame is 14 to 65600 bytes of hex");
			return;
		}
		for (k = 0u; k < n; k++)
			if (!isxdigit((unsigned char)hex[k])) {
				send_error(c, ch, "a frame is hex digits");
				return;
			}
		for (k = 0u; k < n; k += 2u) {
			unsigned byte;

			sscanf(hex + k, "%2x", &byte);
			frame[k / 2u] = (unsigned char)byte;
		}
		length = n / 2u;
	} else if (count != 0) {
		send_error(c, ch, "inject takes a frame of hex, or a size for random bytes");
		return;
	}
	if (hull_segment_inject(name, strcmp(machine, "*") == 0 ? NULL : machine, frame, length,
	                        randomize, (unsigned)size, (uint64_t)count, (unsigned)every,
	                        (uint64_t)seed, why, sizeof why) != 0) {
		send_error(c, ch, why);
		return;
	}
	send_simple(c, "(4:done", ch);
}

/* A capture of a segment's frames into a pcap file the core writes, as
 * the user, by its whole path (Appendix D): `frames` of them, at most a
 * million, ten thousand where none is given, and 0 to end one. */
static void handle_capture(client *c, uint32_t ch, const hull_sexpr_doc *body)
{
	hull_sexpr_form value;
	char            name[512];
	char            machine[64] = "*";
	char            way[8] = "out";
	char            path[4096] = "";
	long long       frames = 10000;
	char            why[300];

	if (!hull_msg_find(body, "name", &value) || !hull_msg_string(body, &value, name, sizeof name)) {
		send_error(c, ch, "capture names a segment");
		return;
	}
	if ((hull_msg_find(body, "machine", &value) &&
	     (!hull_msg_string(body, &value, machine, sizeof machine) ||
	      (strcmp(machine, "*") != 0 && !is_label(machine)))) ||
	    (hull_msg_find(body, "way", &value) &&
	     (!hull_msg_string(body, &value, way, sizeof way) ||
	      (strcmp(way, "in") != 0 && strcmp(way, "out") != 0)))) {
		send_error(c, ch, "capture names a machine or *, and in or out");
		return;
	}
	if ((hull_msg_find(body, "frames", &value) && !hull_msg_long(body, &value, &frames)) ||
	    frames < 0 || frames > 1000000) {
		send_error(c, ch, "a capture's frames are 0 to 1000000");
		return;
	}
	if (frames != 0 && (!hull_msg_find(body, "path", &value) ||
	                    !hull_msg_string(body, &value, path, sizeof path))) {
		send_error(c, ch, "a capture names its file");
		return;
	}
	if (hull_segment_capture(name, strcmp(machine, "*") == 0 ? NULL : machine,
	                         strcmp(way, "in") == 0 ? HULL_SEGMENT_IN : HULL_SEGMENT_OUT,
	                         frames != 0 ? path : NULL, (uint64_t)frames, why, sizeof why) != 0) {
		send_error(c, ch, why);
		return;
	}
	send_simple(c, "(4:done", ch);
}

static void handle_segment(client *c, uint32_t ch, const hull_sexpr_doc *body)
{
	static const char *const names[] = { "bytes", "corrupted", "duplicated", "frames", "full",
	                                     "injected", "lost", "matched", "overflow", "passed",
	                                     "passed_bytes", "replayed", "tampered", "too_big" };
	static hull_segment_counts counts[64];
	hull_sexpr_form value;
	hull_msg        m = { 0 };
	char            name[512];
	int             n;

	if (!hull_msg_find(body, "name", &value) ||
	    !hull_msg_string(body, &value, name, sizeof name)) {
		send_error(c, ch, "segment names a segment");
		return;
	}
	n = hull_segment_counts_of(name, counts, sizeof counts / sizeof counts[0]);
	if (n < 0) {
		send_error(c, ch, "no machine is on that segment");
		return;
	}
	hull_msg_raw(&m, "(5:ports");
	hull_msg_channel(&m, ch);
	for (int i = 0; i < n; i++) {
		/* Sorted as A.1 sorts: in, machine, out, and each way's
		 * counts by name. */
		for (int part = 0; part < 3; part++) {
			const hull_segment_count *w;
			uint64_t                  of[14];

			if (part == 0)
				hull_msg_raw(&m, "(4:port");
			if (part == 1) {
				hull_msg_raw(&m, "(7:machine");
				hull_msg_text(&m, counts[i].machine);
				hull_msg_raw(&m, ")");
				continue;
			}
			w = &counts[i].way[part == 0 ? HULL_SEGMENT_IN : HULL_SEGMENT_OUT];
			of[0] = w->bytes;
			of[1] = w->corrupted;
			of[2] = w->duplicated;
			of[3] = w->frames;
			of[4] = w->full;
			of[5] = w->injected;
			of[6] = w->lost;
			of[7] = w->matched;
			of[8] = w->overflow;
			of[9] = w->passed;
			of[10] = w->passed_bytes;
			of[11] = w->replayed;
			of[12] = w->tampered;
			of[13] = w->too_big;
			hull_msg_raw(&m, part == 0 ? "(2:in" : "(3:out");
			for (unsigned k = 0u; k < 14u; k++) {
				hull_msg_raw(&m, "(");
				hull_msg_text(&m, names[k]);
				hull_msg_number(&m, (long long)of[k]);
				hull_msg_raw(&m, ")");
			}
			hull_msg_raw(&m, ")");
		}
		hull_msg_raw(&m, ")");
	}
	hull_msg_raw(&m, ")");
	send_to(c, &m);
}

static void handle_list(client *c, uint32_t ch)
{
	hull_msg m = { 0 };

	hull_msg_raw(&m, "(8:machines");
	hull_msg_channel(&m, ch);
	for (unsigned i = 0u; i < MAX_MACHINES; i++) {
		const slot *s = &machines[i];

		if (!s->used)
			continue;
		hull_msg_raw(&m, "(7:machine(7:address");
		hull_msg_text(&m, s->address);
		hull_msg_raw(&m, ")(5:class");
		hull_msg_text(&m, s->class_name);
		hull_msg_raw(&m, ")(6:engine");
		hull_msg_text(&m, s->engine->caps->engine_name);
		hull_msg_raw(&m, ")(2:id");
		hull_msg_text(&m, s->id);
		hull_msg_raw(&m, ")(5:state");
		hull_msg_text(&m, s->state);
		hull_msg_raw(&m, "))");
	}
	hull_msg_raw(&m, ")");
	send_to(c, &m);
}

/* gc, on a thread of its own, since an engine's may take a while: each
 * engine that keeps anything is asked in turn, its report sent as data and
 * its failure on fd 2, then done. One at a time, an engine's report being
 * its own until it is next asked. */
typedef struct gc_job {
	unsigned client;
	unsigned generation;
	uint32_t ch;
} gc_job;

static pthread_mutex_t gc_lock = PTHREAD_MUTEX_INITIALIZER;

static void gc_said(const gc_job *job, event_kind kind, int fd, const void *bytes, size_t n)
{
	event *e = new_event(kind, NULL, n);

	if (e == NULL)
		return;
	e->client            = job->client;
	e->client_generation = job->generation;
	e->ch                = job->ch;
	e->fd                = fd;
	if (n > 0u)
		memcpy(e->bytes, bytes, n);
	enqueue(e);
}

static void *run_gc(void *arg)
{
	gc_job *job = arg;

	pthread_mutex_lock(&gc_lock);
	for (uint32_t i = 0u; i < registry.count; i++) {
		const hull_engine_ops *ops = registry.engines[i]->ops;
		hull_error             error = { .struct_size = sizeof error };
		hull_buf               report = { NULL, 0u };
		char                   line[600];

		if (ops->struct_size < offsetof(hull_engine_ops, gc) + sizeof ops->gc || ops->gc == NULL)
			continue;
		if (ops->gc(NULL, &report, &error) == HULL_OK) {
			if (report.len > 0u)
				gc_said(job, EVENT_GC, 1, report.data, report.len);
		} else {
			snprintf(line, sizeof line, "%s: %s\n", registry.engines[i]->caps->engine_name,
			         error.detail != NULL ? error.detail : "refused");
			gc_said(job, EVENT_GC, 2, line, strlen(line));
		}
	}
	pthread_mutex_unlock(&gc_lock);
	gc_said(job, EVENT_GC_DONE, 0, NULL, 0u);
	free(job);
	return NULL;
}

static void handle_gc(client *c, uint32_t ch)
{
	gc_job   *job = malloc(sizeof *job);
	pthread_t thread;

	if (job == NULL) {
		send_error(c, ch, "out of memory");
		return;
	}
	job->client     = (unsigned)(c - clients);
	job->generation = client_generation[job->client];
	job->ch         = ch;
	if (pthread_create(&thread, NULL, run_gc, job) != 0) {
		free(job);
		send_error(c, ch, "no thread for gc");
		return;
	}
	pthread_detach(thread);
}

static void handle(client *c, const hull_sexpr_doc *body)
{
	hull_sexpr_form value;
	long long       ch = 0;

	if (!hull_msg_find(body, "ch", &value) || !hull_msg_long(body, &value, &ch) || ch < 0 ||
	    ch > 0xFFFFFFFFll) {
		send_error(c, 0u, "a message carries its channel");
		return;
	}
	if (hull_msg_is(body, "hello")) {
		hull_msg m = { 0 };

		hull_msg_raw(&m, "(5:hello(2:ch1:0)(12:core_version");
		hull_msg_text(&m, HULL_COMMIT);
		hull_msg_raw(&m, ")(5:proto1:1))");
		send_to(c, &m);
	} else if (ch == 0) {
		send_error(c, 0u, "channel 0 is for hello");
	} else if (hull_msg_is(body, "run")) {
		handle_run(c, (uint32_t)ch, body);
	} else if (hull_msg_is(body, "attach")) {
		handle_attach(c, (uint32_t)ch, body);
	} else if (hull_msg_is(body, "logs")) {
		handle_logs(c, (uint32_t)ch, body);
	} else if (hull_msg_is(body, "stop")) {
		handle_stop(c, (uint32_t)ch, body);
	} else if (hull_msg_is(body, "list")) {
		handle_list(c, (uint32_t)ch);
	} else if (hull_msg_is(body, "exec")) {
		handle_exec(c, (uint32_t)ch, body, 0);
	} else if (hull_msg_is(body, "dial")) {
		handle_exec(c, (uint32_t)ch, body, 1);
	} else if (hull_msg_is(body, "data")) {
		handle_input(c, (uint32_t)ch, body, 0);
	} else if (hull_msg_is(body, "eof")) {
		handle_input(c, (uint32_t)ch, body, 1);
	} else if (hull_msg_is(body, "credit")) {
		handle_credit(c, (uint32_t)ch, body);
	} else if (hull_msg_is(body, "winch")) {
		handle_control(c, (uint32_t)ch, body, 1);
	} else if (hull_msg_is(body, "signal")) {
		handle_control(c, (uint32_t)ch, body, 0);
	} else if (hull_msg_is(body, "kill")) {
		handle_kill(c, (uint32_t)ch, body);
	} else if (hull_msg_is(body, "pause")) {
		handle_pause(c, (uint32_t)ch, body, 1);
	} else if (hull_msg_is(body, "unpause")) {
		handle_pause(c, (uint32_t)ch, body, 0);
	} else if (hull_msg_is(body, "segment")) {
		handle_segment(c, (uint32_t)ch, body);
	} else if (hull_msg_is(body, "tamper")) {
		handle_tamper(c, (uint32_t)ch, body);
	} else if (hull_msg_is(body, "inject")) {
		handle_inject(c, (uint32_t)ch, body);
	} else if (hull_msg_is(body, "capture")) {
		handle_capture(c, (uint32_t)ch, body);
	} else if (hull_msg_is(body, "impair")) {
		handle_impair(c, (uint32_t)ch, body);
	} else if (hull_msg_is(body, "stats")) {
		handle_stats(c, (uint32_t)ch, body);
	} else if (hull_msg_is(body, "gc")) {
		handle_gc(c, (uint32_t)ch);
	} else {
		send_error(c, (uint32_t)ch, "unsupported");
	}
}

/* A frontend has gone: its commands with it, and its machines unless
 * they keep. */
static void disconnect(client *c)
{
	for (unsigned i = 0u; i < MAX_EXECS; i++)
		if (execs[i].used && execs[i].client == c)
			close_exec(&execs[i]);
	for (unsigned i = 0u; i < MAX_MACHINES; i++) {
		slot    *s = &machines[i];
		unsigned kept = 0u;

		if (!s->used)
			continue;
		for (unsigned f = 0u; f < s->follower_count; f++) {
			if (s->followers[f].client != c)
				s->followers[kept++] = s->followers[f];
			else if (s->followers[f].holds)
				release_hold(s);
		}
		s->follower_count = kept;
		kept = 0u;
		for (unsigned f = 0u; f < s->stop_count; f++)
			if (s->stops[f].client != c)
				s->stops[kept++] = s->stops[f];
		s->stop_count = kept;
		if (s->owner == c) {
			s->owner = NULL;
			if (!s->keep)
				stop_machine(s, 10000);
		}
	}
	hull_wire_close(&c->wire);
	free(c->out);
	memset(c, 0, sizeof *c);
	/* The closed commands' exits are queued: seen now, before any new
	 * stream could be given a freed one's pointer. */
	drain_events();
}

/* --- the loop ------------------------------------------------------------ */

/* Write what every frontend has waiting, and let go of those that went. */
static void flush_all(void)
{
	for (unsigned i = 0u; i < MAX_CLIENTS; i++)
		if (clients[i].used)
			flush(&clients[i]);
	for (unsigned i = 0u; i < MAX_CLIENTS; i++)
		if (clients[i].used && clients[i].dead)
			disconnect(&clients[i]);
}

static volatile sig_atomic_t ending;

static void end(int signal)
{
	(void)signal;
	ending = 1;
}

static int running_machines(void)
{
	for (unsigned i = 0u; i < MAX_MACHINES; i++)
		if (machines[i].used && !machines[i].exited)
			return 1;
	return 0;
}

static int any_clients(void)
{
	for (unsigned i = 0u; i < MAX_CLIENTS; i++)
		if (clients[i].used)
			return 1;
	return 0;
}

/* hull/hosts, section 8.1's names for nss/nss_hull.c: a line per machine
 * the core holds, `ID ADDRESS`, as `ps` lists them. Rewritten whole, by a
 * rename, whenever the listing has changed, so a reader never sees half of
 * one; written only by the core that holds the lock, and removed by it as
 * it goes, before the lock is free. */
static char hosts_path[4200];
static char hosts_written[MAX_MACHINES * 96u];
static int  hosts_current;

static void sync_hosts(void)
{
	char   listing[sizeof hosts_written];
	char   fresh[sizeof hosts_path + 8u];
	size_t n = 0u;
	int    fd;

	listing[0] = '\0';
	for (unsigned i = 0u; i < MAX_MACHINES; i++) {
		const slot *s = &machines[i];

		if (s->used && s->address[0] != '\0')
			n += (size_t)snprintf(listing + n, sizeof listing - n, "%s %s\n", s->id, s->address);
	}
	if (hosts_path[0] == '\0' || (hosts_current && strcmp(listing, hosts_written) == 0))
		return;
	snprintf(fresh, sizeof fresh, "%s.new", hosts_path);
	fd = open(fresh, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
	if (fd < 0)
		return;
	if (write(fd, listing, n) != (ssize_t)n || close(fd) != 0 || rename(fresh, hosts_path) != 0) {
		unlink(fresh);
		return;  /* tried again the next time round */
	}
	snprintf(hosts_written, sizeof hosts_written, "%s", listing);
	hosts_current = 1;
}

/* The idle clock's: in whole seconds, an idle time begun at x.999 would be
 * over at the next tick, a millisecond later. */
static long long monotonic_ms(void)
{
	struct timespec now;

	clock_gettime(CLOCK_MONOTONIC, &now);
	return (long long)now.tv_sec * 1000LL + now.tv_nsec / 1000000L;
}

/* The socket a frontend dials, `path` in `dir`, listened on: the fd, or
 * -1 with errno. A socket's path has 108 bytes, and a runtime directory
 * can be deeper: then it is named through a descriptor on its directory,
 * which the frontend does too. */
static int listen_at(const char *dir, const char *path)
{
	struct sockaddr_un address;
	int                listener = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	int                at = -1;
	int                saved;

	memset(&address, 0, sizeof address);
	address.sun_family = AF_UNIX;
	if (strlen(path) < sizeof address.sun_path) {
		strcpy(address.sun_path, path);
	} else if ((at = open(dir, O_PATH | O_DIRECTORY | O_CLOEXEC)) >= 0) {
		snprintf(address.sun_path, sizeof address.sun_path, "/proc/self/fd/%d/core.sock", at);
	}
	if (listener >= 0 && address.sun_path[0] != '\0' &&
	    bind(listener, (struct sockaddr *)&address, sizeof address) == 0 &&
	    chmod(path, 0600) == 0 && listen(listener, 16) == 0) {
		if (at >= 0)
			close(at);
		return listener;
	}
	saved = errno;
	if (listener >= 0)
		close(listener);
	if (at >= 0)
		close(at);
	errno = saved;
	return -1;
}

/*
 * Whether this core still holds its place, checked about once a second:
 * one core per user rests on the lock file being the one this core
 * locked, and on the socket path naming this core's socket. Something
 * outside that removes either -- a cleanup, a test, a hand -- lets a
 * second core start beside this one, or leaves this one running its
 * machines where no frontend can reach them; seen once, 2026-10-29, as a
 * machine ticking in one core while `ps` and `pause` reached another.
 * Each is said once in core.log, with this core's pid. A socket removed
 * while the lock is still this core's is bound again, since no other core
 * can hold that lock; a lock file replaced is only said, since another
 * core may by now hold the new one, and two cannot both rebind.
 */
#define PLACE_CHECK_MS 1000LL

static struct stat place_lock;
static struct stat place_socket;
static int         place_said_lock;
static int         place_said_socket;

static int same_file(const struct stat *a, const struct stat *b)
{
	return a->st_dev == b->st_dev && a->st_ino == b->st_ino;
}

static void check_place(const char *dir, const char *path, const char *lock_path, int *listener)
{
	struct stat now;
	int         lock_ours = stat(lock_path, &now) == 0 && same_file(&now, &place_lock);

	if (!lock_ours && !place_said_lock) {
		fprintf(stderr, "hull core %ld: core.lock is no longer the file this core locked: "
		        "another core can start beside this one\n", (long)getpid());
		place_said_lock = 1;
	}
	if (stat(path, &now) == 0) {
		if (!same_file(&now, &place_socket) && !place_said_socket) {
			fprintf(stderr, "hull core %ld: core.sock is no longer this core's socket: "
			        "frontends reach another core\n", (long)getpid());
			place_said_socket = 1;
		}
		return;
	}
	if (errno != ENOENT)
		return;
	if (lock_ours) {
		int fresh = listen_at(dir, path);

		if (fresh >= 0 && stat(path, &place_socket) == 0) {
			close(*listener);
			*listener = fresh;
			fprintf(stderr, "hull core %ld: core.sock was removed while this core ran; "
			        "bound again\n", (long)getpid());
			place_said_socket = 0;
			return;
		}
		if (fresh >= 0)
			close(fresh);
	}
	if (!place_said_socket) {
		fprintf(stderr, "hull core %ld: core.sock was removed while this core ran, and "
		        "cannot be bound again: no frontend can reach this core\n", (long)getpid());
		place_said_socket = 1;
	}
}

int hull_core_run(const char *host_arch)
{
	char               dir[4096];
	char               path[4200];
	char               lock_path[4200];
	struct sigaction   action;
	const char        *runtime = getenv("XDG_RUNTIME_DIR");
	const char        *idle_text = getenv("HULL_CORE_IDLE");
	long long          idle = (idle_text != NULL ? strtol(idle_text, NULL, 10) : 30) * 1000LL;
	long long          idle_since = monotonic_ms();
	const char        *reason = NULL;
	int                lock;
	int                listener;
	int                logged = 0;
	long long          place_checked = monotonic_ms();

	/* A started core's log, named by the frontend that started it, which
	 * leaves its own output at /dev/null: appended to until this core
	 * holds the lock, so that a core that gives way, or one that fails
	 * before it, adds its line after the live core's rather than erasing
	 * them, as truncating it on opening once did. */
	{
		const char *log_path = getenv("HULL_CORE_LOG");
		int         log = log_path != NULL ?
		                  open(log_path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600) : -1;

		if (log >= 0) {
			dup2(log, 1);
			dup2(log, 2);
			close(log);
			logged = 1;
		}
		unsetenv("HULL_CORE_LOG");
	}

	if (runtime == NULL || runtime[0] != '/') {
		fprintf(stderr, "hull core: XDG_RUNTIME_DIR is not set\n");
		return 1;
	}
	snprintf(dir, sizeof dir, "%s/hull", runtime);
	if (mkdir(dir, 0700) != 0 && errno != EEXIST) {
		fprintf(stderr, "hull core: %s: %s\n", dir, strerror(errno));
		return 1;
	}
	/* One core per user: the lock is held for the core's life. A core
	 * that is going removes its socket before anything else, so a
	 * frontend that comes meanwhile starts this one. While the lock is
	 * held and there is no socket, this one waits for the lock, a
	 * minute at most, rather than leave that frontend with no core. A
	 * core with its socket is the one wanted, and this one gives way. */
	snprintf(path, sizeof path, "%s/core.sock", dir);
	snprintf(lock_path, sizeof lock_path, "%s/core.lock", dir);
	lock = open(lock_path, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
	for (int tries = 0; lock >= 0 && flock(lock, LOCK_EX | LOCK_NB) != 0; tries++) {
		struct timespec pause = { 0, 50000000L };

		if ((errno != EWOULDBLOCK && errno != EINTR) || tries == 1200 ||
		    access(path, F_OK) == 0) {
			close(lock);
			lock = -1;
			break;
		}
		nanosleep(&pause, NULL);
	}
	if (lock < 0) {
		fprintf(stderr, "hull core %ld: another core holds the lock; giving way\n",
		        (long)getpid());
		return 0;  /* another core has it, which is what was wanted */
	}
	/* The log is this core's from here: what one before it said is gone. */
	if (logged && ftruncate(2, 0) != 0) {
		/* Appended to, then, which loses nothing. */
	}
	fprintf(stderr, "hull core %ld: started\n", (long)getpid());
	if (ftruncate(lock, 0) == 0) {
		char pid[24];
		int  n = snprintf(pid, sizeof pid, "%ld\n", (long)getpid());

		if (write(lock, pid, (size_t)n) != n) {
			/* The pid is a convenience; the lock is what counts. */
		}
	}

	unlink(path);  /* a socket left by a core that died; the lock is ours */
	snprintf(hosts_path, sizeof hosts_path, "%s/hosts", dir);
	if ((listener = listen_at(dir, path)) < 0 || fstat(lock, &place_lock) != 0 ||
	    stat(path, &place_socket) != 0) {
		fprintf(stderr, "hull core: %s: %s\n", path, strerror(errno));
		return 1;
	}
	if (pipe2(wake, O_CLOEXEC | O_NONBLOCK) != 0)
		return 1;

	hull_registry_init(&registry, host_arch);
	if (hull_registry_add(&registry, hull_engine_docker(&host_ops), &reason) != HULL_REGISTRY_OK ||
	    hull_registry_add(&registry, hull_engine_qemu(&host_ops), &reason) != HULL_REGISTRY_OK ||
	    hull_registry_add(&registry, hull_engine_chroot(&host_ops), &reason) != HULL_REGISTRY_OK ||
	    hull_registry_add(&registry, hull_engine_dosbox(&host_ops), &reason) != HULL_REGISTRY_OK ||
	    hull_registry_add(&registry, hull_engine_dosemu(&host_ops), &reason) != HULL_REGISTRY_OK ||
	    hull_registry_set_default(&registry, "ns", "chroot") != HULL_REGISTRY_OK) {
		fprintf(stderr, "hull core: a built-in engine is refused: %s\n", reason);
		return 1;
	}
	snprintf(kept_dir, sizeof kept_dir, "%s/machines", dir);
	if (mkdir(kept_dir, 0700) != 0 && errno != EEXIST)
		kept_dir[0] = '\0';  /* nothing is kept, and nothing taken back */
	else
		adopt_kept();
	snprintf(secrets_root, sizeof secrets_root, "%s/secrets", dir);
	if (mkdir(secrets_root, 0700) != 0 && errno != EEXIST)
		secrets_root[0] = '\0';  /* no machine with secrets can run */
	else
		remove_stale_secrets();
	snprintf(ssh_root, sizeof ssh_root, "%s/ssh", dir);
	if (mkdir(ssh_root, 0700) != 0 && errno != EEXIST)
		ssh_root[0] = '\0';  /* no machine with (ssh true) can run */
	{
		/* The persistent per-machine host keys, where the store is, so
		 * they outlive a core and a reboot as machine state (section 8.1
		 * item 2): $XDG_DATA_HOME/hull/machines or ~/.local/share's. Empty
		 * if neither says where, and then a host key is ephemeral. */
		const char *data = getenv("XDG_DATA_HOME");
		const char *home = getenv("HOME");
		char        base[4100];

		base[0] = '\0';
		if (data != NULL && data[0] == '/')
			snprintf(base, sizeof base, "%s/hull", data);
		else if (home != NULL && home[0] == '/')
			snprintf(base, sizeof base, "%s/.local/share/hull", home);
		/* The data home and its machines directory, made whole, the data
		 * home's parents too, as the XDG spec has a program do where they
		 * are not there yet; empty host_key_root means ephemeral host keys
		 * and moved addresses kept for the run only. */
		if (base[0] == '\0' || make_dirs(base) != 0)
			host_key_root[0] = '\0';
		else {
			snprintf(host_key_root, sizeof host_key_root, "%s/machines", base);
			if (mkdir(host_key_root, 0700) != 0 && errno != EEXIST)
				host_key_root[0] = '\0';
		}
	}

	memset(&action, 0, sizeof action);
	action.sa_handler = end;
	sigaction(SIGINT, &action, NULL);
	sigaction(SIGTERM, &action, NULL);
	action.sa_handler = SIG_IGN;
	sigaction(SIGPIPE, &action, NULL);
	sigaction(SIGHUP, &action, NULL);

	/* Idle from now, when a frontend can reach it: a core that waited
	 * for the lock has a frontend coming, which an idle clock started
	 * before the wait would have it leave before it could arrive. */
	idle_since = monotonic_ms();
	while (!ending) {
		struct pollfd fds[2u + MAX_CLIENTS];
		client       *owners[2u + MAX_CLIENTS];
		nfds_t        n = 0u;
		int           timeout = -1;
		int           busy;

		/* Before each wait, so that the first, before any request, replaces
		 * what a core that died left, which is not true. */
		sync_hosts();
		fds[n++] = (struct pollfd){ listener, POLLIN, 0 };
		fds[n++] = (struct pollfd){ wake[0], POLLIN, 0 };
		for (unsigned i = 0u; i < MAX_CLIENTS; i++) {
			if (clients[i].used) {
				short events = POLLIN;

				if (clients[i].out_sent < clients[i].out_length)
					events |= POLLOUT;
				owners[n] = &clients[i];
				fds[n++]  = (struct pollfd){ clients[i].wire.fd, events, 0 };
			}
		}
		busy = any_clients() || running_machines();
		if (!busy) {
			long long waited = monotonic_ms() - idle_since;

			if (waited >= idle)
				break;
			timeout = (int)(idle - waited);
		}
		/* Woken at least once a second to see that its place is its. */
		if (monotonic_ms() - place_checked >= PLACE_CHECK_MS) {
			check_place(dir, path, lock_path, &listener);
			fds[0].fd     = listener;
			place_checked = monotonic_ms();
		}
		if (timeout < 0 || timeout > (int)PLACE_CHECK_MS)
			timeout = (int)PLACE_CHECK_MS;
		if (poll(fds, n, timeout) < 0) {
			if (errno == EINTR)
				continue;
			break;
		}
		/* Idle from the end of the wait that was busy, not its start: a
		 * client quiet before it left, or a machine before it ended, is
		 * not idle time. */
		if (busy)
			idle_since = monotonic_ms();
		if (fds[1].revents != 0)
			drain_events();
		if (fds[0].revents & POLLIN) {
			/* Non-blocking, so that a frontend that stops reading
			 * stalls nothing: what it is sent waits in its queue. */
			int fd = accept4(listener, NULL, NULL, SOCK_CLOEXEC | SOCK_NONBLOCK);

			for (unsigned i = 0u; fd >= 0 && i < MAX_CLIENTS; i++) {
				if (!clients[i].used) {
					if (hull_wire_open(&clients[i].wire, fd, HULL_WIRE_FROM_CLIENT) == 0) {
						clients[i].used = 1;
						client_generation[i]++;
						fd = -1;
					}
				}
			}
			if (fd >= 0)
				close(fd);
		}
		for (nfds_t i = 2u; i < n; i++) {
			client          *c = owners[i];
			hull_wire_status status;
			hull_sexpr_doc   body;

			if ((fds[i].revents & (POLLIN | POLLHUP | POLLERR)) == 0 || !c->used)
				continue;
			status = hull_wire_fill(&c->wire);
			while (status == HULL_WIRE_OK &&
			       (status = hull_wire_next(&c->wire, &body)) == HULL_WIRE_OK)
				handle(c, &body);
			if (status == HULL_WIRE_CLOSED || status == HULL_WIRE_BROKEN)
				c->dead = 1;
		}
		flush_all();
	}

	fprintf(stderr, "hull core %ld: going\n", (long)getpid());
	/* Going. A frontend that comes now must not reach a core that will
	 * never answer it, so this one stops listening at once; one that
	 * comes starts a core of its own, which waits for the lock. */
	unlink(path);
	close(listener);

	/* Every machine still running is stopped, and waited for: let go of
	 * first, since no run will read what they write now. */
	for (unsigned i = 0u; i < MAX_MACHINES; i++)
		if (machines[i].used)
			release_hold(&machines[i]);
	/* Every machine still running is stopped, and waited for. */
	for (unsigned i = 0u; i < MAX_MACHINES; i++)
		if (machines[i].used && !machines[i].exited)
			stop_machine(&machines[i], 10000);
	while (running_machines()) {
		struct pollfd fd = { wake[0], POLLIN, 0 };

		if (poll(&fd, 1u, 30000) <= 0)
			break;
		drain_events();
		flush_all();
	}
	/* What the machines' ends left to say, given a second a frontend. */
	for (unsigned i = 0u; i < MAX_CLIENTS; i++) {
		struct timeval patience = { 1, 0 };
		int            flags;

		if (!clients[i].used)
			continue;
		flags = fcntl(clients[i].wire.fd, F_GETFL);
		fcntl(clients[i].wire.fd, F_SETFL, flags & ~O_NONBLOCK);
		setsockopt(clients[i].wire.fd, SOL_SOCKET, SO_SNDTIMEO, &patience, sizeof patience);
		flush(&clients[i]);
	}
	unlink(hosts_path);
	close(lock);
	return 0;
}
