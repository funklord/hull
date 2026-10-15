/*
 * docker.c -- the docker adapter: the ns class through the Docker
 * daemon's API (project.md sections 5.0 and 4.9).
 *
 * HTTP/1.1 over the daemon's unix socket, one connection per request,
 * closed by the daemon after its answer. A machine is a container:
 * created from the plan, attached to before it starts so no output is
 * missed, started, and waited for once its output ends. stop() asks the
 * daemon to stop it, which is SIGTERM and then SIGKILL; destroy() removes
 * it by force. The container's name begins `hull-` and it carries the
 * label `hull.machine`, so one left by a hull that was killed can be
 * found and removed by name.
 *
 * The workload runs as the invoking user's uid and gid, not as root:
 * decided 2026-10-15, hull does everything it can as the user and asks
 * for root only at the very end of the line. Under a rootful daemon the
 * container's root is the host's, which is the end of a line no spec has
 * reached yet. So volumes and the files hull hands in are the user's own
 * on both sides, and capability grants are refused: docker gives them to
 * a root workload only.
 *
 * Output arrives on the attach stream in docker's frames -- a byte for
 * the stream, three of padding, a big-endian length, the bytes -- and
 * goes to the core through on_data, by a thread per machine that then
 * asks for the exit status and reports on_exit.
 */

#define _GNU_SOURCE

#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "access.h"
#include "builtin.h"

#define API           "/v1.41"
#define MAX_RESPONSE  (1u << 20)

struct hull_machine {
	hull_core       *core;
	char            *body;       /* the create request */
	char             name[96];
	char             id[80];     /* the container, once created */
	int              attach;     /* the attach stream, once started */
	pthread_t        reader;
	int              started;
	pthread_mutex_t  lock;
	pthread_cond_t   done_cond;
	int              done;
	char             detail[512];
};

static const hull_host_ops *host;
static unsigned             machines_made;

/* --- a growing string ---------------------------------------------------- */

typedef struct text {
	char   *data;
	size_t  length;
	size_t  cap;
	int     failed;
} text;

static void add(text *t, const char *bytes, size_t n)
{
	if (t->failed)
		return;
	if (t->length + n + 1u > t->cap) {
		size_t cap = t->cap == 0u ? 1024u : t->cap;
		char  *data;

		while (cap < t->length + n + 1u)
			cap *= 2u;
		data = realloc(t->data, cap);
		if (data == NULL) {
			t->failed = 1;
			return;
		}
		t->data = data;
		t->cap  = cap;
	}
	memcpy(t->data + t->length, bytes, n);
	t->length += n;
	t->data[t->length] = '\0';
}

static void add_text(text *t, const char *s)
{
	add(t, s, strlen(s));
}

static void addf(text *t, const char *format, ...)
{
	char    buffer[PATH_MAX + 64];
	va_list args;
	int     n;

	va_start(args, format);
	n = vsnprintf(buffer, sizeof buffer, format, args);
	va_end(args);
	if (n < 0 || (size_t)n >= sizeof buffer)
		t->failed = 1;
	else
		add(t, buffer, (size_t)n);
}

/* `other`'s text appended, and its failure carried. */
static void add_all(text *t, const text *other)
{
	if (other->data != NULL)
		add(t, other->data, other->length);
	t->failed |= other->failed;
}

/* A JSON string: quoted, with what JSON requires escaped. */
static void add_json(text *t, const char *s)
{
	add(t, "\"", 1u);
	for (; *s != '\0'; s++) {
		unsigned char c = (unsigned char)*s;

		if (c == '"' || c == '\\') {
			char escaped[2] = { '\\', (char)c };

			add(t, escaped, 2u);
		} else if (c < 0x20u) {
			addf(t, "\\u%04x", c);
		} else {
			add(t, s, 1u);
		}
	}
	add(t, "\"", 1u);
}

/* --- the daemon ---------------------------------------------------------- */

/* The daemon's socket: $DOCKER_HOST when it names a unix socket. */
static const char *socket_path(void)
{
	const char *docker_host = getenv("DOCKER_HOST");

	if (docker_host != NULL && strncmp(docker_host, "unix://", 7u) == 0)
		return docker_host + 7;
	return "/var/run/docker.sock";
}

