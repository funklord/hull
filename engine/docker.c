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
 *
 * A dir source runs as an overlay: the tree is imported as an image
 * named for the SHA-256 of its tar, `hull-dir:<digest>`, which is
 * imported only when the daemon does not have it. The tar is read twice,
 * once for the digest and once into the import, which is tagged only
 * when the second reading comes to the same digest, so a tree that
 * changed between the two never wears a name it does not hold.
 */

#define _GNU_SOURCE

#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <limits.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <poll.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <arpa/inet.h>
#include <netinet/in.h>

#include "access.h"
#include "builtin.h"
#include "sha256.h"
#include "tree.h"
#include "usage.h"

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

/* The daemon's answer on `fd`, which it closes, as call() gives it. */
static int read_answer(int fd, int *status, char **answer)
{
	text  reply = { 0 };
	char  buffer[4096];
	char *head_end;
	char *payload;
	long  length;

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

/* One request, answered and closed. `status` is the HTTP status, and
 * `answer` the body, which the caller frees. Returns 0, or -1 when the
 * daemon could not be reached or answered with nonsense. */
static int call(const char *method, const char *path, const char *body, int *status,
                char **answer)
{
	int fd = dial();

	*answer = NULL;
	*status = 0;
	if (fd < 0)
		return -1;
	if (send_request(fd, method, path, body, "Connection: close\r\n") != 0) {
		close(fd);
		return -1;
	}
	return read_answer(fd, status, answer);
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

/* --- a dir source, imported ---------------------------------------------- */

/* A reading of the tree's tar: its digest and length, and, sending, where
 * it goes and how much it may be. */
typedef struct reading {
	hull_sha256        sha;
	unsigned long long length;
	int                fd;
	unsigned long long limit;
	int                over;
} reading;

static int counted(void *context, const uint8_t *data, size_t n)
{
	reading *r = context;

	hull_sha256_update(&r->sha, data, n);
	r->length += n;
	return 0;
}

static int sent(void *context, const uint8_t *data, size_t n)
{
	reading *r = context;

	if (n > r->limit - r->length) {
		r->over = 1;
		return -1;
	}
	hull_sha256_update(&r->sha, data, n);
	r->length += n;
	return send_all(r->fd, (const char *)data, n);
}

/*
 * Remove the imports of the tree labelled `tree` but `keep`, each
 * unforced: the daemon refuses one a container uses or that has another
 * name as well, and that one stays. An import from before the label has
 * none, and is not touched.
 */
static void retire_imports(const char *tree, const char *keep)
{
	char        path[400];
	char        id[80];
	char       *answer = NULL;
	const char *at;
	int         status = 0;

	/* {"label":["hull.tree=<tree>"]}, escaped for the query. */
	snprintf(path, sizeof path,
	         "/images/json?filters=%%7B%%22label%%22%%3A%%5B%%22hull.tree%%3D%s%%22%%5D%%7D", tree);
	if (call("GET", path, NULL, &status, &answer) != 0 || status != 200) {
		free(answer);
		return;
	}
	for (at = strstr(answer, "\"Id\":\""); at != NULL; at = strstr(at, "\"Id\":\"")) {
		size_t n = 0u;

		for (at += strlen("\"Id\":\""); *at != '"' && *at != '\0' && n + 1u < sizeof id;)
			id[n++] = *at++;
		id[n] = '\0';
		if (n > 0u && strcmp(id, keep) != 0) {
			char *gone = NULL;
			int   result = 0;

			snprintf(path, sizeof path, "/images/%s", id);
			if (call("DELETE", path, NULL, &result, &gone) == 0)
				free(gone);
		}
	}
	free(answer);
}

/*
 * The image a dir source at `root` runs from, `hull-dir:<digest>`, into
 * `image`: the daemon's already, or imported now. Returns NULL, or why,
 * in `detail`.
 *
 * An import is labelled hull.tree, the SHA-256 of the tree's real path, which
 * no path can make a label instruction or a query of, and a fresh one
 * retires the tree's others: the daemon keeps one import per tree, and
 * not one per state of it that ever ran. hull.path is that path in hex,
 * for gc to ask whether the tree is still there.
 */
static const char *import_dir(const char *root, char *image, size_t cap, char *detail,
                              size_t detail_cap)
{
	static const char changed[] = "the directory changed while it was imported";
	reading           first;
	reading           second;
	uint8_t           digest[32];
	uint8_t           again[32];
	char              hex[65];
	char              tree[65];
	char              where[2u * PATH_MAX + 1u];
	char              path[400];
	char              id[80];
	char             *answer = NULL;
	const char       *at;
	text              head = { 0 };
	int               status = 0;
	int               fd;
	size_t            n = 0u;

	memset(&first, 0, sizeof first);
	hull_sha256_init(&first.sha);
	if (hull_tree_tar(root, counted, &first, detail, detail_cap) != 0)
		return detail;
	hull_sha256_final(&first.sha, digest);
	hull_sha256_hex(digest, hex);
	snprintf(image, cap, "hull-dir:%s", hex);
	{
		/* The tree by its canonical path, so that two spellings of it,
		 * or a link to it, are one tree. */
		hull_sha256 named;
		char       *canonical = realpath(root, NULL);
		const char *name      = canonical != NULL ? canonical : root;

		hull_sha256_init(&named);
		hull_sha256_update(&named, (const uint8_t *)name, strlen(name));
		/* And the path itself, in hex, which gc reads back. */
		where[0] = '\0';
		for (size_t i = 0u; name[i] != '\0' && 2u * i + 2u < sizeof where; i++)
			snprintf(where + 2u * i, 3u, "%02x", (unsigned char)name[i]);
		free(canonical);
		hull_sha256_final(&named, again);
		hull_sha256_hex(again, tree);
	}

	snprintf(path, sizeof path, "/images/%s/json", image);
	if (call("GET", path, NULL, &status, &answer) != 0) {
		snprintf(detail, detail_cap, "the Docker daemon at %s cannot be reached", socket_path());
		return detail;
	}
	free(answer);
	answer = NULL;
	if (status == 200)
		return NULL;
	if (status != 404)
		return daemon_says(detail, detail_cap, "looking for the import", status, NULL);

	/* Untagged, so that nothing has the name until it is known to hold it. */
	fd = dial();
	if (fd < 0) {
		snprintf(detail, detail_cap, "the Docker daemon at %s cannot be reached", socket_path());
		return detail;
	}
	addf(&head, "POST " API "/images/create?fromSrc=-&changes=LABEL%%20hull.tree%%3D%s"
	     "&changes=LABEL%%20hull.path%%3D%s HTTP/1.1\r\n"
	     "Host: docker\r\nConnection: close\r\nContent-Type: application/x-tar\r\n"
	     "Content-Length: %llu\r\n\r\n", tree, where, first.length);
	memset(&second, 0, sizeof second);
	hull_sha256_init(&second.sha);
	second.fd    = fd;
	second.limit = first.length;
	if (head.failed || send_all(fd, head.data, head.length) != 0) {
		free(head.data);
		close(fd);
		snprintf(detail, detail_cap, "the import could not be sent");
		return detail;
	}
	free(head.data);
	if (hull_tree_tar(root, sent, &second, detail, detail_cap) != 0) {
		close(fd);
		if (second.over)
			snprintf(detail, detail_cap, "%s", changed);
		return detail;
	}
	/* A tree that shrank is sent out to the length promised, and its
	 * digest then differs. */
	while (second.length < second.limit) {
		static const char zeroes[4096];
		size_t            take = second.limit - second.length < sizeof zeroes ?
		                         (size_t)(second.limit - second.length) : sizeof zeroes;

		second.over = 1;
		if (send_all(fd, zeroes, take) != 0)
			break;
		second.length += take;
	}
	if (read_answer(fd, &status, &answer) != 0) {
		snprintf(detail, detail_cap, "the import got no answer");
		return detail;
	}
	if (status != 200 || json_string(answer, "error", detail, detail_cap)) {
		if (status != 200)
			daemon_says(detail, detail_cap, "import", status, answer);
		free(answer);
		return detail;
	}
	at = strstr(answer, "\"status\":\"sha256:");
	if (at != NULL)
		for (at += strlen("\"status\":\""); *at != '"' && *at != '\0' && n + 1u < sizeof id;)
			id[n++] = *at++;
	id[n] = '\0';
	free(answer);
	if (n == 0u) {
		snprintf(detail, detail_cap, "the import named no image");
		return detail;
	}
	hull_sha256_final(&second.sha, again);
	if (second.over || memcmp(digest, again, sizeof digest) != 0) {
		snprintf(path, sizeof path, "/images/%s?force=true", id);
		if (call("DELETE", path, NULL, &status, &answer) == 0)
			free(answer);
		snprintf(detail, detail_cap, "%s", changed);
		return detail;
	}
	snprintf(path, sizeof path, "/images/%s/tag?repo=hull-dir&tag=%s", id, hex);
	if (call("POST", path, NULL, &status, &answer) != 0 || status != 201) {
		daemon_says(detail, detail_cap, "tagging the import", status, answer);
		free(answer);
		return detail;
	}
	free(answer);
	retire_imports(tree, id);
	return NULL;
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

/* One more of Env's strings, after any before it. */
static void add_pair(void *context, const char *pair)
{
	text *t = context;

	if (t->length != 0u)
		add_text(t, ",");
	add_json(t, pair);
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
 * into `bindings`, each bound on `address`. */
static void add_ports(text *exposed, text *bindings, const hull_sexpr_doc *doc,
                      const hull_sexpr_form *entry, const char *address)
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
			addf(bindings, "\"%s/%s\":[{\"HostIp\":\"%s\",\"HostPort\":\"%s\"}]",
			     guest, proto, address, host_port);
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
                         const hull_sexpr_form *machine, const char *image)
{
	hull_sexpr_form top;
	struct in_addr  parsed;
	text            body     = { 0 };
	text            exposed  = { 0 };
	text            bindings = { 0 };
	text            binds    = { 0 };
	text            env      = { 0 };
	hull_sexpr_form entry;
	const char     *reason   = NULL;
	char           *value;

	add_text(&body, "{\"Image\":");
	add_json(&body, image);

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
	/* The env, then the secrets it is to hold (J.3's `(expose env)`), which
	 * the daemon then keeps in the container's configuration: that is
	 * what the frontend's warning says. */
	if (hull_access_find(doc, machine, "env", &entry))
		add_env(&env, doc, &entry);
	if (hull_access_exposed(doc, machine, add_pair, &env) != 0) {
		free(env.data);
		free(body.data);
		return "a secret to expose cannot be read, or holds a NUL";
	}
	if (env.length != 0u) {
		add_text(&body, ",\"Env\":[");
		add_all(&body, &env);
		add_text(&body, "]");
	}
	body.failed |= env.failed;
	free(env.data);
	/* Ports on the machine's address, which the core gives the plan. */
	if (hull_access_find(doc, machine, "ports", &entry)) {
		hull_sexpr_first(doc, &top);
		value = hull_access_value(doc, &top, "address");
		if (value == NULL || inet_pton(AF_INET, value, &parsed) != 1) {
			free(value);
			free(body.data);
			return "the plan gives its ports no address";
		}
		add_ports(&exposed, &bindings, doc, &entry, value);
		free(value);
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
	/* (network none) is docker's none, lo alone; (network host) its host;
	 * none given, its own bridge, as it always was. Ports need one. */
	if ((value = hull_access_value(doc, machine, "network")) != NULL) {
		int ported = hull_access_find(doc, machine, "ports", &entry);

		if (strcmp(value, "none") == 0 || strcmp(value, "host") == 0) {
			if (ported) {
				free(value);
				free(body.data);
				return "ports need the machine's own network: (network none) has none to "
				       "put them on, and (network host) is the host's";
			}
			addf(&body, ",\"NetworkMode\":\"%s\"", value);
		}
		free(value);
	}
	if (hull_access_find(doc, machine, "volumes", &entry))
		reason = add_binds(&binds, doc, &entry, m->detail, sizeof m->detail);
	/* The secrets the core wrote, read-only where J.3 puts them; the
	 * daemon makes the mount point. */
	hull_sexpr_first(doc, &top);
	if ((value = hull_access_value(doc, &top, "secrets")) != NULL) {
		text bind = { 0 };

		add_text(&bind, value);
		add_text(&bind, ":/run/hull/secrets:ro");
		if (binds.length != 0u)
			add_text(&binds, ",");
		add_json(&binds, bind.data != NULL ? bind.data : "");
		binds.failed |= bind.failed;
		free(bind.data);
		free(value);
	}
	if (binds.length != 0u) {
		add_text(&body, ",\"Binds\":[");
		add_all(&body, &binds);
		add_text(&body, "]");
	}
	body.failed |= binds.failed;
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

/* The image the machine runs: its ref, or its dir's import. A reason, or
 * NULL. */
static const char *source_image(const hull_sexpr_doc *doc, const hull_sexpr_form *machine,
                                char *image, size_t cap)
{
	char       *source = hull_access_value(doc, machine, "source");
	char       *root;
	const char *reason = NULL;

	if (source != NULL && is_ref(source)) {
		snprintf(image, cap, "%s", source);
	} else if (source != NULL && strncmp(source, "dir:", 4u) == 0 && source[4] != '\0') {
		root = hull_access_path(doc, source + 4);
		reason = root == NULL ? "out of memory" :
		         import_dir(root, image, cap, create_detail, sizeof create_detail);
		free(root);
	} else {
		reason = "the docker adapter runs an image ref or a dir";
	}
	free(source);
	return reason;
}

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
	char            image[256];
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

	reason = source_image(&doc, &machine, image, sizeof image);
	if (reason == NULL)
		reason = build(m, &doc, &machine, image);
	if (reason != NULL) {
		if (reason != create_detail)
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

/* --- adoption ------------------------------------------------------------ */

/* What a core that comes after needs to find the container again: its id. */
static hull_status docker_persist(hull_core *core, hull_machine *m, hull_buf *out,
                                  hull_error *error)
{
	(void)core;
	(void)error;
	out->data = (const uint8_t *)m->id;
	out->len  = strlen(m->id);
	return HULL_OK;
}

/*
 * A container a core that went left running, taken back by its id: one of
 * hull's, by its name, or it is not taken. Its output comes from its logs,
 * followed, rather than from an attach: the logs begin at its start, so
 * `hull logs` loses nothing the old core had, and following them ends when
 * the container does, or at once for one that already has, where an attach
 * to an ended container waits for good. That end is the reader's, as for
 * a machine this core started. HTTP/1.0, so that the stream is not
 * chunked.
 */
static hull_status docker_adopt(hull_core *core, hull_buf state, hull_machine **out,
                                hull_error *error)
{
	hull_machine *m;
	char          path[160];
	char          head[512];
	char          request[256];
	char          name[128];
	char         *answer = NULL;
	size_t        n = 0u;
	int           status = 0;

	if (state.len == 0u || state.len >= sizeof m->id)
		return fail(error, HULL_ERR_UNSUPPORTED, "the state is not a container id");
	for (size_t i = 0u; i < state.len; i++)
		if (!((state.data[i] >= '0' && state.data[i] <= '9') ||
		      (state.data[i] >= 'a' && state.data[i] <= 'f')))
			return fail(error, HULL_ERR_UNSUPPORTED, "the state is not a container id");
	m = calloc(1u, sizeof *m);
	if (m == NULL)
		return fail(error, HULL_ERR_INTERNAL, "out of memory");
	m->core   = core;
	m->attach = -1;
	pthread_mutex_init(&m->lock, NULL);
	pthread_cond_init(&m->done_cond, NULL);
	memcpy(m->id, state.data, state.len);

	snprintf(path, sizeof path, "/containers/%s/json", m->id);
	if (call("GET", path, NULL, &status, &answer) != 0) {
		release(m);
		snprintf(create_detail, sizeof create_detail,
		         "the Docker daemon at %s cannot be reached", socket_path());
		return fail(error, HULL_ERR_RUNTIME, create_detail);
	}
	if (status != 200 || !json_string(answer, "Name", name, sizeof name) ||
	    strncmp(name, "/hull-", 6u) != 0) {
		free(answer);
		release(m);
		return fail(error, HULL_ERR_GONE, status == 200 ? "the container is not hull's" :
		                                                   "the container is gone");
	}
	free(answer);
	snprintf(m->name, sizeof m->name, "%.95s", name + 1);

	m->attach = dial();
	snprintf(request, sizeof request,
	         "GET " API "/containers/%s/logs?follow=1&stdout=1&stderr=1 HTTP/1.0\r\n"
	         "Host: docker\r\n\r\n", m->id);
	if (m->attach < 0 || send_all(m->attach, request, strlen(request)) != 0) {
		if (m->attach >= 0)
			close(m->attach);
		release(m);
		return fail(error, HULL_ERR_RUNTIME, "the logs request failed");
	}
	while (n + 1u < sizeof head && read(m->attach, head + n, 1u) == 1) {
		n++;
		if (n >= 4u && memcmp(head + n - 4u, "\r\n\r\n", 4u) == 0)
			break;
	}
	head[n] = '\0';
	if (sscanf(head, "HTTP/1.%*d %d", &status) != 1 || status != 200) {
		close(m->attach);
		release(m);
		snprintf(create_detail, sizeof create_detail, "logs: HTTP %d", status);
		return fail(error, HULL_ERR_RUNTIME, create_detail);
	}
	if (pthread_create(&m->reader, NULL, read_workload, m) != 0) {
		close(m->attach);
		release(m);
		return fail(error, HULL_ERR_INTERNAL, "the reader thread could not start");
	}
	m->started = 1;
	*out = m;
	host->on_state(core, m, "running");
	return HULL_OK;
}

/* --- exec --------------------------------------------------------------- */

/*
 * A command in the running container, through docker's exec API: created
 * with stdin, stdout and stderr attached, as the invoking user like the
 * workload, then started on a hijacked connection. stdin is written to
 * that socket and ended by shutting down its writing half; output comes
 * back in the attach stream's frames, and the exit code from the exec's
 * inspection once the stream ends.
 *
 * Docker has no call to end an exec. As it runs as the user, its host pid
 * -- read from the same inspection, while it is still running, at the
 * moment it is to be ended -- can be signalled, which is stream_close().
 */
struct hull_stream {
	hull_machine *machine;
	hull_core    *core;
	char          id[80];
	int           socket;
	pthread_t     reader;
	int           ended;   /* stdin shut, or to be: under `lock` */
	pthread_mutex_t lock;
	int           tty;     /* on a terminal: one raw stream, not frames */
	int           dialed;  /* a connection to a port of the container's: raw */
};

static void *read_exec(void *arg)
{
	hull_stream *st = arg;
	uint8_t      header[8];
	uint8_t      buffer[4096];
	char         path[160];
	char        *answer = NULL;
	long         code   = 125;
	int          status = 0;

	/* A terminal's output is the bytes themselves, with nothing to say
	 * which stream: all of it is stdout. */
	while (st->tty) {
		ssize_t n = read(st->socket, buffer, sizeof buffer);

		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0)
			break;
		host->on_data(st->core, st, 1, buffer, (size_t)n);
	}
	while (!st->tty && read_full(st->socket, header, sizeof header) == 0) {
		uint32_t size = (uint32_t)header[4] << 24 | (uint32_t)header[5] << 16 |
		                (uint32_t)header[6] << 8 | header[7];

		while (size > 0u) {
			size_t n = size < sizeof buffer ? size : sizeof buffer;

			if (read_full(st->socket, buffer, n) != 0)
				break;
			host->on_data(st->core, st, header[0] == 2 ? 2 : 1, buffer, n);
			size -= (uint32_t)n;
		}
	}
	/* A connection's end is the machine's close, which is 0. */
	if (st->dialed) {
		host->on_exit(st->core, st->machine, st, 0, 0);
		return NULL;
	}
	snprintf(path, sizeof path, "/exec/%s/json", st->id);
	if (call("GET", path, NULL, &status, &answer) == 0 && status == 200)
		json_number(answer, "ExitCode", &code);
	free(answer);
	host->on_exit(st->core, st->machine, st, (int)code, 0);
	return NULL;
}

static hull_status docker_resize(hull_core *core, hull_stream *st, int rows, int cols,
                                 hull_error *error);

static hull_status docker_exec(hull_core *core, hull_machine *m, hull_buf spec,
                               hull_stream **out, hull_error *error)
{
	hull_sexpr_doc  doc;
	hull_sexpr_form message;
	hull_sexpr_form argv;
	hull_stream    *st;
	text            body = { 0 };
	char            path[160];
	char            head[512];
	char           *answer = NULL;
	size_t          n = 0u;
	int             status = 0;
	int             tty;
	int             cols = 0;
	int             rows = 0;

	if (hull_sexpr_open(&doc, spec.data, spec.len, HULL_SEXPR_CANONICAL, NULL) != HULL_SEXPR_OK ||
	    !hull_sexpr_first(&doc, &message) || !hull_access_find(&doc, &message, "argv", &argv))
		return fail(error, HULL_ERR_UNSUPPORTED, "an exec names a command");
	tty = hull_access_terminal(&doc, &message, &cols, &rows);
	add_text(&body, "{\"AttachStdin\":true,\"AttachStdout\":true,\"AttachStderr\":true,");
	addf(&body, "\"Tty\":%s,", tty ? "true" : "false");
	addf(&body, "\"User\":\"%u:%u\",\"Cmd\":[", (unsigned)getuid(), (unsigned)getgid());
	add_strings(&body, &doc, &argv);
	add_text(&body, "]}");
	st = calloc(1u, sizeof *st);
	if (body.failed || st == NULL) {
		free(body.data);
		free(st);
		return fail(error, HULL_ERR_INTERNAL, "out of memory");
	}
	st->machine = m;
	st->core    = core;
	st->socket  = -1;
	st->tty     = tty;
	snprintf(path, sizeof path, "/containers/%s/exec", m->id);
	if (call("POST", path, body.data, &status, &answer) != 0 || status != 201 ||
	    !json_string(answer, "Id", st->id, sizeof st->id)) {
		daemon_says(m->detail, sizeof m->detail, "exec", status, answer);
		free(answer);
		free(body.data);
		free(st);
		return fail(error, HULL_ERR_RUNTIME, m->detail);
	}
	free(answer);
	free(body.data);

	st->socket = dial();
	snprintf(path, sizeof path, "/exec/%s/start", st->id);
	if (st->socket < 0 ||
	    send_request(st->socket, "POST", path,
	                 tty ? "{\"Detach\":false,\"Tty\":true}" : "{\"Detach\":false,\"Tty\":false}",
	                 "Connection: Upgrade\r\nUpgrade: tcp\r\n") != 0) {
		if (st->socket >= 0)
			close(st->socket);
		free(st);
		return fail(error, HULL_ERR_RUNTIME, "the exec's start request failed");
	}
	while (n + 1u < sizeof head && read(st->socket, head + n, 1u) == 1) {
		n++;
		if (n >= 4u && memcmp(head + n - 4u, "\r\n\r\n", 4u) == 0)
			break;
	}
	head[n] = '\0';
	if (sscanf(head, "HTTP/1.%*d %d", &status) != 1 || (status != 101 && status != 200)) {
		close(st->socket);
		free(st);
		snprintf(m->detail, sizeof m->detail, "exec start: HTTP %d", status);
		return fail(error, HULL_ERR_RUNTIME, m->detail);
	}
	if (pthread_create(&st->reader, NULL, read_exec, st) != 0) {
		close(st->socket);
		free(st);
		return fail(error, HULL_ERR_INTERNAL, "the reader thread could not start");
	}
	/* API 1.41 creates an exec with no size, and docker's own client sets
	 * it once started, as here: a command that reads it at once may see
	 * the daemon's default first, and the SIGWINCH after. */
	if (tty)
		docker_resize(core, st, rows, cols, error);
	pthread_mutex_init(&st->lock, NULL);
	*out = st;
	return HULL_OK;
}

/* The container's address on its network, from docker's inspection: the
 * first IPAddress that is not empty, the bridge's or a network's. */
static int container_address(const hull_machine *m, struct in_addr *address)
{
	char        path[160];
	char       *answer = NULL;
	const char *at;
	int         status = 0;
	int         found = 0;

	snprintf(path, sizeof path, "/containers/%s/json", m->id);
	if (call("GET", path, NULL, &status, &answer) != 0 || status != 200) {
		free(answer);
		return 0;
	}
	for (at = strstr(answer, "\"IPAddress\":\""); at != NULL && !found;
	     at = strstr(at, "\"IPAddress\":\"")) {
		char   text[16];
		size_t n = 0u;

		for (at += strlen("\"IPAddress\":\""); *at != '"' && *at != '\0' && n + 1u < sizeof text;)
			text[n++] = *at++;
		text[n] = '\0';
		found = n > 0u && inet_pton(AF_INET, text, address) == 1;
	}
	free(answer);
	return found;
}

/* One of the container's ports, declared or not, as a stream: a TCP
 * connection from the host to the container's address, as docker's own
 * proxy makes one. A connect not answered in two seconds is refused. */
static hull_status docker_dial(hull_core *core, hull_machine *m, uint16_t port,
                               hull_stream **out, hull_error *error)
{
	struct sockaddr_in to;
	struct pollfd      wait;
	hull_stream       *st;
	int                fd;
	int                failed = 0;
	socklen_t          length = sizeof failed;

	memset(&to, 0, sizeof to);
	to.sin_family = AF_INET;
	to.sin_port   = htons(port);
	if (!m->started || !container_address(m, &to.sin_addr))
		return fail(error, HULL_ERR_GONE, "the machine has no address to dial");
	fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
	if (fd < 0)
		return fail(error, HULL_ERR_INTERNAL, "no socket to dial with");
	if (connect(fd, (struct sockaddr *)&to, sizeof to) != 0) {
		wait = (struct pollfd){ fd, POLLOUT, 0 };
		if (errno != EINPROGRESS)
			failed = errno;
		else if (poll(&wait, 1u, 2000) != 1)
			failed = ETIMEDOUT;
		else if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &failed, &length) != 0)
			failed = errno;
	}
	if (failed != 0) {
		close(fd);
		snprintf(m->detail, sizeof m->detail, "port %u: %s", (unsigned)port, strerror(failed));
		return fail(error, HULL_ERR_RUNTIME, m->detail);
	}
	/* Blocking from here: the reader waits on it, and the writes do
	 * not, sending with MSG_DONTWAIT. */
	if (fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) & ~O_NONBLOCK) != 0 ||
	    (st = calloc(1u, sizeof *st)) == NULL) {
		close(fd);
		return fail(error, HULL_ERR_INTERNAL, "out of memory");
	}
	st->machine = m;
	st->core    = core;
	st->socket  = fd;
	st->tty     = 1;
	st->dialed  = 1;
	pthread_mutex_init(&st->lock, NULL);
	if (pthread_create(&st->reader, NULL, read_exec, st) != 0) {
		pthread_mutex_destroy(&st->lock);
		close(fd);
		free(st);
		return fail(error, HULL_ERR_INTERNAL, "the reader thread could not start");
	}
	*out = st;
	return HULL_OK;
}

/* stdin, written whole however long the command takes to read it, on a
 * thread of the caller's own. The sends do not block, so the wait is a
 * poll that stream_eof() can end, within a tenth of a second. The socket
 * stays open until stream_close(), so it is never a number reused. */
static hull_status docker_stream_write(hull_core *core, hull_stream *st, const uint8_t *data,
                                       size_t length, hull_error *error)
{
	(void)core;
	while (length > 0u) {
		struct pollfd ready = { st->socket, POLLOUT, 0 };
		ssize_t       n;
		int           ended;

		pthread_mutex_lock(&st->lock);
		ended = st->ended;
		pthread_mutex_unlock(&st->lock);
		if (ended)
			return fail(error, HULL_ERR_GONE, "the command's input has closed");
		n = send(st->socket, data, length, MSG_NOSIGNAL | MSG_DONTWAIT);
		if (n < 0 && (errno == EAGAIN || errno == EINTR)) {
			poll(&ready, 1, 100);
			continue;
		}
		if (n <= 0)
			return fail(error, HULL_ERR_GONE, "the command's input has closed");
		data += n;
		length -= (size_t)n;
	}
	return HULL_OK;
}

/* The end of stdin. May be called while a write is under way on another
 * thread, which then gives up. */
static hull_status docker_stream_eof(hull_core *core, hull_stream *st, hull_error *error)
{
	int ended;

	(void)core;
	(void)error;
	pthread_mutex_lock(&st->lock);
	ended = st->ended;
	st->ended = 1;
	pthread_mutex_unlock(&st->lock);
	if (!ended)
		shutdown(st->socket, SHUT_WR);
	return HULL_OK;
}

/* `signum` to an exec's command, while docker says it runs. Docker has no
 * call to signal an exec, and the command is the user's: its host pid,
 * read at the moment of signalling. */
static int signal_exec(const hull_stream *st, int signum)
{
	char  path[160];
	char *answer = NULL;
	int   status = 0;
	long  pid    = 0;
	int   sent   = 0;

	snprintf(path, sizeof path, "/exec/%s/json", st->id);
	if (call("GET", path, NULL, &status, &answer) == 0 && status == 200 &&
	    strstr(answer, "\"Running\":true") != NULL && json_number(answer, "Pid", &pid) &&
	    pid > 1)
		sent = kill((pid_t)pid, signum) == 0;
	free(answer);
	return sent;
}

static hull_status docker_resize(hull_core *core, hull_stream *st, int rows, int cols,
                                 hull_error *error)
{
	char  path[192];
	char *answer = NULL;
	int   status = 0;

	(void)core;
	if (st == NULL || !st->tty)
		return fail(error, HULL_ERR_UNSUPPORTED, "the command has no terminal");
	snprintf(path, sizeof path, "/exec/%s/resize?h=%d&w=%d", st->id, rows, cols);
	if (call("POST", path, NULL, &status, &answer) != 0 || (status != 200 && status != 201)) {
		daemon_says(st->machine->detail, sizeof st->machine->detail, "resize", status, answer);
		free(answer);
		return fail(error, HULL_ERR_RUNTIME, st->machine->detail);
	}
	free(answer);
	return HULL_OK;
}

static hull_status docker_signal(hull_core *core, hull_machine *m, hull_stream *st, int signum,
                                 hull_error *error)
{
	char  path[160];
	char *answer = NULL;
	int   status = 0;

	(void)core;
	if (st != NULL && st->dialed) {
		/* A connection has no process: any signal resets it. */
		struct linger now = { 1, 0 };

		setsockopt(st->socket, SOL_SOCKET, SO_LINGER, &now, sizeof now);
		shutdown(st->socket, SHUT_RDWR);
		return HULL_OK;
	}
	if (st != NULL) {
		signal_exec(st, signum);
		return HULL_OK;
	}
	/* The workload's: docker's kill signals the container's PID 1, its
	 * init, which forwards it as the chroot engine's does. A container
	 * that has stopped is 409, and is not running as far as the caller
	 * is concerned. */
	snprintf(path, sizeof path, "/containers/%s/kill?signal=%d", m->id, signum);
	if (call("POST", path, NULL, &status, &answer) != 0)
		return fail(error, HULL_ERR_RUNTIME, "the docker daemon could not be reached");
	free(answer);
	if (status == 409)
		return fail(error, HULL_ERR_GONE, "the machine is not running");
	if (status != 204)
		return fail(error, HULL_ERR_RUNTIME, "docker refused the signal");
	return HULL_OK;
}

/* End the command if it runs, then wait for its stream and free it. */
static hull_status docker_stream_close(hull_core *core, hull_stream *st, hull_error *error)
{
	docker_stream_eof(core, st, error);
	if (!st->dialed)
		signal_exec(st, SIGKILL);
	shutdown(st->socket, SHUT_RDWR);
	pthread_join(st->reader, NULL);
	close(st->socket);
	pthread_mutex_destroy(&st->lock);
	free(st);
	return HULL_OK;
}

/* --- the declaration ----------------------------------------------------- */

static const hull_cap_entry entries[] = {
	/* Docker runs an image, not a directory (5.0). The cached docker
	 * import that would run a dir as an overlay is not written yet. */
	{ "source.dir.direct",  HULL_SUP_REJECTED,
	  "docker cannot run a directory in place", NULL },
	{ "source.dir.overlay", HULL_SUP_EMULATED,
	  "a docker import of the tree, kept by its SHA-256; / is root's, as docker makes it",
	  NULL },
	{ "source.ref.overlay", HULL_SUP_NATIVE,   NULL, NULL },

	/* "ports/volumes/env map directly" (5.0). */
	{ "command",            HULL_SUP_NATIVE,   NULL, NULL },
	{ "workdir",            HULL_SUP_NATIVE,   NULL, NULL },
	{ "env",                HULL_SUP_NATIVE,   NULL, NULL },
	{ "network.host",       HULL_SUP_NATIVE,   NULL, NULL },
	{ "network.none",       HULL_SUP_NATIVE,   NULL, NULL },
	{ "export",             HULL_SUP_REJECTED,
	  "not yet: docker export does it, and the adapter does not call it", NULL },
	{ "layers",             HULL_SUP_REJECTED,
	  "not yet: a container's layers are its image's", NULL },
	{ "upper",              HULL_SUP_REJECTED,
	  "a container's writable layer is the daemon's", NULL },
	{ "owners",             HULL_SUP_REJECTED,
	  "not yet: an image's own owners are docker's", NULL },
	{ "init",               HULL_SUP_REJECTED,
	  "not yet: a container's PID 1 is docker's init, for its signals", NULL },
	{ "segments",           HULL_SUP_REJECTED,
	  "not yet: a container's NIC on the core's switch is not written", NULL },
	{ "impair",             HULL_SUP_REJECTED,
	  "not yet: a link is impaired in the core's switch, which a container is not on", NULL },
	{ "tamper",             HULL_SUP_REJECTED,
	  "not yet: a fault on a link in the core's switch, which a container is not on", NULL },
	{ "ports",              HULL_SUP_NATIVE,
	  "bound on the machine's address", NULL },
	{ "volumes",            HULL_SUP_NATIVE,   NULL, NULL },
	{ "volumes.layers",     HULL_SUP_REJECTED,
	  "not yet: a stack of layers would be an overlay the daemon mounts", NULL },
	{ "secrets",            HULL_SUP_NATIVE,
	  "bound read-only from the core's tmpfs; one exposed is in the container's "
	  "configuration too, where docker inspect shows it", NULL },
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
	  "kept, and taken back by a core that comes after one that went", NULL },

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

/* --- stats --------------------------------------------------------------- */

/* The number after `"key":` at or after `from` in `json`, or -1. */
static long long json_after(const char *json, const char *from, const char *key)
{
	char        pattern[64];
	const char *at = json != NULL ? strstr(json, from) : NULL;

	snprintf(pattern, sizeof pattern, "\"%s\":", key);
	at = at != NULL ? strstr(at, pattern) : NULL;
	return at != NULL ? strtoll(at + strlen(pattern), NULL, 10) : -1;
}

/* What the host spends on the container, as hull_engine.h's stats: CPU
 * time and storage io from docker's cgroup figures, which count every
 * process it ran, an ended exec's too, where /proc has lost one that
 * containerd's shim reaped; and memory resident by Pss, summed over the
 * container's pid namespace from /proc, as the chroot engine's is, since
 * a cgroup's usage counts the page cache. The processes are the user's,
 * so /proc is theirs to read. */
static hull_status docker_stats(hull_core *core, hull_machine *m, hull_buf *out, hull_error *error)
{
	static unsigned char encoded[128];
	hull_usage           usage = { 0 };
	char                 path[160];
	char                *answer = NULL;
	const char          *at;
	const char          *stop;
	long                 pid = 0;
	long long            cpu;
	int                  status = 0;

	(void)core;
	snprintf(path, sizeof path, "/containers/%s/json", m->id);
	if (!m->started || call("GET", path, NULL, &status, &answer) != 0 || status != 200 ||
	    strstr(answer, "\"Running\":true") == NULL || !json_number(answer, "Pid", &pid) ||
	    pid < 1 || !hull_usage_namespace((pid_t)pid, &usage)) {
		free(answer);
		return fail(error, HULL_ERR_GONE, "the machine is not running");
	}
	free(answer);
	answer = NULL;
	usage.cpu = 0u;
	usage.io  = 0u;
	snprintf(path, sizeof path, "/containers/%s/stats?stream=false&one-shot=true", m->id);
	if (call("GET", path, NULL, &status, &answer) != 0 || status != 200 ||
	    (cpu = json_after(answer, "\"cpu_stats\":", "total_usage")) < 0) {
		free(answer);
		return fail(error, HULL_ERR_RUNTIME, "docker gave no stats for the machine");
	}
	usage.cpu = (unsigned long long)cpu / 1000u;
	/* Each device's read and write, as entries of
	 * {"major":..,"minor":..,"op":"read","value":N}. */
	at   = strstr(answer, "\"io_service_bytes_recursive\":[");
	stop = at != NULL ? strchr(at, ']') : NULL;
	while (stop != NULL && (at = strstr(at, "\"op\":\"")) != NULL && at < stop) {
		const char *op = at + strlen("\"op\":\"");
		const char *value = strstr(op, "\"value\":");

		if (value == NULL || value > stop)
			break;
		if (strncasecmp(op, "read\"", 5u) == 0 || strncasecmp(op, "write\"", 6u) == 0)
			usage.io += strtoull(value + strlen("\"value\":"), NULL, 10);
		at = value;
	}
	free(answer);
	out->data = encoded;
	out->len  = hull_usage_encode(&usage, encoded, sizeof encoded);
	return out->len == 0u ? fail(error, HULL_ERR_INTERNAL, "stats do not fit") : HULL_OK;
}

/* --- gc ------------------------------------------------------------------ */

/* Why an import of a dir source is no longer wanted, or NULL: it has no
 * label naming its tree, being older than the label, or its tree is not
 * a directory now. `inspected` is the daemon's answer for the image. */
static const char *unwanted(const char *inspected, char *tree_path, size_t cap)
{
	char        tree[80];
	char        where[2u * PATH_MAX + 1u];
	struct stat st;
	size_t      n = 0u;

	if (!json_string(inspected, "hull.tree", tree, sizeof tree))
		return "imported before hull.tree labelled its tree";
	if (!json_string(inspected, "hull.path", where, sizeof where))
		return "imported before hull.path named its tree";
	for (size_t i = 0u; where[i] != '\0' && where[i + 1u] != '\0' && n + 1u < cap; i += 2u) {
		unsigned byte = 0u;

		if (sscanf(where + i, "%2x", &byte) != 1)
			return "its hull.path is not hex";
		tree_path[n++] = (char)byte;
	}
	tree_path[n] = '\0';
	if (stat(tree_path, &st) != 0 || !S_ISDIR(st.st_mode))
		return "its tree is gone";
	return NULL;
}

/* The daemon's pause, its cgroup freezer, and unpause. */
static hull_status docker_pause(hull_core *core, hull_machine *m, int paused, hull_error *error)
{
	char  path[160];
	char *answer = NULL;
	int   status = 0;

	(void)core;
	snprintf(path, sizeof path, "/containers/%s/%s", m->id, paused ? "pause" : "unpause");
	if (call("POST", path, NULL, &status, &answer) != 0)
		return fail(error, HULL_ERR_RUNTIME, "the docker daemon could not be reached");
	free(answer);
	if (status == 409 || status == 404)
		return fail(error, HULL_ERR_GONE, "the machine is not running");
	if (status != 204)
		return fail(error, HULL_ERR_RUNTIME, "the docker daemon refused the pause");
	return HULL_OK;
}

/* Each hull-dir import nothing wants, removed unforced, so that the
 * daemon keeps one a container uses or one with another name; a line
 * each, of what was removed and what was kept. */
static hull_status docker_gc(hull_core *core, hull_buf *report, hull_error *error)
{
	static text said;
	static char detail[512];
	char       *answer = NULL;
	const char *at;
	int         status = 0;

	(void)core;
	free(said.data);
	said = (text){ 0 };
	/* {"reference":["hull-dir"]}, escaped for the query. */
	if (call("GET", "/images/json?filters=%7B%22reference%22%3A%5B%22hull-dir%22%5D%7D", NULL,
	         &status, &answer) != 0) {
		snprintf(detail, sizeof detail, "the Docker daemon at %s cannot be reached",
		         socket_path());
		return fail(error, HULL_ERR_RUNTIME, detail);
	}
	if (status != 200) {
		daemon_says(detail, sizeof detail, "listing the imports", status, answer);
		free(answer);
		return fail(error, HULL_ERR_RUNTIME, detail);
	}
	for (at = strstr(answer, "\"Id\":\""); at != NULL; at = strstr(at, "\"Id\":\"")) {
		char        id[80];
		char        name[160] = "";
		char        path[200];
		char        tree_path[PATH_MAX];
		char       *inspected = NULL;
		char       *gone = NULL;
		const char *why;
		const char *tags;
		size_t      n = 0u;
		int         result = 0;

		for (at += strlen("\"Id\":\""); *at != '"' && *at != '\0' && n + 1u < sizeof id;)
			id[n++] = *at++;
		id[n] = '\0';
		snprintf(path, sizeof path, "/images/%s/json", id);
		if (n == 0u || call("GET", path, NULL, &result, &inspected) != 0 || result != 200) {
			free(inspected);
			continue;
		}
		/* Its first name, which is the one hull gave it. */
		tags = strstr(inspected, "\"RepoTags\":[\"");
		n = 0u;
		if (tags != NULL)
			for (tags += strlen("\"RepoTags\":[\""); *tags != '"' && *tags != '\0' &&
			     n + 1u < sizeof name;)
				name[n++] = *tags++;
		name[n] = '\0';
		why = unwanted(inspected, tree_path, sizeof tree_path);
		if (why == NULL) {
			free(inspected);
			continue;
		}
		snprintf(path, sizeof path, "/images/%s", id);
		if (call("DELETE", path, NULL, &result, &gone) == 0 && result == 200) {
			addf(&said, "docker: removed %s, %s\n", name[0] != '\0' ? name : id, why);
		} else {
			daemon_says(detail, sizeof detail, "kept", result, gone);
			addf(&said, "docker: %s %s, %s\n", detail, name[0] != '\0' ? name : id, why);
		}
		free(gone);
		free(inspected);
	}
	free(answer);
	report->data = (const uint8_t *)said.data;
	report->len  = said.length;
	return HULL_OK;
}

static const hull_engine_ops ops = {
	.struct_size = sizeof (hull_engine_ops),
	.create      = docker_create,
	.start       = docker_start,
	.stop        = docker_stop,
	.destroy     = docker_destroy,
	.adopt       = docker_adopt,
	.persist     = docker_persist,
	.exec         = docker_exec,
	.stream_write = docker_stream_write,
	.stream_eof   = docker_stream_eof,
	.stream_close = docker_stream_close,
	.resize       = docker_resize,
	.signal       = docker_signal,
	.stats        = docker_stats,
	.gc           = docker_gc,
	.dial         = docker_dial,
	.pause        = docker_pause,
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
