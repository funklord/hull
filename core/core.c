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

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include "access.h"
#include "builtin.h"
#include "core.h"
#include "registry.h"
#include "wire.h"

#define MAX_CLIENTS      32u
#define MAX_MACHINES     64u
#define MAX_ATTACHED     16u
#define MAX_STOPS        16u
#define MAX_EXECS        64u
#define LOG_LIMIT        (1u << 20)
#define DATA_CHUNK       65536u
#define INPUT_WINDOW     65536u  /* an exec's stdin the core holds, at most */
#define OUTPUT_WINDOW    65536u  /* an exec's output a frontend has unasked */
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
	const hull_engine *engine;
	char               class_name[8];
	hull_core          core;
	hull_machine      *handle;
	char               state[16];
	int                keep;        /* (on_disconnect keep) */
	client            *owner;       /* the frontend that ran it */
	int                exited;
	int                code;
	int                signal;
	follower           followers[MAX_ATTACHED];
	unsigned           follower_count;
	follower           stops[MAX_STOPS];  /* answered `done` at the exit */
	unsigned           stop_count;
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
static slot          machines[MAX_MACHINES];
static exec_entry    execs[MAX_EXECS];
static hull_registry registry;

/* An engine's reader can report a stream's first output before exec()
 * has returned it. The loop makes one exec() at a time, so a stream no
 * entry knows is the one under way, which `adopting` is. */
static pthread_mutex_t flow_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  flow_room = PTHREAD_COND_INITIALIZER;
static exec_entry     *adopting;

static exec_entry *exec_of(hull_stream *stream);

/* --- events from engine threads ------------------------------------------ */

typedef enum event_kind { EVENT_DATA, EVENT_EXIT, EVENT_STATE } event_kind;