static int dial(void)
{
	struct sockaddr_un address;
	const char        *path = socket_path();
	int                fd;

	if (strlen(path) >= sizeof address.sun_path)
		return -1;
	fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (fd < 0)
		return -1;
	memset(&address, 0, sizeof address);
	address.sun_family = AF_UNIX;
	strcpy(address.sun_path, path);
	if (connect(fd, (struct sockaddr *)&address, sizeof address) != 0) {
		close(fd);
		return -1;
	}
	return fd;
}

static int send_all(int fd, const char *data, size_t n)
{
	while (n > 0u) {
		ssize_t wrote = write(fd, data, n);

		if (wrote < 0 && errno == EINTR)
			continue;
		if (wrote <= 0)
			return -1;
		data += wrote;
		n -= (size_t)wrote;
	}
	return 0;
}

static int send_request(int fd, const char *method, const char *path, const char *body,
                        const char *extra)
{
	text request = { 0 };
	int  result;

	addf(&request, "%s " API "%s HTTP/1.1\r\nHost: docker\r\n%s", method, path, extra);
	if (body != NULL) {
		addf(&request, "Content-Type: application/json\r\nContent-Length: %zu\r\n\r\n",
		     strlen(body));
		add_text(&request, body);
	} else {
		add_text(&request, "Content-Length: 0\r\n\r\n");
	}
	result = request.failed ? -1 : send_all(fd, request.data, request.length);
	free(request.data);
	return result;
}

/* A chunked body, decoded in place. Returns its length, or -1. */
static long dechunk(char *body, size_t length)
{
	size_t in  = 0u;
	size_t out = 0u;

	for (;;) {
		char         *end;
		char         *line;
		unsigned long size;

		if (in >= length)
			return -1;
		size = strtoul(body + in, &end, 16);
		line = strstr(end, "\r\n");
		if (line == NULL || end == body + in)
			return -1;
		in = (size_t)(line - body) + 2u;
		if (size == 0u)
			return (long)out;
		if (size > length - in)
			return -1;
		memmove(body + out, body + in, size);
		out += size;
		in += size + 2u;
	}
}

/* One request, answered and closed. `status` is the HTTP status, and
 * `answer` the body, which the caller frees. Returns 0, or -1 when the
 * daemon could not be reached or answered with nonsense. */
static int call(const char *method, const char *path, const char *body, int *status,
                char **answer)
{
	text  reply = { 0 };
	char  buffer[4096];
	char *head_end;
	char *payload;
	long  length;
	int   fd = dial();

	*answer = NULL;
	*status = 0;
	if (fd < 0)
		return -1;
	if (send_request(fd, method, path, body, "Connection: close\r\n") != 0) {
		close(fd);
		return -1;
	}
	for (;;) {
		ssize_t n = read(fd, buffer, sizeof buffer);

		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0)
			break;
		add(&reply, buffer, (size_t)n);
		if (reply.length > MAX_RESPONSE)
			reply.failed = 1;
		if (reply.failed)
			break;
	}
	close(fd);
	if (reply.failed || reply.data == NULL || sscanf(reply.data, "HTTP/1.%*d %d", status) != 1 ||
	    (head_end = strstr(reply.data, "\r\n\r\n")) == NULL) {
		free(reply.data);
		return -1;
	}
	*head_end = '\0';
	payload = head_end + 4;
	length  = (long)(reply.length - (size_t)(payload - reply.data));
	if (strcasestr(reply.data, "\r\nTransfer-Encoding: chunked") != NULL)
		length = dechunk(payload, (size_t)length);
	if (length < 0) {
		free(reply.data);
		return -1;
	}
	payload[length] = '\0';
	memmove(reply.data, payload, (size_t)length + 1u);
	*answer = reply.data;
	return 0;
}

/* The string value of `"key":` in a flat JSON answer, copied to `out`. */
static int json_string(const char *json, const char *key, char *out, size_t cap)
{
	char        pattern[64];
	const char *at;
	size_t      n = 0u;

	snprintf(pattern, sizeof pattern, "\"%s\":\"", key);
	at = json != NULL ? strstr(json, pattern) : NULL;
	if (at == NULL)
		return 0;
	for (at += strlen(pattern); *at != '\0' && *at != '"' && n + 1u < cap; at++) {
		if (*at == '\\' && at[1] != '\0')
			at++;
		out[n++] = *at;
	}
	out[n] = '\0';
	return 1;
}

