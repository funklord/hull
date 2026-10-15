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
#define LOG_LIMIT        (1u << 20)
#define DATA_CHUNK       65536u

/* --- the table ----------------------------------------------------------- */

typedef struct client {
	int        used;
	hull_wire  wire;
	int        dead;  /* a send failed: closed at the end of this turn */
} client;

typedef struct follower {
	client   *client;
	uint32_t  ch;
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
};

static client        clients[MAX_CLIENTS];
static slot          machines[MAX_MACHINES];
static hull_registry registry;

/* --- events from engine threads ------------------------------------------ */

typedef enum event_kind { EVENT_DATA, EVENT_EXIT, EVENT_STATE } event_kind;

typedef struct event {
	event_kind     kind;
	slot          *slot;
	unsigned       generation;
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

	(void)stream;
	if (e == NULL || e->slot == NULL) {
		free(e);
		return;
	}
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
	(void)stream;
	if (e == NULL || e->slot == NULL) {
		free(e);
		return;
	}
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

static void send_to(client *c, hull_msg *m)
{
	if (c->used && !c->dead && hull_wire_send(c->wire.fd, m) != 0)
		c->dead = 1;
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

static void send_exited(client *c, uint32_t ch, const slot *s)
{
	hull_msg m = { 0 };

	hull_msg_raw(&m, "(6:exited");
	hull_msg_channel(&m, ch);
	hull_msg_raw(&m, "(4:code");
	hull_msg_number(&m, s->code);
	hull_msg_raw(&m, ")(6:signal");
	hull_msg_number(&m, s->signal);
	hull_msg_raw(&m, "))");
	send_to(c, &m);
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
	s->log_tail = NULL;
	s->log_size = 0u;
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
	while (s->log_size > LOG_LIMIT && s->log != r) {
		record *old = s->log;

		s->log = old->next;
		s->log_size -= old->length;
		free(old);
	}
}

static void add_follower(slot *s, client *c, uint32_t ch)
{
	if (s->follower_count < MAX_ATTACHED)
		s->followers[s->follower_count++] = (follower){ c, ch };
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
	for (unsigned i = 0u; i < s->follower_count; i++)
		send_exited(s->followers[i].client, s->followers[i].ch, s);
	for (unsigned i = 0u; i < s->stop_count; i++)
		send_simple(s->stops[i].client, "(4:done", s->stops[i].ch);
	s->follower_count = 0u;
	s->stop_count     = 0u;
	s->owner          = NULL;
	if (s->handle != NULL) {
		s->engine->ops->destroy(&s->core, s->handle, &error);
		s->handle = NULL;
	}
}

static void drain_events(void)
{
	event *e;
	char   bytes[64];

	while (read(wake[0], bytes, sizeof bytes) > 0)
		;
	pthread_mutex_lock(&queue_lock);
	e = queue_head;
	queue_head = queue_tail = NULL;
	pthread_mutex_unlock(&queue_lock);
	while (e != NULL) {
		event *next = e->next;
		slot  *s    = e->slot;

		/* An engine's thread can report a last state after the exit, by
		 * when the slot may hold another machine: that is not news. */
		if (s->generation != e->generation) {
			free(e);
			e = next;
			continue;
		}
		switch (e->kind) {
		case EVENT_DATA:
			keep_log(s, e->fd, e->bytes, e->length);
			for (unsigned i = 0u; i < s->follower_count; i++)
				send_data(s->followers[i].client, s->followers[i].ch, e->fd, e->bytes,
				          e->length);
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
	add_follower(s, c, ch);
	if (engine->ops->start(&s->core, s->handle, &error) != HULL_OK) {
		send_error(c, ch, error.detail != NULL ? error.detail : "start failed");
		engine->ops->destroy(&s->core, s->handle, &error);
		s->handle = NULL;
		s->used   = 0;
		return;
	}
	{
		hull_msg m = { 0 };

		hull_msg_raw(&m, "(7:started");
		hull_msg_channel(&m, ch);
		hull_msg_raw(&m, "(7:machine");
		hull_msg_text(&m, s->id);
		hull_msg_raw(&m, "))");
		send_to(c, &m);
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
		add_follower(s, c, ch);
}

static void handle_logs(client *c, uint32_t ch, const hull_sexpr_doc *body)
{
	hull_sexpr_form value;
	slot           *s = named(c, ch, body);
	int             follow;

	if (s == NULL)
		return;
	follow = hull_msg_find(body, "follow", &value) && hull_sexpr_is(body, &value, "true");
	for (record *r = s->log; r != NULL; r = r->next)
		send_data(c, ch, r->fd, r->bytes, r->length);
	if (follow && !s->exited)
		add_follower(s, c, ch);
	else if (follow)
		send_exited(c, ch, s);
	else
		send_simple(c, "(3:eof", ch);
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
		s->stops[s->stop_count++] = (follower){ c, ch };
	stop_machine(s, timeout < 0 ? 0 : timeout > 600000 ? 600000 : (int)timeout);
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
	} else {
		send_error(c, (uint32_t)ch, "unsupported");
	}
}

/* A frontend has gone: its machines go with it unless they keep. */
static void disconnect(client *c)
{
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
	c->used = 0;
	c->dead = 0;
}

/* --- the loop ------------------------------------------------------------ */

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
	/* One core per user: the lock is held for the core's life. */
	snprintf(path, sizeof path, "%s/core.lock", dir);
	lock = open(path, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
	if (lock < 0 || flock(lock, LOCK_EX | LOCK_NB) != 0) {
		if (lock >= 0)
			close(lock);
		return 0;  /* another core has it, which is what was wanted */
	}
	if (ftruncate(lock, 0) == 0) {
		char pid[24];
		int  n = snprintf(pid, sizeof pid, "%ld\n", (long)getpid());

		if (write(lock, pid, (size_t)n) != n) {
			/* The pid is a convenience; the lock is what counts. */
		}
	}

	snprintf(path, sizeof path, "%s/core.sock", dir);
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

	while (!ending) {
		struct pollfd fds[2u + MAX_CLIENTS];
		client       *owners[2u + MAX_CLIENTS];
		nfds_t        n = 0u;
		int           timeout = -1;

		fds[n++] = (struct pollfd){ listener, POLLIN, 0 };
		fds[n++] = (struct pollfd){ wake[0], POLLIN, 0 };
		for (unsigned i = 0u; i < MAX_CLIENTS; i++) {
			if (clients[i].used) {
				owners[n] = &clients[i];
				fds[n++]  = (struct pollfd){ clients[i].wire.fd, POLLIN, 0 };
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
			int fd = accept4(listener, NULL, NULL, SOCK_CLOEXEC);

			for (unsigned i = 0u; fd >= 0 && i < MAX_CLIENTS; i++) {
				if (!clients[i].used) {
					struct timeval patience = { 5, 0 };

					/* A frontend that stops reading is dropped
					 * rather than allowed to stall every other. */
					setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &patience, sizeof patience);
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

			if (fds[i].revents == 0 || !c->used)
				continue;
			status = hull_wire_fill(&c->wire);
			while (status == HULL_WIRE_OK &&
			       (status = hull_wire_next(&c->wire, &body)) == HULL_WIRE_OK)
				handle(c, &body);
			if (status == HULL_WIRE_CLOSED || status == HULL_WIRE_BROKEN)
				c->dead = 1;
		}
		for (unsigned i = 0u; i < MAX_CLIENTS; i++)
			if (clients[i].used && clients[i].dead)
				disconnect(&clients[i]);
	}

	/* Going: every machine still running is stopped, and waited for. */
	for (unsigned i = 0u; i < MAX_MACHINES; i++)
		if (machines[i].used && !machines[i].exited)
			stop_machine(&machines[i], 10000);
	while (running_machines()) {
		struct pollfd fd = { wake[0], POLLIN, 0 };

		if (poll(&fd, 1u, 30000) <= 0)
			break;
		drain_events();
	}
	unlink(path);
	close(listener);
	close(lock);
	return 0;
}