typedef struct event {
	event_kind     kind;
	slot          *slot;
	unsigned       generation;
	hull_stream   *stream;  /* an exec's, or NULL for the machine's own */
	int            fd;
	int            code;
	int            signal;
	char           state[16];
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

static const hull_host_ops host_ops = {
	.struct_size = sizeof (hull_host_ops),
	.on_data     = on_data,
	.on_eof      = on_eof,
	.on_exit     = on_exit_status,
	.on_state    = on_state,
	.log         = on_log,
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
		s->followers[s->follower_count++] = (follower){ c, ch, at, until };
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

static void stop_machine(slot *s, int timeout_ms)
{
	stop_job *job = malloc(sizeof *job);
	pthread_t thread;

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

/* The machine has ended: tell whoever is waiting, and let the engine go. */
static void machine_exited(slot *s, int code, int signal)
{
	hull_error error = { .struct_size = sizeof error };

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

static void handle_run(client *c, uint32_t ch, const hull_sexpr_doc *body)
{
	hull_sexpr_form    value;
	hull_sexpr_form    plan_form;
	hull_sexpr_doc     plan_doc;
	hull_sexpr_form    machine;
	hull_error         error = { .struct_size = sizeof error };
	const hull_engine *engine = NULL;
	char               id[64];
	char              *engine_name;
	char              *class_name;
	char              *on_disconnect;
	unsigned           generation;
	slot              *s;
	hull_buf           plan;

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
	}
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
	generation = s->generation + 1u;
	memset(s, 0, sizeof *s);
	s->used            = 1;
	s->generation      = generation;
	s->engine          = engine;
	s->core.slot       = s;
	s->core.generation = generation;
	s->owner     = c;
	snprintf(s->id, sizeof s->id, "%s", id);
	snprintf(s->class_name, sizeof s->class_name, "%s", class_name);
	snprintf(s->state, sizeof s->state, "creating");
	free(class_name);
	on_disconnect = hull_access_value(&plan_doc, &machine, "on_disconnect");
	s->keep = on_disconnect != NULL && strcmp(on_disconnect, "keep") == 0;
	free(on_disconnect);

	if (engine->ops->create(&s->core, plan, &s->handle, &error) != HULL_OK) {
		send_error(c, ch, error.detail != NULL ? error.detail : "create failed");
		s->used = 0;
		return;
	}
	add_follower(s, c, ch, 0u, FOLLOW_ON);
	if (engine->ops->start(&s->core, s->handle, &error) != HULL_OK) {
		send_error(c, ch, error.detail != NULL ? error.detail : "start failed");
		engine->ops->destroy(&s->core, s->handle, &error);
		s->handle         = NULL;
		s->used           = 0;
		s->follower_count = 0u;
		return;
	}
	send_started(c, ch, s);
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
		s->stops[s->stop_count++] = (follower){ c, ch, 0u, 0u };
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

static void handle_exec(client *c, uint32_t ch, const hull_sexpr_doc *body)
{
	hull_error   error = { .struct_size = sizeof error };
	hull_stream *stream = NULL;
	exec_entry  *x = NULL;
	slot        *s = named(c, ch, body);
	hull_buf     spec;
	hull_status  status;

	if (s == NULL)
		return;
	if (s->exited || s->handle == NULL) {
		send_error(c, ch, "the machine is not running");
		return;
	}
	if (!terminal_ok(body)) {
		send_error(c, ch, "a window is 1 to 65535 in each direction");
		return;
	}
	if (s->engine->ops->exec == NULL || s->engine->ops->stream_write == NULL ||
	    s->engine->ops->stream_eof == NULL || s->engine->ops->stream_close == NULL) {
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
	status = s->engine->ops->exec(&s->core, s->handle, spec, &stream, &error);
	pthread_mutex_lock(&flow_lock);
	adopting = NULL;
	if (status == HULL_OK)
		x->stream = stream;
	else
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
	pthread_mutex_lock(&flow_lock);
	x->output_open = 1;
	pthread_mutex_unlock(&flow_lock);
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
	} else if (s->engine->ops->signal(&s->core, s->handle, NULL, number, &error) != HULL_OK) {
		send_error(c, ch, error.detail != NULL ? error.detail : "refused");
	} else {
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
	if (s->engine->ops->stats == NULL) {
		send_error(c, ch, "that engine does not measure a machine");
		return;
	}
	if (s->engine->ops->stats(&s->core, s->handle, &out, &error) != HULL_OK) {
		send_error(c, ch, error.detail != NULL ? error.detail : "refused");
		return;
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

static void handle_list(client *c, uint32_t ch)
{
	hull_msg m = { 0 };

	hull_msg_raw(&m, "(8:machines");
	hull_msg_channel(&m, ch);
	for (unsigned i = 0u; i < MAX_MACHINES; i++) {
		const slot *s = &machines[i];

		if (!s->used)
			continue;
		hull_msg_raw(&m, "(7:machine(5:class");
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
		hull_msg_text(&m, HULL_VERSION);
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
		handle_exec(c, (uint32_t)ch, body);
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
	} else if (hull_msg_is(body, "stats")) {
		handle_stats(c, (uint32_t)ch, body);
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
		for (unsigned f = 0u; f < s->follower_count; f++)
			if (s->followers[f].client != c)
				s->followers[kept++] = s->followers[f];
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

int hull_core_run(const char *host_arch)
{
	char               dir[4096];
	char               path[4200];
	char               lock_path[4200];
	struct sockaddr_un address;
	struct sigaction   action;
	const char        *runtime = getenv("XDG_RUNTIME_DIR");
	const char        *idle_text = getenv("HULL_CORE_IDLE");
	long               idle = idle_text != NULL ? strtol(idle_text, NULL, 10) : 30;
	time_t             idle_since = time(NULL);
	const char        *reason = NULL;
	int                lock;
	int                listener;

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
	if (lock < 0)
		return 0;  /* another core has it, which is what was wanted */
	if (ftruncate(lock, 0) == 0) {
		char pid[24];
		int  n = snprintf(pid, sizeof pid, "%ld\n", (long)getpid());

		if (write(lock, pid, (size_t)n) != n) {
			/* The pid is a convenience; the lock is what counts. */
		}
	}

	unlink(path);  /* a socket left by a core that died; the lock is ours */
	listener = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	memset(&address, 0, sizeof address);
	address.sun_family = AF_UNIX;
	/* A socket's path has 108 bytes, and a runtime directory can be
	 * deeper: then it is named through a descriptor on its directory,
	 * which the frontend does too. */
	if (strlen(path) < sizeof address.sun_path) {
		strcpy(address.sun_path, path);
	} else {
		int at = open(dir, O_PATH | O_DIRECTORY | O_CLOEXEC);

		snprintf(address.sun_path, sizeof address.sun_path, "/proc/self/fd/%d/core.sock", at);
	}
	if (listener < 0 || bind(listener, (struct sockaddr *)&address, sizeof address) != 0 ||
	    chmod(path, 0600) != 0 || listen(listener, 16) != 0) {
		fprintf(stderr, "hull core: %s: %s\n", path, strerror(errno));
		return 1;
	}
	if (pipe2(wake, O_CLOEXEC | O_NONBLOCK) != 0)
		return 1;

	hull_registry_init(&registry, host_arch);
	if (hull_registry_add(&registry, hull_engine_docker(&host_ops), &reason) != HULL_REGISTRY_OK ||
	    hull_registry_add(&registry, hull_engine_qemu(&host_ops), &reason) != HULL_REGISTRY_OK ||
	    hull_registry_add(&registry, hull_engine_chroot(&host_ops), &reason) != HULL_REGISTRY_OK) {
		fprintf(stderr, "hull core: a built-in engine is refused: %s\n", reason);
		return 1;
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
	idle_since = time(NULL);
	while (!ending) {
		struct pollfd fds[2u + MAX_CLIENTS];
		client       *owners[2u + MAX_CLIENTS];
		nfds_t        n = 0u;
		int           timeout = -1;

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
		if (!any_clients() && !running_machines()) {
			long waited = (long)(time(NULL) - idle_since);

			if (waited >= idle)
				break;
			timeout = (int)((idle - waited) * 1000);
		} else {
			idle_since = time(NULL);
		}
		if (poll(fds, n, timeout) < 0) {
			if (errno == EINTR)
				continue;
			break;
		}
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

	/* Going. A frontend that comes now must not reach a core that will
	 * never answer it, so this one stops listening at once; one that
	 * comes starts a core of its own, which waits for the lock. */
	unlink(path);
	close(listener);

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
	close(lock);
	return 0;
}