static int json_number(const char *json, const char *key, long *out)
{
	char        pattern[64];
	const char *at;

	snprintf(pattern, sizeof pattern, "\"%s\":", key);
	at = json != NULL ? strstr(json, pattern) : NULL;
	if (at == NULL)
		return 0;
	*out = strtol(at + strlen(pattern), NULL, 10);
	return 1;
}

static hull_status fail(hull_error *error, hull_status code, const char *detail)
{
	if (error != NULL) {
		error->code           = code;
		error->detail         = detail;
		error->runtime_detail = NULL;
	}
	return code;
}

/* The daemon's own words for a failed call, kept in `detail`. */
static const char *daemon_says(char *detail, size_t cap, const char *what, int status,
                               const char *answer)
{
	char message[400];

	if (!json_string(answer, "message", message, sizeof message))
		snprintf(message, sizeof message, "HTTP %d", status);
	snprintf(detail, cap, "%s: %s", what, message);
	return detail;
}

/* --- the plan, as a container -------------------------------------------- */

/* Whether `source` is a bare image ref: none of J.1's schemes. */
static int is_ref(const char *source)
{
	static const char *const schemes[] = { "dir:", "tar:", "pack:", "image:" };

	for (unsigned i = 0u; i < sizeof schemes / sizeof schemes[0]; i++)
		if (strncmp(source, schemes[i], strlen(schemes[i])) == 0)
			return 0;
	return 1;
}

/* "2G" in bytes, as J.1 writes a size. */
static unsigned long long bytes_of(const char *size)
{
	char              *unit;
	unsigned long long n = strtoull(size, &unit, 10);

	switch (*unit) {
	case 'T': n <<= 10; /* fall through */
	case 'G': n <<= 10; /* fall through */
	case 'M': n <<= 10; /* fall through */
	case 'K': n <<= 10; break;
	default:  break;
	}
	return n;
}

/* Each element after the key of `(key ...)`, as JSON strings. */
static void add_strings(text *t, const hull_sexpr_doc *doc, const hull_sexpr_form *entry)
{
	hull_sexpr_form item;
	int             more;
	int             first = 1;

	hull_sexpr_child(doc, entry, &item);
	for (more = hull_sexpr_next(doc, &item, &item); more;
	     more = hull_sexpr_next(doc, &item, &item)) {
		char *value = hull_access_text(doc, &item);

		if (value == NULL) {
			t->failed = 1;
			return;
		}
		if (!first)
			add_text(t, ",");
		add_json(t, value);
		free(value);
		first = 0;
	}
}

/* `(env (NAME "value") ...)` as ["NAME=value", ...]. */
static void add_env(text *t, const hull_sexpr_doc *doc, const hull_sexpr_form *entry)
{
	hull_sexpr_form item;
	int             more;
	int             first = 1;

	hull_sexpr_child(doc, entry, &item);
	for (more = hull_sexpr_next(doc, &item, &item); more;
	     more = hull_sexpr_next(doc, &item, &item)) {
		hull_sexpr_form name_form;
		hull_sexpr_form value_form;
		char           *name;
		char           *value;
		text            pair = { 0 };

		hull_sexpr_child(doc, &item, &name_form);
		hull_sexpr_next(doc, &name_form, &value_form);
		name  = hull_access_text(doc, &name_form);
		value = hull_access_text(doc, &value_form);
		if (name == NULL || value == NULL) {
			t->failed = 1;
		} else {
			add_text(&pair, name);
			add_text(&pair, "=");
			add_text(&pair, value);
			if (!first)
				add_text(t, ",");
			add_json(t, pair.data != NULL ? pair.data : "");
			t->failed |= pair.failed;
		}
		first = 0;
		free(pair.data);
		free(name);
		free(value);
	}
}

/* `(volumes (volume ...))` as Binds, each host path made absolute against
 * the plan's base: the daemon resolves nothing relative. Returns a
 * reason, or NULL. */
static const char *add_binds(text *t, const hull_sexpr_doc *doc, const hull_sexpr_form *entry,
                             char *detail, size_t cap)
{
	hull_sexpr_form item;
	int             more;
	int             first = 1;

	hull_sexpr_child(doc, entry, &item);
	for (more = hull_sexpr_next(doc, &item, &item); more;
	     more = hull_sexpr_next(doc, &item, &item)) {
		char *written   = hull_access_value(doc, &item, "host");
		char *host_path = written != NULL ? hull_access_path(doc, written) : NULL;
		char *guest     = hull_access_value(doc, &item, "guest");
		char *mode      = hull_access_value(doc, &item, "mode");
		char  absolute[PATH_MAX];
		text  bind = { 0 };

		free(written);
		if (host_path == NULL || guest == NULL || realpath(host_path, absolute) == NULL) {
			snprintf(detail, cap, "volume %s: %s", host_path != NULL ? host_path : "?",
			         strerror(errno));
			free(host_path);
			free(guest);
			free(mode);
			return detail;
		}
		add_text(&bind, absolute);
		add_text(&bind, ":");
		add_text(&bind, guest);
		add_text(&bind, mode != NULL && strcmp(mode, "ro") == 0 ? ":ro" : ":rw");
		if (!first)
			add_text(t, ",");
		add_json(t, bind.data != NULL ? bind.data : "");
		t->failed |= bind.failed;
		first = 0;
		free(bind.data);
		free(host_path);
		free(guest);
		free(mode);
	}
	return NULL;
}

/* `(ports (port ...))` as ExposedPorts into `exposed` and PortBindings
 * into `bindings`. */
static void add_ports(text *exposed, text *bindings, const hull_sexpr_doc *doc,
                      const hull_sexpr_form *entry)
{
	hull_sexpr_form item;
	int             more;

	hull_sexpr_child(doc, entry, &item);
	for (more = hull_sexpr_next(doc, &item, &item); more;
	     more = hull_sexpr_next(doc, &item, &item)) {
		char *host_port = hull_access_value(doc, &item, "host");
		char *guest     = hull_access_value(doc, &item, "guest");
		char *proto     = hull_access_value(doc, &item, "proto");

		if (host_port != NULL && guest != NULL && proto != NULL) {
			if (exposed->length != 0u) {
				add_text(exposed, ",");
				add_text(bindings, ",");
			}
			addf(exposed, "\"%s/%s\":{}", guest, proto);
			addf(bindings, "\"%s/%s\":[{\"HostIp\":\"127.0.0.1\",\"HostPort\":\"%s\"}]",
			     guest, proto, host_port);
		} else {
			exposed->failed = 1;
		}
		free(host_port);
		free(guest);
		free(proto);
	}
}

/* The create request's body, into `m->body`. Returns a reason, or NULL. */
static const char *build(hull_machine *m, const hull_sexpr_doc *doc,
                         const hull_sexpr_form *machine)
{
	text            body     = { 0 };
	text            exposed  = { 0 };
	text            bindings = { 0 };
	text            binds    = { 0 };
	hull_sexpr_form entry;
	const char     *reason   = NULL;
	char           *value;

	value = hull_access_value(doc, machine, "source");
	if (value == NULL || !is_ref(value)) {
		free(value);
		return "the docker adapter runs only an image ref";
	}
	add_text(&body, "{\"Image\":");
	add_json(&body, value);
	free(value);

	if (hull_access_find(doc, machine, "command", &entry)) {
		add_text(&body, ",\"Cmd\":[");
		add_strings(&body, doc, &entry);
		add_text(&body, "]");
	}
	if ((value = hull_access_value(doc, machine, "workdir")) != NULL) {
		add_text(&body, ",\"WorkingDir\":");
		add_json(&body, value);
		free(value);
	}
	if ((value = hull_access_value(doc, machine, "hostname")) != NULL) {
		add_text(&body, ",\"Hostname\":");
		add_json(&body, value);
		free(value);
	}
	if (hull_access_find(doc, machine, "env", &entry)) {
		add_text(&body, ",\"Env\":[");
		add_env(&body, doc, &entry);
		add_text(&body, "]");
	}
	if (hull_access_find(doc, machine, "ports", &entry)) {
		add_ports(&exposed, &bindings, doc, &entry);
		add_text(&body, ",\"ExposedPorts\":{");
		add_all(&body, &exposed);
		add_text(&body, "}");
	}
	addf(&body, ",\"User\":\"%u:%u\"", (unsigned)getuid(), (unsigned)getgid());
	add_text(&body, ",\"Labels\":{\"hull.machine\":");
	add_json(&body, m->name);
	add_text(&body, "}");

	/* HostConfig: binds, ports, limits, and section 7's security, whose
	 * capabilities start empty and are granted one by one. Init puts
	 * docker's init at PID 1, for the reason the chroot engine has its
	 * own: PID 1 ignores a signal it has no handler for, so a workload
	 * there would sit through stop's SIGTERM, and nothing would reap. */
	add_text(&body, ",\"HostConfig\":{\"Init\":true,\"CapDrop\":[\"ALL\"]");
	if (hull_access_find(doc, machine, "volumes", &entry)) {
		reason = add_binds(&binds, doc, &entry, m->detail, sizeof m->detail);
		add_text(&body, ",\"Binds\":[");
		add_all(&body, &binds);
		add_text(&body, "]");
	}
	if (bindings.data != NULL) {
		add_text(&body, ",\"PortBindings\":{");
		add_all(&body, &bindings);
		add_text(&body, "}");
	}
	if ((value = hull_access_value(doc, machine, "cpus")) != NULL) {
		addf(&body, ",\"NanoCpus\":%llu000000000", strtoull(value, NULL, 10));
		free(value);
	}
	if ((value = hull_access_value(doc, machine, "memory")) != NULL) {
		addf(&body, ",\"Memory\":%llu", bytes_of(value));
		free(value);
	}
	if (hull_access_find(doc, machine, "security", &entry)) {
		if (hull_access_true(doc, &entry, "readonly_root"))
			add_text(&body, ",\"ReadonlyRootfs\":true");
		if (hull_access_true(doc, &entry, "no_new_privs"))
			add_text(&body, ",\"SecurityOpt\":[\"no-new-privileges\"]");
	}
	add_text(&body, "}}");
	free(exposed.data);
	free(bindings.data);
	free(binds.data);
	if (reason == NULL && (body.failed || exposed.failed))
		reason = "the create request could not be built";
	if (reason != NULL) {
		free(body.data);
		return reason;
	}
	m->body = body.data;
	return NULL;
}

/* --- the reader ---------------------------------------------------------- */

static int read_full(int fd, uint8_t *buffer, size_t n)
{
	while (n > 0u) {
		ssize_t got = read(fd, buffer, n);

		if (got < 0 && errno == EINTR)
			continue;
		if (got <= 0)
			return -1;
		buffer += got;
		n -= (size_t)got;
	}
	return 0;
}

static void *read_workload(void *arg)
{
	hull_machine *m = arg;
	uint8_t       header[8];
	uint8_t       buffer[4096];
	char          path[128];
	char         *answer = NULL;
	long          code   = 125;
	int           status = 0;

	while (read_full(m->attach, header, sizeof header) == 0) {
		uint32_t size = (uint32_t)header[4] << 24 | (uint32_t)header[5] << 16 |
		                (uint32_t)header[6] << 8 | header[7];

		while (size > 0u) {
			size_t n = size < sizeof buffer ? size : sizeof buffer;

			if (read_full(m->attach, buffer, n) != 0)
				break;
			host->on_data(m->core, NULL, header[0] == 2 ? 2 : 1, buffer, n);
			size -= (uint32_t)n;
		}
	}
	close(m->attach);
	m->attach = -1;

	/* The stream has ended, so the container has: ask how. Docker gives a
	 * signal as 128 and its number, and that is what is reported. */
	snprintf(path, sizeof path, "/containers/%s/wait", m->id);
	if (call("POST", path, NULL, &status, &answer) == 0 && status == 200)
		json_number(answer, "StatusCode", &code);
	free(answer);
	pthread_mutex_lock(&m->lock);
	m->done = 1;
	pthread_cond_broadcast(&m->done_cond);
	pthread_mutex_unlock(&m->lock);
	host->on_exit(m->core, m, NULL, (int)code, 0);
	host->on_state(m->core, m, "stopped");
	return NULL;
}

/* --- operations ---------------------------------------------------------- */

static void release(hull_machine *m)
{
	free(m->body);
	pthread_mutex_destroy(&m->lock);
	pthread_cond_destroy(&m->done_cond);
	free(m);
}

static void remove_container(const char *id)
{
	char  path[160];
	char *answer = NULL;
	int   status = 0;

	snprintf(path, sizeof path, "/containers/%s?force=1", id);
	call("DELETE", path, NULL, &status, &answer);
	free(answer);
}

/* A failed create returns no handle, so it keeps its reason where the
 * caller can read it after the handle is gone. */
static char create_detail[512];

static hull_status docker_create(hull_core *core, hull_buf plan, hull_machine **out,
                                 hull_error *error)
{
	hull_sexpr_doc  doc;
	hull_sexpr_form machine;
	hull_machine   *m;
	const char     *reason;
	char           *answer = NULL;
	char           *hostname;
	char            path[160];
	int             status = 0;

	if (!hull_access_machine(&doc, plan.data, plan.len, &machine))
		return fail(error, HULL_ERR_UNSUPPORTED, "the plan is not canonical");
	m = calloc(1u, sizeof *m);
	if (m == NULL)
		return fail(error, HULL_ERR_INTERNAL, "out of memory");
	m->core   = core;
	m->attach = -1;
	pthread_mutex_init(&m->lock, NULL);
	pthread_cond_init(&m->done_cond, NULL);
	hostname = hull_access_value(&doc, &machine, "hostname");
	snprintf(m->name, sizeof m->name, "hull-%s-%ld-%u", hostname != NULL ? hostname : "machine",
	         (long)getpid(), machines_made++);
	free(hostname);

	reason = build(m, &doc, &machine);
	if (reason != NULL) {
		snprintf(create_detail, sizeof create_detail, "%s", reason);
		release(m);
		return fail(error, HULL_ERR_UNSUPPORTED, create_detail);
	}
	snprintf(path, sizeof path, "/containers/create?name=%s", m->name);
	if (call("POST", path, m->body, &status, &answer) != 0) {
		snprintf(create_detail, sizeof create_detail,
		         "the Docker daemon at %s cannot be reached", socket_path());
		release(m);
		return fail(error, HULL_ERR_RUNTIME, create_detail);
	}
	if (status != 201 || !json_string(answer, "Id", m->id, sizeof m->id)) {
		daemon_says(create_detail, sizeof create_detail, "create", status, answer);
		free(answer);
		release(m);
		return fail(error, HULL_ERR_RUNTIME, create_detail);
	}
	free(answer);
	*out = m;
	host->on_state(core, m, "created");
	return HULL_OK;
}

/* Attach, then start: attached first, nothing the workload writes is lost. */
static hull_status docker_start(hull_core *core, hull_machine *m, hull_error *error)
{
	char    path[160];
	char    head[512];
	char   *answer = NULL;
	size_t  n = 0u;
	int     status = 0;

	if (m->started)
		return fail(error, HULL_ERR_BUSY, "the machine has been started already");
	m->attach = dial();
	snprintf(path, sizeof path, "/containers/%s/attach?stream=1&stdout=1&stderr=1", m->id);
	if (m->attach < 0 ||
	    send_request(m->attach, "POST", path, NULL,
	                 "Connection: Upgrade\r\nUpgrade: tcp\r\n") != 0)
		return fail(error, HULL_ERR_RUNTIME, "the attach request failed");
	/* The answer's head, a byte at a time, so that nothing of the stream
	 * after it is read here. */
	while (n + 1u < sizeof head && read(m->attach, head + n, 1u) == 1) {
		n++;
		if (n >= 4u && memcmp(head + n - 4u, "\r\n\r\n", 4u) == 0)
			break;
	}
	head[n] = '\0';
	if (sscanf(head, "HTTP/1.%*d %d", &status) != 1 || (status != 101 && status != 200)) {
		close(m->attach);
		m->attach = -1;
		snprintf(m->detail, sizeof m->detail, "attach: HTTP %d", status);
		return fail(error, HULL_ERR_RUNTIME, m->detail);
	}

	snprintf(path, sizeof path, "/containers/%s/start", m->id);
	if (call("POST", path, NULL, &status, &answer) != 0 || status != 204) {
		daemon_says(m->detail, sizeof m->detail, "start", status, answer);
		free(answer);
		close(m->attach);
		m->attach = -1;
		return fail(error, HULL_ERR_RUNTIME, m->detail);
	}
	free(answer);
	if (pthread_create(&m->reader, NULL, read_workload, m) != 0) {
		close(m->attach);
		m->attach = -1;
		return fail(error, HULL_ERR_INTERNAL, "the reader thread could not start");
	}
	m->started = 1;
	host->on_state(core, m, "running");
	return HULL_OK;
}

/* The daemon's stop: SIGTERM, then SIGKILL after the timeout. */
static hull_status docker_stop(hull_core *core, hull_machine *m, int timeout_ms,
                               hull_error *error)
{
	char  path[160];
	char *answer = NULL;
	int   status = 0;
	int   done;

	(void)error;
	if (!m->started)
		return HULL_OK;
	pthread_mutex_lock(&m->lock);
	done = m->done;
	pthread_mutex_unlock(&m->lock);
	if (done)
		return HULL_OK;
	host->on_state(core, m, "stopping");
	snprintf(path, sizeof path, "/containers/%s/stop?t=%d", m->id,
	         timeout_ms <= 0 ? 0 : (timeout_ms + 999) / 1000);
	call("POST", path, NULL, &status, &answer);
	free(answer);
	return HULL_OK;
}

/* Remove the container whatever its state, then the handle. */
static hull_status docker_destroy(hull_core *core, hull_machine *m, hull_error *error)
{
	if (m->started) {
		docker_stop(core, m, 0, error);
		pthread_join(m->reader, NULL);
	}
	remove_container(m->id);
	release(m);
	return HULL_OK;
}

/* --- the declaration ----------------------------------------------------- */

static const hull_cap_entry entries[] = {
	/* Docker runs an image, not a directory (5.0). The cached docker
	 * import that would run a dir as an overlay is not written yet. */
	{ "source.dir.direct",  HULL_SUP_REJECTED,
	  "docker cannot run a directory in place", NULL },
	{ "source.dir.overlay", HULL_SUP_REJECTED,
	  "the cached docker import of a dir is not written yet", NULL },
	{ "source.ref.overlay", HULL_SUP_NATIVE,   NULL, NULL },

	/* "ports/volumes/env map directly" (5.0). */
	{ "command",            HULL_SUP_NATIVE,   NULL, NULL },
	{ "workdir",            HULL_SUP_NATIVE,   NULL, NULL },
	{ "env",                HULL_SUP_NATIVE,   NULL, NULL },
	{ "ports",              HULL_SUP_NATIVE,
	  "bound on 127.0.0.1 until section 8.1's addresses exist", NULL },
	{ "volumes",            HULL_SUP_NATIVE,   NULL, NULL },
	{ "hostname",           HULL_SUP_NATIVE,   NULL, NULL },

	/* ns: cgroups, memory a ceiling (6). */
	{ "cpus",               HULL_SUP_NATIVE,   NULL, NULL },
	{ "memory",             HULL_SUP_NATIVE,   "a ceiling, not an allocation", NULL },

	/* Section 7: no capabilities, and none granted to a workload that is
	 * not root, which is docker's rule and not ours to bend. */
	{ "security.readonly_root", HULL_SUP_NATIVE, NULL, NULL },
	{ "security.no_new_privs",  HULL_SUP_NATIVE, NULL, NULL },
	{ "security.allow",         HULL_SUP_REJECTED,
	  "docker grants capabilities only to root, and its root is the host's", NULL },

	/* A shared kernel sees every event and lock (6). */
	{ "fidelity.inotify",   HULL_SUP_NATIVE,   NULL, NULL },
	{ "fidelity.flock",     HULL_SUP_NATIVE,   NULL, NULL },

	/* auto never reaches an engine: the plan chooses (3.9, rule 3). */
	{ "projection.bind",    HULL_SUP_NATIVE,   NULL, NULL },
	{ "writeback.none",     HULL_SUP_NATIVE,   NULL, NULL },
	{ "on_disconnect.stop", HULL_SUP_NATIVE,   NULL, NULL },
	{ "on_disconnect.keep", HULL_SUP_NATIVE,
	  "kept while the core runs; adopting it after the core restarts is not written", NULL },

	{ NULL,                 HULL_SUP_NATIVE,   NULL, NULL },
};

static const char *const classes[] = { "ns", NULL };

static const hull_capabilities caps = {
	.struct_size    = sizeof (hull_capabilities),
	.engine_name    = "docker",
	.engine_version = "0",
	.classes        = classes,
	.entries        = entries,
	.migration      = HULL_MIG_WARM,  /* ns: warm, best-effort (H.3) */
	.requires_root  = 1,              /* dockerd's root daemon (5.0) */
};

static const hull_engine_ops ops = {
	.struct_size = sizeof (hull_engine_ops),
	.create      = docker_create,
	.start       = docker_start,
	.stop        = docker_stop,
	.destroy     = docker_destroy,
};

static const hull_engine engine = {
	.struct_size = sizeof (hull_engine),
	.abi_major   = HULL_ABI_MAJOR,
	.abi_minor   = HULL_ABI_MINOR,
	.caps        = &caps,
	.ops         = &ops,
};

const hull_engine *hull_engine_docker(const hull_host_ops *host_ops)
{
	host = host_ops;
	return &engine;
}
