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
#include <sys/utsname.h>
#include <sys/xattr.h>
#include <sys/sysmacros.h>
#include <dirent.h>
#include <time.h>
#include <unistd.h>

#include <arpa/inet.h>
#include <netinet/in.h>

#include "access.h"
#include "builtin.h"
#include "sha256.h"
#include "stack.h"
#include "tree.h"
#include "usage.h"

#define API           "/v1.41"
#define MAX_RESPONSE  (1u << 20)
#define MAX_LAYERS    64u   /* the root's trees and its volumes' stacks, as layers */
#define MAX_STACKS    8u    /* volumes of (layers ...) */
#define MAX_PREFIX    16u   /* the components of a stack's guest path */

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
	int              export_fd;  /* (export PATH)'s file, or -1 */
	int              upper_fd;   /* (upper PATH), its directory, or -1 */
	char            *trees[MAX_LAYERS];  /* a dir source's trees, the source first, upper last */
	unsigned         tree_count;
	struct {
		char      *guest;      /* "srv/data": its guest path, relative */
		hull_stack stack;      /* its layers, upper and mode */
		int        upper_fd;   /* the upper, held open for its writes, or -1 */
	}                stacks[MAX_STACKS];
	unsigned         stack_count;
	char            *direct;     /* (mutable direct): the directory bound in, or NULL */
	char            *direct_binds;  /* its entries' binds, frame_direct's, as JSON strings */
	size_t           direct_binds_length;
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

static int owner_order(const void *a, const void *b)
{
	return strcmp(((const hull_tree_owner *)a)->path, ((const hull_tree_owner *)b)->path);
}

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
static const char *import_dir(const char *root, const hull_tree_owner *owners, size_t count,
                              char *image, size_t cap, char *detail, size_t detail_cap)
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
	if (hull_tree_tar_owned(root, owners, count, counted, &first, detail, detail_cap) != 0)
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
	if (hull_tree_tar_owned(root, owners, count, sent, &second, detail, detail_cap) != 0) {
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

/* One layer of an image import_layers loads: a tree, and where it is a
 * volume's stack's, the guest path it is laid beneath, the metadata of
 * the directories above that, and whether it is the stack's lowest,
 * whose opaque marker hides what the root has there. */
typedef struct layer_in {
	const char   *tree;
	const char   *prefix;
	hull_tree_dir parents[MAX_PREFIX];
	int           opaque;
} layer_in;

/* A layer's tar: the source's with its owners, a layer's or an upper's
 * with its whiteouts docker's, a stack's beneath its guest path. */
static int layer_tar(const layer_in *l, int source, const hull_tree_owner *owners, size_t count,
                     hull_tree_sink sink, void *context, char *detail, size_t detail_cap)
{
	if (l->prefix != NULL)
		return hull_tree_tar_under(l->tree, l->prefix, l->parents, l->opaque, sink, context,
		                           detail, detail_cap);
	return (source ? hull_tree_tar_owned : hull_tree_tar_layer)(l->tree, owners, count, sink,
	                                                            context, detail, detail_cap);
}

/* A ustar header of the docker-save tar's own, for an entry hull names:
 * a short name, a file's size, nothing else; 0, or -1. */
static int save_header(int fd, const char *name, unsigned long long size)
{
	uint8_t  block[512];
	unsigned sum = 0u;

	memset(block, 0, sizeof block);
	snprintf((char *)block, 100u, "%s", name);
	memcpy(block + 100, "0000644", 7u);
	memcpy(block + 108, "0000000", 7u);
	memcpy(block + 116, "0000000", 7u);
	snprintf((char *)block + 124, 12u, "%011llo", size);
	memcpy(block + 136, "00000000000", 11u);
	memset(block + 148, ' ', 8u);
	block[156] = '0';
	memcpy(block + 257, "ustar", 6u);
	memcpy(block + 263, "00", 2u);
	for (size_t i = 0u; i < sizeof block; i++)
		sum += block[i];
	snprintf((char *)block + 148, 8u, "%06o", sum);
	return send_all(fd, (const char *)block, sizeof block);
}

/* The zeroes after `n` bytes, to the end of their block. */
static int save_pad(int fd, unsigned long long n)
{
	static const char zeroes[512];

	return n % 512u == 0u ? 0 : send_all(fd, zeroes, 512u - (size_t)(n % 512u));
}

static unsigned long long save_entry(unsigned long long n)
{
	return 512u + (n + 511u) / 512u * 512u;
}

/*
 * A dir source with (layers ...) as an image of as many layers, `trees`,
 * `count` of them, the source first: each tree's tar a layer of its own,
 * docker's union putting each above the last as overlayfs does, loaded as
 * a docker-save tar, `hull-dir:<digest of its config>`, into `image`. The
 * config names each layer's digest, which the daemon checks as it loads,
 * so a tree that changed between the two readings is refused there.
 * Labelled as an import is, hull.tree the paths' SHA-256 together and
 * hull.path the source's, for gc. NULL, or why in `detail`.
 */
static const char *import_layers(const layer_in *trees, unsigned count,
                                 const hull_tree_owner *owners, size_t owner_count, char *image,
                                 size_t cap, char *detail, size_t detail_cap)
{
	reading            first[MAX_LAYERS];
	uint8_t            digest[32];
	char               hex[65];
	char               tree[65];
	char               where[2u * PATH_MAX + 1u];
	char               path[400];
	char               name[32];
	char              *answer = NULL;
	const char        *arch = "amd64";
	text               config = { 0 };
	text               manifest = { 0 };
	text               head = { 0 };
	unsigned long long total = 1024u;
	int                status = 0;
	int                fd;
	struct utsname     self;

	if (count > MAX_LAYERS) {
		snprintf(detail, detail_cap, "more than %u layers", MAX_LAYERS - 1u);
		return detail;
	}
	if (uname(&self) == 0 && strcmp(self.machine, "aarch64") == 0)
		arch = "arm64";
	{
		hull_sha256 named;

		hull_sha256_init(&named);
		for (unsigned i = 0u; i < count; i++) {
			char       *canonical = realpath(trees[i].tree, NULL);
			const char *at = canonical != NULL ? canonical : trees[i].tree;

			hull_sha256_update(&named, (const uint8_t *)at, strlen(at) + 1u);
			if (trees[i].prefix != NULL)
				hull_sha256_update(&named, (const uint8_t *)trees[i].prefix,
				                   strlen(trees[i].prefix) + 1u);
			if (i == 0u) {
				where[0] = '\0';
				for (size_t k = 0u; at[k] != '\0' && 2u * k + 2u < sizeof where; k++)
					snprintf(where + 2u * k, 3u, "%02x", (unsigned char)at[k]);
			}
			free(canonical);
		}
		hull_sha256_final(&named, digest);
		hull_sha256_hex(digest, tree);
	}
	addf(&config, "{\"architecture\":\"%s\",\"os\":\"linux\",\"config\":{\"Labels\":"
	     "{\"hull.tree\":\"%s\",\"hull.path\":\"%s\"}},\"rootfs\":{\"type\":\"layers\","
	     "\"diff_ids\":[", arch, tree, where);
	for (unsigned i = 0u; i < count; i++) {
		memset(&first[i], 0, sizeof first[i]);
		hull_sha256_init(&first[i].sha);
		if (layer_tar(&trees[i], i == 0u, owners, owner_count, counted, &first[i], detail,
		              detail_cap) != 0) {
			free(config.data);
			return detail;
		}
		hull_sha256_final(&first[i].sha, digest);
		hull_sha256_hex(digest, hex);
		addf(&config, "%s\"sha256:%s\"", i == 0u ? "" : ",", hex);
		total += save_entry(first[i].length);
	}
	add_text(&config, "]}}");
	if (config.failed) {
		free(config.data);
		snprintf(detail, detail_cap, "out of memory");
		return detail;
	}
	{
		hull_sha256 own;

		hull_sha256_init(&own);
		hull_sha256_update(&own, (const uint8_t *)config.data, config.length);
		hull_sha256_final(&own, digest);
		hull_sha256_hex(digest, hex);
	}
	snprintf(image, cap, "hull-dir:%s", hex);
	snprintf(path, sizeof path, "/images/%s/json", image);
	if (call("GET", path, NULL, &status, &answer) != 0) {
		free(config.data);
		snprintf(detail, detail_cap, "the Docker daemon at %s cannot be reached", socket_path());
		return detail;
	}
	free(answer);
	answer = NULL;
	if (status == 200) {
		free(config.data);
		return NULL;
	}
	if (status != 404) {
		free(config.data);
		return daemon_says(detail, detail_cap, "looking for the import", status, NULL);
	}
	addf(&manifest, "[{\"Config\":\"config.json\",\"RepoTags\":[\"%s\"],\"Layers\":[", image);
	for (unsigned i = 0u; i < count; i++)
		addf(&manifest, "%s\"%u/layer.tar\"", i == 0u ? "" : ",", i);
	add_text(&manifest, "]}]");
	total += save_entry(config.length) + save_entry(manifest.length);
	fd = dial();
	addf(&head, "POST " API "/images/load?quiet=1 HTTP/1.1\r\nHost: docker\r\n"
	     "Connection: close\r\nContent-Type: application/x-tar\r\nContent-Length: %llu\r\n\r\n",
	     total);
	if (fd < 0 || manifest.failed || head.failed ||
	    send_all(fd, head.data, head.length) != 0 ||
	    save_header(fd, "config.json", config.length) != 0 ||
	    send_all(fd, config.data, config.length) != 0 || save_pad(fd, config.length) != 0) {
		snprintf(detail, detail_cap, fd < 0 ? "the Docker daemon at %s cannot be reached" :
		                                      "the layers could not be sent to %s", socket_path());
		goto failed;
	}
	for (unsigned i = 0u; i < count; i++) {
		reading second;

		memset(&second, 0, sizeof second);
		hull_sha256_init(&second.sha);
		second.fd    = fd;
		second.limit = first[i].length;
		snprintf(name, sizeof name, "%u/layer.tar", i);
		if (save_header(fd, name, first[i].length) != 0 ||
		    layer_tar(&trees[i], i == 0u, owners, owner_count, sent, &second, detail,
		              detail_cap) != 0) {
			if (second.over)
				snprintf(detail, detail_cap, "the directory changed while it was imported");
			goto failed;
		}
		/* A tree that shrank is sent to the length promised, and its
		 * digest, which the daemon checks, then differs. */
		while (second.length < second.limit) {
			static const char zeroes[4096];
			size_t            take = second.limit - second.length < sizeof zeroes ?
			                         (size_t)(second.limit - second.length) : sizeof zeroes;

			if (send_all(fd, zeroes, take) != 0)
				break;
			second.length += take;
		}
		if (save_pad(fd, first[i].length) != 0) {
			snprintf(detail, detail_cap, "the layers could not be sent");
			goto failed;
		}
	}
	{
		static const char end[1024];

		if (save_header(fd, "manifest.json", manifest.length) != 0 ||
		    send_all(fd, manifest.data, manifest.length) != 0 ||
		    save_pad(fd, manifest.length) != 0 || send_all(fd, end, sizeof end) != 0) {
			snprintf(detail, detail_cap, "the layers could not be sent");
			goto failed;
		}
	}
	if (read_answer(fd, &status, &answer) != 0) {
		snprintf(detail, detail_cap, "the load got no answer");
		goto failed;
	}
	if (status != 200 || json_string(answer, "error", detail, detail_cap)) {
		if (status != 200)
			daemon_says(detail, detail_cap, "load", status, answer);
		goto failed;
	}
	free(answer);
	free(config.data);
	free(manifest.data);
	free(head.data);
	close(fd);
	snprintf(path, sizeof path, "sha256:%s", hex);
	retire_imports(tree, path);
	return NULL;

failed:
	free(answer);
	free(config.data);
	free(manifest.data);
	free(head.data);
	if (fd >= 0)
		close(fd);
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
		hull_sexpr_form layers;
		char           *written;
		char           *host_path;
		char           *guest;
		char           *mode;
		char            absolute[PATH_MAX];
		text            bind = { 0 };

		/* A stack of layers is laid into the image (import_stacked). */
		if (hull_access_find(doc, &item, "layers", &layers))
			continue;
		written   = hull_access_value(doc, &item, "host");
		host_path = written != NULL ? hull_access_path(doc, written) : NULL;
		guest     = hull_access_value(doc, &item, "guest");
		mode      = hull_access_value(doc, &item, "mode");
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

/* A failed create returns no handle, so it keeps its reason where the
 * caller can read it after the handle is gone. */
static char create_detail[512];
static int  resolve_in(const char *root, const char *path, char *out, size_t cap);

/* Whether the machine is a dir run in place, (mutable direct). */
static int in_place(const hull_sexpr_doc *doc, const hull_sexpr_form *machine)
{
	char *mode = hull_access_value(doc, machine, "mutable");
	int   direct = mode != NULL && strcmp(mode, "direct") == 0;

	free(mode);
	return direct;
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
	hull_device     device[HULL_MAX_DEVICES];
	unsigned        device_count = 0u;

	/* Its devices, found now: a container's are fixed when it is made.
	 * Each must be the user's to open, as on the other engines, though
	 * the daemon could open any: nothing is passed as root. */
	if ((reason = hull_access_devices(doc, machine, device, &device_count)) != NULL)
		return reason;
	for (unsigned i = 0u; i < device_count; i++)
		if (hull_hostdev_find(&device[i], create_detail, sizeof create_detail) != NULL)
			return create_detail;
	if (hull_hostdev_claim(m, device, device_count, create_detail, sizeof create_detail) != NULL)
		return create_detail;
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
	 * (network nat), or none given, its own bridge, as it always was;
	 * (network bridge) that bridge said, or the docker network (bridge
	 * NAME) names, the daemon being root already. Ports need one. */
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
		} else if (strcmp(value, "bridge") == 0) {
			char *name = hull_access_value(doc, machine, "bridge");

			if (name != NULL && strpbrk(name, "\"\\") != NULL) {
				free(name);
				free(value);
				free(body.data);
				return "a bridge's name cannot hold a quote or a backslash";
			}
			addf(&body, ",\"NetworkMode\":\"%s\"", name != NULL ? name : "bridge");
			free(name);
		}
		free(value);
	}
	if (hull_access_find(doc, machine, "volumes", &entry))
		reason = add_binds(&binds, doc, &entry, m->detail, sizeof m->detail);
	/* In place: the directory's entries, and a volume's guest path there,
	 * which docker would otherwise make in the user's tree as root. */
	if (reason == NULL && m->direct != NULL) {
		if (hull_access_find(doc, machine, "volumes", &entry)) {
			hull_sexpr_form item;

			hull_sexpr_child(doc, &entry, &item);
			for (int more = hull_sexpr_next(doc, &item, &item); more && reason == NULL;
			     more = hull_sexpr_next(doc, &item, &item)) {
				char       *guest = hull_access_value(doc, &item, "guest");
				char        resolved[4096];
				char        at[8400];
				struct stat there;

				if (guest != NULL &&
				    (resolve_in(m->direct, guest, resolved, sizeof resolved) != 0 ||
				     (snprintf(at, sizeof at, "%s/%s", m->direct, resolved),
				      lstat(at, &there) != 0))) {
					snprintf(m->detail, sizeof m->detail, "volume %s: the guest path is not in "
					         "the machine, and is made only above an overlay", guest);
					reason = m->detail;
				}
				free(guest);
			}
		}
		if (reason == NULL && m->direct_binds_length != 0u) {
			if (binds.length != 0u)
				add_text(&binds, ",");
			add(&binds, m->direct_binds, m->direct_binds_length);
		}
	}
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
	/* Each node at its own path, and again at the path the spec gave
	 * under /dev where that is a link to it, since docker makes nodes and
	 * not links. */
	if (device_count != 0u) {
		int vfio = 0;

		add_text(&body, ",\"Devices\":[");
		/* A PCI device's group node wants VFIO's container node beside it. */
		for (unsigned i = 0u; i < device_count; i++)
			vfio = vfio || device[i].kind == HULL_DEVICE_PCI;
		if (vfio)
			add_text(&body, "{\"PathOnHost\":\"" HULL_VFIO "\",\"PathInContainer\":\""
			                HULL_VFIO "\",\"CgroupPermissions\":\"rwm\"},");
		for (unsigned i = 0u; i < device_count; i++) {
			const hull_hostdev *host = &device[i].host;

			for (int named = 0; named < 2; named++) {
				const char *inside = named ? host->named : host->node;

				if (named && strncmp(inside, "/dev/", 5u) != 0)
					continue;
				add_text(&body, i == 0u && !named ? "{\"PathOnHost\":" : ",{\"PathOnHost\":");
				add_json(&body, host->node);
				add_text(&body, ",\"PathInContainer\":");
				add_json(&body, inside);
				add_text(&body, ",\"CgroupPermissions\":\"rwm\"}");
			}
		}
		add_text(&body, "]");
	}
	/* A read-only stack is its guest path made read-only, ReadonlyPaths,
	 * which replaces docker's own list, so that list is given too. */
	for (unsigned i = 0u; i < m->stack_count; i++) {
		if (!m->stacks[i].stack.readonly)
			continue;
		add_text(&body, ",\"ReadonlyPaths\":[\"/proc/bus\",\"/proc/fs\",\"/proc/irq\","
		         "\"/proc/sys\",\"/proc/sysrq-trigger\"");
		for (unsigned k = 0u; k < m->stack_count; k++) {
			text guest = { 0 };

			if (!m->stacks[k].stack.readonly)
				continue;
			add_text(&guest, "/");
			add_text(&guest, m->stacks[k].guest);
			add_text(&body, ",");
			add_json(&body, guest.data != NULL ? guest.data : "/");
			body.failed |= guest.failed;
			free(guest.data);
		}
		add_text(&body, "]");
		break;
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

static void export_filtered(hull_machine *m, int from, char *detail, size_t cap);

/* The container's root as docker export gives it, into (export PATH)'s
 * file once the workload has ended: the daemon's tar, owners and xattrs as
 * the container has them, no volume, as on chroot, and none of docker's
 * own files (export_filtered). Asked over HTTP/1.0 so
 * the body comes unchunked and ends with the connection. A failure empties
 * the file, which no commit takes, says why on the workload's stderr, and
 * makes the end 125, as on chroot. */
static void export_container(hull_machine *m, long *code)
{
	char    request[256];
	char    head[4096];
	char    detail[512] = "";
	size_t  n = 0u;
	int     status = 0;
	int     fd = dial();

	snprintf(request, sizeof request,
	         "GET " API "/containers/%s/export HTTP/1.0\r\nHost: docker\r\n\r\n", m->id);
	if (fd < 0 || send_all(fd, request, strlen(request)) != 0) {
		snprintf(detail, sizeof detail, "the Docker daemon at %s cannot be reached",
		         socket_path());
	} else {
		while (n + 1u < sizeof head && read(fd, head + n, 1u) == 1) {
			n++;
			if (n >= 4u && memcmp(head + n - 4u, "\r\n\r\n", 4u) == 0)
				break;
		}
		head[n] = '\0';
		if (sscanf(head, "HTTP/1.%*d %d", &status) != 1 || status != 200)
			snprintf(detail, sizeof detail, "the daemon answered the export %d", status);
	}
	if (detail[0] == '\0')
		export_filtered(m, fd, detail, sizeof detail);
	if (detail[0] == '\0' && fsync(m->export_fd) != 0)
		snprintf(detail, sizeof detail, "writing it: %s", strerror(errno));
	if (fd >= 0)
		close(fd);
	if (detail[0] != '\0') {
		char said[600];
		int  length = snprintf(said, sizeof said, "hull: export: %s\n", detail);

		if (ftruncate(m->export_fd, 0) != 0) {
			/* nothing more can be done; the status says it */
		}
		if (length > 0)
			host->on_data(m->core, NULL, 2, (const uint8_t *)said, (size_t)length);
		*code = 125;
	}
	close(m->export_fd);
	m->export_fd = -1;
}

/* --- the upper, written back --------------------------------------------- */

/* A tar read from a connection a block at a time. */
typedef struct tar_in {
	int     fd;
	uint8_t block[512];
} tar_in;

static int tar_block(tar_in *t)
{
	return read_full(t->fd, t->block, sizeof t->block);
}

static unsigned long long tar_number(const uint8_t *field, size_t width)
{
	unsigned long long n = 0u;

	if ((field[0] & 0x80u) != 0u) {   /* base-256, for what octal cannot hold */
		for (size_t i = 1u; i < width; i++)
			n = n << 8 | field[i];
		return n;
	}
	for (size_t i = 0u; i < width && field[i] >= '0' && field[i] <= '7'; i++)
		n = n * 8u + (unsigned)(field[i] - '0');
	return n;
}

/* `n` bytes of the stream into `out`, `cap` of them kept, the rest and the
 * block's padding read past. 0, or -1. */
static int tar_data(tar_in *t, unsigned long long n, uint8_t *out, size_t cap)
{
	unsigned long long at = 0u;

	while (at < n) {
		size_t take;

		if (tar_block(t) != 0)
			return -1;
		take = n - at < 512u ? (size_t)(n - at) : 512u;
		if (out != NULL && at < cap)
			memcpy(out + at, t->block, at + take <= cap ? take : (size_t)(cap - at));
		at += take;
	}
	return 0;
}

/* A path of the container's, "/a/b", its parent opened beneath the upper,
 * each directory on the way made where missing and never a link followed,
 * the leaf's name into `leaf`. The parent's descriptor, or -1. */
static int upper_parent(int upper, const char *path, char *leaf, size_t cap)
{
	char  copy[4096];
	char *at;
	char *slash;
	int   dir = dup(upper);

	if (dir < 0 || snprintf(copy, sizeof copy, "%s", path) >= (int)sizeof copy) {
		if (dir >= 0)
			close(dir);
		return -1;
	}
	for (at = copy; *at == '/'; at++)
		;
	while ((slash = strchr(at, '/')) != NULL) {
		int next;

		*slash = '\0';
		if (*at == '\0' || strcmp(at, ".") == 0 || strcmp(at, "..") == 0) {
			close(dir);
			return -1;
		}
		next = openat(dir, at, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
		if (next < 0 && errno == ENOENT && mkdirat(dir, at, 0755) == 0)
			next = openat(dir, at, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
		if (next < 0) {
			/* A whiteout or a file where the container has a directory:
			 * the container's directory wins, as the upper's would. */
			struct stat there;

			if (fstatat(dir, at, &there, AT_SYMLINK_NOFOLLOW) == 0 && !S_ISDIR(there.st_mode) &&
			    unlinkat(dir, at, 0) == 0 && mkdirat(dir, at, 0755) == 0)
				next = openat(dir, at, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
		}
		close(dir);
		if (next < 0)
			return -1;
		dir = next;
		at = slash + 1;
	}
	if (*at == '\0' || strcmp(at, ".") == 0 || strcmp(at, "..") == 0 ||
	    snprintf(leaf, cap, "%s", at) >= (int)cap) {
		close(dir);
		return -1;
	}
	return dir;
}

/* What is at `leaf` in `dir` removed, a directory with what it holds. */
static void upper_clear(int dir, const char *leaf)
{
	struct stat there;

	if (fstatat(dir, leaf, &there, AT_SYMLINK_NOFOLLOW) != 0)
		return;
	if (S_ISDIR(there.st_mode)) {
		int            inner = openat(dir, leaf, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
		DIR           *list = inner >= 0 ? fdopendir(inner) : NULL;
		struct dirent *found;

		if (list == NULL && inner >= 0)
			close(inner);
		while (list != NULL && (found = readdir(list)) != NULL)
			if (strcmp(found->d_name, ".") != 0 && strcmp(found->d_name, "..") != 0)
				upper_clear(dirfd(list), found->d_name);
		if (list != NULL)
			closedir(list);
		unlinkat(dir, leaf, AT_REMOVEDIR);
	} else {
		unlinkat(dir, leaf, 0);
	}
}

static int path_order(const void *a, const void *b)
{
	return strcmp(*(char *const *)a, *(char *const *)b);
}

/* Whether `path` is `mount` or beneath it. */
static int beneath(const char *path, const char *mount)
{
	size_t length = strlen(mount);

	return strncmp(path, mount, length) == 0 && (path[length] == '\0' || path[length] == '/');
}

/* What docker mounts in every container, which its list of changes names
 * though no workload wrote it: its init, its marker, the files it binds
 * over /etc, and the kernel's filesystems. */
static const char *const docker_mounts[] = {
	"/.dockerenv", "/sbin/docker-init", "/etc/hosts", "/etc/hostname", "/etc/resolv.conf",
	"/dev", "/proc", "/sys", NULL
};

/* Whether a change at `path` is docker's or a mount's rather than the
 * workload's: at or beneath one of `mounts`, or a directory docker added
 * only to hold one, nothing the workload kept beneath it. */
static int not_written(const char *path, int added, char *const *mounts, size_t mount_count,
                       char *const *kept, size_t kept_count)
{
	int holds = 0;

	for (size_t i = 0u; i < mount_count; i++) {
		if (beneath(path, mounts[i]))
			return 1;
		holds |= beneath(mounts[i], path);
	}
	if (!added || !holds)
		return 0;
	for (size_t i = 0u; i < kept_count; i++) {
		int mine = strcmp(kept[i], path) != 0 && beneath(kept[i], path);

		for (size_t j = 0u; mine && j < mount_count; j++)
			if (beneath(kept[i], mounts[j]))
				mine = 0;
		if (mine)
			return 0;
	}
	return 1;
}

/* One entry of the export, `path` the container's, put in the upper as
 * the container has it, the user's. 0; 1 where it is a kind the upper
 * does not take; -1. */
static int upper_put(int upper, const char *path, char type, unsigned mode,
                     unsigned long long mtime, const char *link, tar_in *t,
                     unsigned long long size)
{
	char leaf[256];
	int  dir = upper_parent(upper, path, leaf, sizeof leaf);
	int  result = 0;

	if (dir < 0)
		return -1;
	if (type == '5') {
		struct stat there;

		if (fstatat(dir, leaf, &there, AT_SYMLINK_NOFOLLOW) == 0 && !S_ISDIR(there.st_mode))
			upper_clear(dir, leaf);
		if ((mkdirat(dir, leaf, 0700) != 0 && errno != EEXIST) ||
		    fchmodat(dir, leaf, mode & 07777, 0) != 0)
			result = -1;
	} else {
		upper_clear(dir, leaf);
		if (type == '0' || type == '7') {
			int                fd = openat(dir, leaf, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW |
			                               O_CLOEXEC, 0600);
			unsigned long long at = 0u;

			while (fd >= 0 && at < size && result == 0) {
				size_t take;

				if (tar_block(t) != 0) {
					result = -1;
					break;
				}
				take = size - at < 512u ? (size_t)(size - at) : 512u;
				if (send_all(fd, (const char *)t->block, take) != 0)
					result = -1;
				at += take;
			}
			if (fd < 0)
				result = -1;
			size = 0u;   /* read */
			if (fd >= 0) {
				struct timespec times[2] = { { .tv_nsec = UTIME_OMIT },
				                             { .tv_sec = (time_t)mtime } };

				if (fchmod(fd, mode & 07777) != 0)
					result = -1;
				futimens(fd, times);
				close(fd);
			}
		} else if (type == '2') {
			if (symlinkat(link, dir, leaf) != 0)
				result = -1;
		} else if (type == '6') {
			if (mknodat(dir, leaf, S_IFIFO | (mode & 0777), 0) != 0)
				result = -1;
		} else {
			result = 1;   /* a hard link or a device: not kept */
		}
	}
	close(dir);
	if (size != 0u && tar_data(t, size, NULL, 0u) != 0)
		return -1;
	return result;
}

/* Where a change at `full` is kept: the upper of the volume's stack it is
 * beneath, `*relative` then its path there, or the root's upper, unless
 * it is no write of the workload's (not_written) or a stack's own mount
 * point. The upper's descriptor, or -1 where nothing keeps it. */
static int upper_of(const hull_machine *m, const char *full, int added, char *const *mounts,
                    size_t mount_count, char *const *kept, size_t kept_count,
                    const char **relative)
{
	*relative = full;
	for (unsigned k = 0u; k < m->stack_count; k++) {
		size_t length = strlen(m->stacks[k].guest);

		if (strncmp(full + 1, m->stacks[k].guest, length) != 0 ||
		    (full[1u + length] != '\0' && full[1u + length] != '/'))
			continue;
		if (full[1u + length] == '\0')
			return -1;
		*relative = full + 1u + length;
		return m->stacks[k].upper_fd;
	}
	if (m->upper_fd < 0 || not_written(full, added, mounts, mount_count, kept, kept_count))
		return -1;
	return m->upper_fd;
}

/*
 * (upper PATH)'s writes, kept as an overlay's upper keeps them, from the
 * container docker made of the layers with the upper on top: what the
 * daemon lists as changed, /containers/ID/changes, and what each added or
 * changed entry now is, from docker export. A deletion is a whiteout, a
 * character device 0/0, which the kernel lets the user make; a directory
 * made again over one below is listed by docker as each child deleted,
 * so a whiteout each says the same. Every entry is the user's, as an
 * upper on disk is on chroot (one uid). A hard link or a device the
 * container made is not kept, and said. A failure says why on the
 * workload's stderr and makes the end 125, as an export's does.
 */
static void write_upper(hull_machine *m, long *code)
{
	char         path[160];
	char         request[256];
	char         head[4096];
	char         detail[512] = "";
	char        *answer = NULL;
	char       **kept = NULL;
	char       **gone = NULL;
	char        *mounts[64];
	size_t       mount_count = 0u;
	size_t       kept_count = 0u;
	size_t       gone_count = 0u;
	unsigned     dropped = 0u;
	int          status = 0;
	int          fd = -1;
	size_t       n = 0u;
	tar_in       t;
	const char  *at;
	char        *name = malloc(4096u);
	char        *link = malloc(4096u);
	uint8_t     *records = malloc(65536u);

	/* Its mounts, which docker's list of changes names: docker's own and
	 * each volume's destination. */
	for (unsigned i = 0u; docker_mounts[i] != NULL; i++)
		mounts[mount_count++] = (char *)docker_mounts[i];
	snprintf(path, sizeof path, "/containers/%s/json", m->id);
	if (call("GET", path, NULL, &status, &answer) != 0 || status != 200) {
		snprintf(detail, sizeof detail, "the container's mounts could not be read");
		goto said;
	}
	for (at = strstr(answer, "\"Destination\":\""); at != NULL && mount_count < 64u;
	     at = strstr(at, "\"Destination\":\"")) {
		size_t k = 0u;
		char   found[4096];

		for (at += 15; *at != '"' && *at != '\0' && k + 1u < sizeof found; at++)
			found[k++] = *at;
		found[k] = '\0';
		if ((mounts[mount_count] = strdup(found)) != NULL)
			mount_count++;
	}
	free(answer);
	answer = NULL;
	/* Each volume's stack is a mount to the root's upper, as on chroot. */
	for (unsigned k = 0u; k < m->stack_count && mount_count < 64u; k++) {
		size_t length = strlen(m->stacks[k].guest);

		if ((mounts[mount_count] = malloc(length + 2u)) != NULL) {
			mounts[mount_count][0] = '/';
			memcpy(mounts[mount_count] + 1, m->stacks[k].guest, length + 1u);
			mount_count++;
		}
	}
	if (name == NULL || link == NULL || records == NULL) {
		snprintf(detail, sizeof detail, "out of memory");
		goto said;
	}
	snprintf(path, sizeof path, "/containers/%s/changes", m->id);
	if (call("GET", path, NULL, &status, &answer) != 0 || status != 200) {
		snprintf(detail, sizeof detail, "the daemon's list of changes: %s",
		         status != 0 ? "refused" : "not read, or too long");
		goto said;
	}
	for (at = answer != NULL ? strstr(answer, "\"Path\":\"") : NULL; at != NULL;
	     at = strstr(at, "\"Path\":\"")) {
		char        found[4096];
		size_t      k = 0u;
		long        kind = -1;
		const char *kind_at;
		char     ***list;
		size_t     *count;

		for (at += 8; *at != '"' && *at != '\0' && k + 1u < sizeof found; at++) {
			if (*at == '\\' && at[1] != '\0')
				at++;
			found[k++] = *at;
		}
		found[k] = '\0';
		kind_at = strstr(at, "\"Kind\":");
		if (kind_at != NULL)
			kind = strtol(kind_at + 7, NULL, 10);
		list  = kind == 2 ? &gone : &kept;
		count = kind == 2 ? &gone_count : &kept_count;
		/* An added one carries its kind as a NUL after its path. */
		if ((*count & 1023u) == 0u) {
			char **grown = realloc(*list, (*count + 1024u) * sizeof **list);

			if (grown == NULL) {
				snprintf(detail, sizeof detail, "out of memory");
				goto said;
			}
			*list = grown;
		}
		if (((*list)[*count] = malloc(k + 2u)) != NULL) {
			memcpy((*list)[*count], found, k + 1u);
			(*list)[*count][k + 1u] = kind == 1 ? 'A' : 'C';
		}
		if ((*list)[*count] == NULL) {
			snprintf(detail, sizeof detail, "out of memory");
			goto said;
		}
		(*count)++;
	}
	/* Deletions first, a path beneath one already deleted skipped. */
	qsort(gone, gone_count, sizeof *gone, path_order);
	for (size_t i = 0u; i < gone_count; i++) {
		char        leaf[256];
		int         dir;
		int         target;
		const char *relative;
		size_t      under = 0u;

		for (size_t j = 0u; j < i; j++) {
			size_t length = strlen(gone[j]);

			if (strncmp(gone[i], gone[j], length) == 0 && gone[i][length] == '/')
				under = 1u;
		}
		if (under ||
		    (target = upper_of(m, gone[i], 0, mounts, mount_count, kept, kept_count,
		                       &relative)) < 0)
			continue;
		if ((dir = upper_parent(target, relative, leaf, sizeof leaf)) < 0) {
			snprintf(detail, sizeof detail, "%.400s: not a path the upper can hold", gone[i]);
			goto said;
		}
		upper_clear(dir, leaf);
		if (mknodat(dir, leaf, S_IFCHR, makedev(0, 0)) != 0) {
			snprintf(detail, sizeof detail, "%.400s's whiteout: %s", gone[i], strerror(errno));
			close(dir);
			goto said;
		}
		close(dir);
	}
	qsort(kept, kept_count, sizeof *kept, path_order);

	/* What each added or changed entry is, from the export. */
	fd = dial();
	snprintf(request, sizeof request,
	         "GET " API "/containers/%s/export HTTP/1.0\r\nHost: docker\r\n\r\n", m->id);
	if (fd < 0 || send_all(fd, request, strlen(request)) != 0) {
		snprintf(detail, sizeof detail, "the Docker daemon at %s cannot be reached",
		         socket_path());
		goto said;
	}
	while (n + 1u < sizeof head && read(fd, head + n, 1u) == 1) {
		n++;
		if (n >= 4u && memcmp(head + n - 4u, "\r\n\r\n", 4u) == 0)
			break;
	}
	head[n] = '\0';
	if (sscanf(head, "HTTP/1.%*d %d", &status) != 1 || status != 200) {
		snprintf(detail, sizeof detail, "the daemon answered the export %d", status);
		goto said;
	}
	t.fd = fd;
	name[0] = '\0';
	link[0] = '\0';
	for (;;) {
		unsigned long long size;
		char               type;
		char               full[4200];
		const char        *key = full;
		char             **found_kept;
		const char        *relative;
		int                target = -1;
		int                put;

		if (tar_block(&t) != 0) {
			snprintf(detail, sizeof detail, "the export ended before its end");
			goto said;
		}
		if (t.block[0] == '\0')
			break;
		size = tar_number(t.block + 124, 12u);
		type = (char)t.block[156];
		if (type == 'x' || type == 'L' || type == 'K') {
			if (size >= 65536u || tar_data(&t, size, records, 65536u) != 0) {
				snprintf(detail, sizeof detail, "an export's header past what hull reads");
				goto said;
			}
			records[size] = '\0';
			if (type == 'L')
				snprintf(name, 4096u, "%.4095s", (char *)records);
			else if (type == 'K')
				snprintf(link, 4096u, "%.4095s", (char *)records);
			else
				for (size_t r = 0u; r < size;) {
					unsigned long length = strtoul((char *)records + r, NULL, 10);
					char         *field = strchr((char *)records + r, ' ');

					if (length == 0u || field == NULL || r + length > size)
						break;
					records[r + length - 1u] = '\0';   /* its newline */
					if (strncmp(field + 1, "path=", 5u) == 0)
						snprintf(name, 4096u, "%.4095s", field + 6);
					else if (strncmp(field + 1, "linkpath=", 9u) == 0)
						snprintf(link, 4096u, "%.4095s", field + 10);
					r += length;
				}
			continue;
		}
		if (type == 'g') {
			if (tar_data(&t, size, NULL, 0u) != 0)
				break;
			continue;
		}
		if (name[0] == '\0') {
			char prefix[156];
			char own[101];

			memcpy(prefix, t.block + 345, 155u);
			prefix[155] = '\0';
			memcpy(own, t.block, 100u);
			own[100] = '\0';
			snprintf(name, 4096u, "%s%s%s", prefix, prefix[0] != '\0' ? "/" : "", own);
		}
		if (link[0] == '\0') {
			memcpy(link, t.block + 157, 100u);
			link[100] = '\0';
		}
		/* "/a/b", as the daemon lists it. */
		{
			const char *bare = name;
			size_t      length;

			while (bare[0] == '.' && bare[1] == '/')
				bare += 2;
			while (*bare == '/')
				bare++;
			snprintf(full, sizeof full, "/%s", bare);
			length = strlen(full);
			while (length > 1u && full[length - 1u] == '/')
				full[--length] = '\0';
		}
		put = 0;
		found_kept = strcmp(full, "/") != 0 ?
		             bsearch(&key, kept, kept_count, sizeof *kept, path_order) : NULL;
		if (found_kept != NULL &&
		    (target = upper_of(m, full, (*found_kept)[strlen(*found_kept) + 1u] == 'A', mounts,
		                       mount_count, kept, kept_count, &relative)) >= 0) {
			put = upper_put(target, relative, type == '\0' ? '0' : type,
			                (unsigned)tar_number(t.block + 100, 8u), tar_number(t.block + 136, 12u),
			                link, &t, type == '5' ? 0u : size);
			if (put < 0) {
				snprintf(detail, sizeof detail, "%.400s: %s", full, strerror(errno));
				goto said;
			}
			dropped += put == 1;
			if (type == '5' && size != 0u && tar_data(&t, size, NULL, 0u) != 0) {
				snprintf(detail, sizeof detail, "the export ended before its end");
				goto said;
			}
		} else if (tar_data(&t, size, NULL, 0u) != 0) {
			snprintf(detail, sizeof detail, "the export ended before its end");
			goto said;
		}
		name[0] = '\0';
		link[0] = '\0';
	}
	if (dropped != 0u) {
		char said[200];
		int  length = snprintf(said, sizeof said,
		                       "hull: upper: %u hard links or devices the machine made not kept\n",
		                       dropped);

		if (length > 0)
			host->on_data(m->core, NULL, 2, (const uint8_t *)said, (size_t)length);
	}

said:
	if (detail[0] != '\0') {
		char said[600];
		int  length = snprintf(said, sizeof said, "hull: upper: %s\n", detail);

		if (length > 0)
			host->on_data(m->core, NULL, 2, (const uint8_t *)said, (size_t)length);
		*code = 125;
	}
	if (fd >= 0)
		close(fd);
	for (size_t i = 0u; i < mount_count; i++)   /* the volumes', strdup'd */
		if (i >= sizeof docker_mounts / sizeof *docker_mounts - 1u)
			free(mounts[i]);
	for (size_t i = 0u; i < kept_count; i++)
		free(kept[i]);
	for (size_t i = 0u; i < gone_count; i++)
		free(gone[i]);
	free(kept);
	free(gone);
	free(answer);
	free(name);
	free(link);
	free(records);
}

/* An entry's header of the export hull writes itself, ustar, its name
 * short enough for one: 0, or -1. */
static int export_header(int fd, const char *name, char type, unsigned mode, unsigned long long uid,
                         unsigned long long gid, unsigned long long size, unsigned long long mtime)
{
	uint8_t  block[512];
	unsigned sum = 0u;

	memset(block, 0, sizeof block);
	snprintf((char *)block, 100u, "%s", name);
	snprintf((char *)block + 100, 8u, "%07o", mode & 07777u);
	snprintf((char *)block + 108, 8u, "%07llo", uid & 07777777u);
	snprintf((char *)block + 116, 8u, "%07llo", gid & 07777777u);
	snprintf((char *)block + 124, 12u, "%011llo", size);
	snprintf((char *)block + 136, 12u, "%011llo", mtime & 077777777777u);
	memset(block + 148, ' ', 8u);
	block[156] = (uint8_t)type;
	memcpy(block + 257, "ustar", 6u);
	memcpy(block + 263, "00", 2u);
	for (size_t i = 0u; i < sizeof block; i++)
		sum += block[i];
	snprintf((char *)block + 148, 8u, "%06o", sum);
	return send_all(fd, (const char *)block, sizeof block);
}

/* Where the machine's trees hold `relative`, "etc/hosts": the topmost
 * tree's file into `found`, 1; 0 where none holds it or the topmost that
 * says anything removed it, a whiteout or an opaque etc/ above it. */
static int tree_file(const hull_machine *m, const char *relative, char *found, size_t cap)
{
	for (unsigned i = m->tree_count; i-- > 0u;) {
		struct stat there;
		char        dir[4200];
		char        opaque[2];

		snprintf(found, cap, "%s/%s", m->trees[i], relative);
		if (lstat(found, &there) == 0) {
			if (S_ISCHR(there.st_mode) && there.st_rdev == makedev(0, 0))
				return 0;
			return S_ISREG(there.st_mode);
		}
		snprintf(dir, sizeof dir, "%s/etc", m->trees[i]);
		if (lgetxattr(dir, "user.overlay.opaque", opaque, sizeof opaque) == 1 && opaque[0] == 'y')
			return 0;
	}
	return 0;
}

/*
 * docker export's tar from `from` into the export's file without what
 * docker puts in every container and no tree held: its marker
 * /.dockerenv, its init /sbin/docker-init and a /sbin made only for it;
 * and docker's empty /etc/hosts, /etc/hostname and /etc/resolv.conf,
 * which an init layer lays over the image's for the files it binds there,
 * replaced by the topmost tree's, or left out where no tree has one. So a
 * hull build step on docker keeps its parent's /etc/resolv.conf rather
 * than committing docker's empty one, and an export is the tree chroot's
 * would be. A ref's source has no tree here: its three are docker's. Each
 * other entry, with the pax or long-name headers before it, goes on as
 * the daemon wrote it. Sets `detail` on a failure.
 */
static void export_filtered_into(hull_machine *m, int from, char *detail, size_t cap,
                                 uint8_t *held, size_t held_cap, char *name, size_t name_cap);

/* Its buffers its own, the machine's reader thread being one of several. */
static void export_filtered(hull_machine *m, int from, char *detail, size_t cap)
{
	uint8_t *held = malloc(65536u + 1024u);
	char    *name = malloc(4096u);

	if (held == NULL || name == NULL)
		snprintf(detail, cap, "out of memory");
	else
		export_filtered_into(m, from, detail, cap, held, 65536u + 1024u, name, 4096u);
	free(held);
	free(name);
}

static void export_filtered_into(hull_machine *m, int from, char *detail, size_t cap,
                                 uint8_t *held, size_t held_cap, char *name, size_t name_cap)
{
	static const char zeroes[512];
	size_t         held_length = 0u;
	tar_in         t;
	int            sbin = 0;
	int            to = m->export_fd;

	for (unsigned i = 0u; i < m->tree_count; i++) {
		char        at[4200];
		struct stat there;

		snprintf(at, sizeof at, "%s/sbin", m->trees[i]);
		sbin |= lstat(at, &there) == 0;
	}
	t.fd = from;
	name[0] = '\0';
	for (;;) {
		unsigned long long size;
		char               type;
		char               full[4200];
		const char        *bare;
		size_t             length;
		int                drop = 0;
		static const char *const etc[] = { "/etc/hosts", "/etc/hostname", "/etc/resolv.conf",
		                                   NULL };

		if (tar_block(&t) != 0) {
			snprintf(detail, cap, "the export ended before its end");
			return;
		}
		if (t.block[0] == '\0') {
			static const char end[1024];

			if (send_all(to, end, sizeof end) != 0)
				snprintf(detail, cap, "writing it: %s", strerror(errno));
			return;
		}
		size = tar_number(t.block + 124, 12u);
		type = (char)t.block[156];
		if (type == 'x' || type == 'L' || type == 'g') {
			unsigned long long blocks = (size + 511u) / 512u;
			uint8_t           *records = held + held_length + 512u;

			if (held_length + 512u + blocks * 512u > held_cap) {
				snprintf(detail, cap, "an export's header past what hull reads");
				return;
			}
			memcpy(held + held_length, t.block, 512u);
			if (tar_data(&t, size, records, (size_t)(blocks * 512u)) != 0) {
				snprintf(detail, cap, "the export ended before its end");
				return;
			}
			held_length += 512u + (size_t)blocks * 512u;
			if (type == 'L') {
				snprintf(name, name_cap, "%.*s", (int)(size < name_cap ? size :
				                                                 name_cap - 1u),
				         (char *)records);
			} else if (type == 'x') {
				for (size_t r = 0u; r < size;) {
					unsigned long field_length = strtoul((char *)records + r, NULL, 10);
					char         *field = memchr(records + r, ' ', (size_t)(size - r));

					if (field_length == 0u || field == NULL || r + field_length > size)
						break;
					if (strncmp(field + 1, "path=", 5u) == 0)
						snprintf(name, name_cap, "%.*s",
						         (int)((char *)records + r + field_length - 1 - (field + 6)),
						         field + 6);
					r += field_length;
				}
			}
			continue;
		}
		if (name[0] == '\0') {
			char prefix[156];
			char own[101];

			memcpy(prefix, t.block + 345, 155u);
			prefix[155] = '\0';
			memcpy(own, t.block, 100u);
			own[100] = '\0';
			snprintf(name, name_cap, "%s%s%s", prefix, prefix[0] != '\0' ? "/" : "", own);
		}
		for (bare = name; bare[0] == '.' && bare[1] == '/'; bare += 2)
			;
		while (*bare == '/')
			bare++;
		snprintf(full, sizeof full, "/%s", bare);
		length = strlen(full);
		while (length > 1u && full[length - 1u] == '/')
			full[--length] = '\0';
		drop = strcmp(full, "/.dockerenv") == 0 || strcmp(full, "/sbin/docker-init") == 0 ||
		       (m->tree_count != 0u && !sbin && strcmp(full, "/sbin") == 0);
		/* What a volume's stack holds is the volume's, as chroot's export
		 * leaves a mount point empty. */
		for (unsigned k = 0u; !drop && k < m->stack_count; k++) {
			size_t g = strlen(m->stacks[k].guest);

			drop = strncmp(full + 1, m->stacks[k].guest, g) == 0 && full[1u + g] == '/';
		}
		for (unsigned i = 0u; !drop && m->tree_count != 0u && etc[i] != NULL; i++) {
			char        found[4200];
			struct stat there;
			int         given;

			if (strcmp(full, etc[i]) != 0)
				continue;
			drop = 1;
			if (tree_file(m, etc[i] + 1, found, sizeof found) && lstat(found, &there) == 0) {
				int file = open(found, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);

				given = file >= 0 &&
				        export_header(to, etc[i] + 1, '0', (unsigned)there.st_mode,
				                      there.st_uid, there.st_gid,
				                      (unsigned long long)there.st_size,
				                      (unsigned long long)there.st_mtime) == 0;
				for (off_t sent = 0; given && sent < there.st_size;) {
					char    chunk[4096];
					ssize_t got = read(file, chunk, sizeof chunk);

					if (got <= 0 || send_all(to, chunk, (size_t)got) != 0)
						given = 0;
					else
						sent += got;
				}
				if (file >= 0)
					close(file);
				if (!given || (there.st_size % 512 != 0 &&
				               send_all(to, zeroes, 512u - (size_t)(there.st_size % 512)) != 0)) {
					snprintf(detail, cap, "%s, from the machine's tree: %s", etc[i],
					         strerror(errno));
					return;
				}
			}
		}
		if (drop) {
			if (tar_data(&t, size, NULL, 0u) != 0) {
				snprintf(detail, cap, "the export ended before its end");
				return;
			}
		} else {
			unsigned long long left = (size + 511u) / 512u;

			if ((held_length != 0u && send_all(to, (const char *)held, held_length) != 0) ||
			    send_all(to, (const char *)t.block, 512u) != 0) {
				snprintf(detail, cap, "writing it: %s", strerror(errno));
				return;
			}
			for (; left > 0u; left--)
				if (tar_block(&t) != 0 || send_all(to, (const char *)t.block, 512u) != 0) {
					snprintf(detail, cap, "the export ended before its end");
					return;
				}
		}
		held_length = 0u;
		name[0] = '\0';
	}
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
	{
		int keeps = m->upper_fd >= 0;

		for (unsigned k = 0u; k < m->stack_count; k++)
			keeps |= m->stacks[k].upper_fd >= 0;
		if (keeps)
			write_upper(m, &code);
	}
	if (m->export_fd >= 0)
		export_container(m, &code);
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
	hull_hostdev_unclaim(m);
	if (m->export_fd >= 0)
		close(m->export_fd);
	if (m->upper_fd >= 0)
		close(m->upper_fd);
	for (unsigned i = 0u; i < m->tree_count; i++)
		free(m->trees[i]);
	free(m->direct);
	free(m->direct_binds);
	for (unsigned i = 0u; i < m->stack_count; i++) {
		free(m->stacks[i].guest);
		hull_stack_release(&m->stacks[i].stack);
		if (m->stacks[i].upper_fd >= 0)
			close(m->stacks[i].upper_fd);
	}
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


/* (owners PATH)'s list, as engine/idmap.c reads it -- UID, GID and a path
 * of the tree, each ended by a NUL -- into `list`, sorted by path, its
 * text held in `*data`, for the import's tar to give each its owner, as
 * chroot gives them in its namespace: the workload runs as the user, so
 * a docker import is where an owner can be given without root. A
 * reason, or NULL. */
static const char *read_owners(const char *path, char **data, hull_tree_owner **list,
                               size_t *count)
{
	FILE  *f = fopen(path, "re");
	size_t length = 0u;
	size_t cap = 0u;
	size_t n;

	*data  = NULL;
	*list  = NULL;
	*count = 0u;
	if (f == NULL)
		return "the owners' file cannot be read";
	for (;;) {
		char *more;

		if (length == cap) {
			if (cap >= (64u << 20) || (more = realloc(*data, cap + 65536u)) == NULL) {
				fclose(f);
				return "the owners' file is too large";
			}
			*data = more;
			cap += 65536u;
		}
		n = fread(*data + length, 1u, cap - length, f);
		length += n;
		if (n == 0u)
			break;
	}
	fclose(f);
	for (size_t at = 0u; at < length;) {
		const char      *fields[3];
		unsigned         k = 0u;
		hull_tree_owner *grown;

		for (; k < 3u && at < length; k++) {
			fields[k] = *data + at;
			while (at < length && (*data)[at] != '\0')
				at++;
			if (at == length)
				break;
			at++;
		}
		if (k < 3u)
			break;
		if ((*count & 1023u) == 0u) {
			grown = realloc(*list, (*count + 1024u) * sizeof **list);
			if (grown == NULL)
				return "out of memory";
			*list = grown;
		}
		(*list)[*count].path = fields[2];
		(*list)[*count].uid  = strtoull(fields[0], NULL, 10);
		(*list)[*count].gid  = strtoull(fields[1], NULL, 10);
		(*count)++;
	}
	if (*count > 1u)
		qsort(*list, *count, sizeof **list, owner_order);
	return NULL;
}

/* The image the machine runs: its ref, or its dir's import, with the
 * owners (owners PATH) gives. A reason, or NULL. */
/* --- a directory in place ---------------------------------------------- */

static const char *direct_image(const char *root, const text *body, char *image, size_t cap,
                                char *detail, size_t detail_cap);

/* A ustar entry of the frame's image, into `t`: a directory or a symlink,
 * its name short enough for ustar's field. 0, or -1. */
static int frame_entry(text *t, const char *name, char type, unsigned mode, const char *link)
{
	char     block[512];
	unsigned sum = 0u;

	if (strlen(name) >= 100u || (link != NULL && strlen(link) >= 100u))
		return -1;
	memset(block, 0, sizeof block);
	memcpy(block, name, strlen(name));
	snprintf(block + 100, 8u, "%07o", mode & 07777u);
	memcpy(block + 108, "0000000", 7u);
	memcpy(block + 116, "0000000", 7u);
	memcpy(block + 124, "00000000000", 11u);
	memcpy(block + 136, "00000000000", 11u);
	memset(block + 148, ' ', 8u);
	block[156] = type;
	if (link != NULL)
		memcpy(block + 157, link, strlen(link));
	memcpy(block + 257, "ustar", 6u);
	memcpy(block + 263, "00", 2u);
	for (size_t i = 0u; i < sizeof block; i++)
		sum += (unsigned char)block[i];
	snprintf(block + 148, 8u, "%06o", sum);
	add(t, block, sizeof block);
	return t->failed ? -1 : 0;
}

/* `path`, "/etc/hosts", as the machine would find it in the tree at
 * `root`: each symlink on the way followed inside the tree, an absolute
 * one from its root, so that a /sbin linked to usr/sbin is usr/sbin. The
 * real relative path into `out`, "usr/sbin/docker-init"; what is missing
 * is kept as written. 0, or -1 for a loop or a path too long. */
static int resolve_in(const char *root, const char *path, char *out, size_t cap)
{
	char     rest[4096];
	char     done[4096] = "";
	unsigned links = 0u;

	snprintf(rest, sizeof rest, "%s", path);
	for (;;) {
		char       *at = rest;
		char       *slash;
		char        component[256];
		char        here[8400];
		char        target[4096];
		struct stat there;
		ssize_t     n;

		while (*at == '/')
			at++;
		if (*at == '\0')
			break;
		slash = strchr(at, '/');
		snprintf(component, sizeof component, "%.*s", slash != NULL ? (int)(slash - at) :
		         (int)strlen(at), at);
		memmove(rest, slash != NULL ? slash : "", strlen(slash != NULL ? slash : "") + 1u);
		if (strcmp(component, ".") == 0)
			continue;
		if (strcmp(component, "..") == 0) {
			char *up = strrchr(done, '/');

			if (up != NULL)
				*up = '\0';
			else
				done[0] = '\0';
			continue;
		}
		snprintf(here, sizeof here, "%s/%s%s%s", root, done, done[0] != '\0' ? "/" : "",
		         component);
		if (lstat(here, &there) == 0 && S_ISLNK(there.st_mode)) {
			char joined[8200];

			if (++links > 40u || (n = readlink(here, target, sizeof target - 1u)) < 0)
				return -1;
			target[n] = '\0';
			if (target[0] == '/')
				done[0] = '\0';
			snprintf(joined, sizeof joined, "%s%s", target, rest);
			if (strlen(joined) >= sizeof rest)
				return -1;
			memcpy(rest, joined, strlen(joined) + 1u);
			continue;
		}
		if (strlen(done) + strlen(component) + 2u >= sizeof done)
			return -1;
		if (done[0] != '\0')
			strcat(done, "/");
		strcat(done, component);
	}
	return snprintf(out, cap, "%s", done) >= (int)cap ? -1 : 0;
}

/* Whether `rel` is one of the `count` directories split. */
static int is_split(char *const *split, size_t count, const char *rel)
{
	for (size_t i = 0u; i < count; i++)
		if (strcmp(split[i], rel) == 0)
			return 1;
	return 0;
}

/*
 * The tree at `root` bound into a container entry by entry, `rel` the
 * directory walked, "" the root: each entry a bind into `binds`, `:ro`
 * with `readonly`, but a symlink, which goes into `frame` as itself rather
 * than followed on the host; a split directory, one docker mounts
 * something inside, walked in turn, it being the frame's own, root's, so
 * that what docker makes there is made in the image and never in the
 * user's tree, and a new file there is refused rather than kept nowhere;
 * and what docker mounts itself, /proc, /sys, /dev and `skip`, left out.
 * A reason, or NULL.
 */
static const char *bind_tree(const char *root, const char *rel, char *const *split, size_t count,
                             char *const *skip, size_t skip_count, int readonly, text *binds,
                             text *frame)
{
	char           here[4200];
	DIR           *list;
	struct dirent *found;
	char         **names = NULL;
	size_t         n = 0u;
	const char    *reason = NULL;

	snprintf(here, sizeof here, "%s%s%s", root, rel[0] != '\0' ? "/" : "", rel);
	if ((list = opendir(here)) == NULL)
		return "the directory cannot be read";
	while ((found = readdir(list)) != NULL) {
		char **grown;

		if (strcmp(found->d_name, ".") == 0 || strcmp(found->d_name, "..") == 0)
			continue;
		if ((grown = realloc(names, (n + 1u) * sizeof *names)) == NULL ||
		    (grown[n] = strdup(found->d_name)) == NULL) {
			names = grown != NULL ? grown : names;
			reason = "out of memory";
			break;
		}
		names = grown;
		n++;
	}
	closedir(list);
	if (names != NULL)
		qsort(names, n, sizeof *names, path_order);
	for (size_t i = 0u; i < n && reason == NULL; i++) {
		char        child[4200];
		char        host[8400];
		struct stat there;
		int         skipped = 0;

		snprintf(child, sizeof child, "%s%s%s", rel, rel[0] != '\0' ? "/" : "", names[i]);
		snprintf(host, sizeof host, "%s/%s", root, child);
		for (size_t k = 0u; k < skip_count; k++)
			skipped |= strcmp(skip[k], child) == 0;
		if (skipped || (rel[0] == '\0' && (strcmp(child, "proc") == 0 ||
		                                   strcmp(child, "sys") == 0 ||
		                                   strcmp(child, "dev") == 0)))
			continue;
		if (lstat(host, &there) != 0) {
			reason = "an entry of the directory went while it was read";
		} else if (S_ISLNK(there.st_mode)) {
			char    target[4096];
			ssize_t length = readlink(host, target, sizeof target - 1u);

			if (length < 0)
				reason = "a symlink of the directory cannot be read";
			else {
				target[length] = '\0';
				if (frame_entry(frame, child, '2', 0777u, target) != 0)
					reason = "a symlink's path or target is past ustar's 100 bytes";
			}
		} else if (S_ISDIR(there.st_mode) && is_split(split, count, child)) {
			char dir_name[4208];

			snprintf(dir_name, sizeof dir_name, "%s/", child);
			if (frame_entry(frame, dir_name, '5', 0755u, NULL) != 0)
				reason = "a directory's path is past ustar's 100 bytes";
			else
				reason = bind_tree(root, child, split, count, skip, skip_count, readonly, binds,
				                   frame);
		} else {
			text bind = { 0 };

			add_text(&bind, host);
			add_text(&bind, ":/");
			add_text(&bind, child);
			add_text(&bind, readonly ? ":ro" : ":rw");
			if (binds->length != 0u)
				add_text(binds, ",");
			add_json(binds, bind.data != NULL ? bind.data : "");
			binds->failed |= bind.failed;
			free(bind.data);
		}
	}
	for (size_t i = 0u; i < n; i++)
		free(names[i]);
	free(names);
	return reason;
}

/*
 * (mutable direct)'s frame: what docker mounts in every container, its
 * init and the three /etc files, and a secrets directory where there is
 * one, each resolved in the tree; every directory above each split, so
 * docker makes its mount point in the image rather than in the user's
 * tree, where it would make it as root; the tree's symlinks among them
 * and at its top as the image's own; the rest bound into `binds`. The
 * image is imported, and its name into `image`. A reason, or NULL.
 */
static const char *frame_direct(hull_machine *m, int secrets, int readonly, text *binds,
                                char *image, size_t cap)
{
	static const char *const targets[] = {
		"/sbin/docker-init", "/etc/hosts", "/etc/hostname", "/etc/resolv.conf",
		"/run/hull/secrets", NULL
	};
	char       *split[64];
	char       *skip[8];
	size_t      split_count = 0u;
	size_t      skip_count = 0u;
	text        frame = { 0 };
	const char *reason = NULL;
	char        zeroes[1024];

	for (unsigned i = 0u; targets[i] != NULL && reason == NULL; i++) {
		char resolved[4096];
		char cut[4096];

		if (strcmp(targets[i], "/run/hull/secrets") == 0 && !secrets)
			continue;
		if (resolve_in(m->direct, targets[i], resolved, sizeof resolved) != 0) {
			reason = "a symlink in the directory loops, or a path in it is too long";
			break;
		}
		/* The target itself is docker's, not bound; each above it split. */
		if (skip_count < 8u && (skip[skip_count] = strdup(resolved)) != NULL)
			skip_count++;
		snprintf(cut, sizeof cut, "%s", resolved);
		for (char *slash = strrchr(cut, '/'); slash != NULL; slash = strrchr(cut, '/')) {
			*slash = '\0';
			if (!is_split(split, split_count, cut) && split_count < 64u &&
			    (split[split_count] = strdup(cut)) != NULL)
				split_count++;
		}
	}
	if (reason == NULL)
		reason = bind_tree(m->direct, "", split, split_count, skip, skip_count, readonly, binds,
		                   &frame);
	memset(zeroes, 0, sizeof zeroes);
	add(&frame, zeroes, sizeof zeroes);
	if (reason == NULL && frame.failed)
		reason = "out of memory";
	if (reason == NULL)
		reason = direct_image(m->direct, &frame, image, cap, create_detail, sizeof create_detail);
	for (size_t i = 0u; i < split_count; i++)
		free(split[i]);
	for (size_t i = 0u; i < skip_count; i++)
		free(skip[i]);
	free(frame.data);
	return reason;
}

/*
 * (mutable direct)'s image: the frame, `hull-dir:<digest>` for the tree's
 * real path and the frame, since what the machine runs is the directory
 * itself, bound in entry by entry (frame_direct). Labelled as a dir's import is, so gc
 * keeps it while the tree is there and an overlay's import of the same
 * tree retires it, and it them. NULL, or why in `detail`.
 */
static const char *direct_image(const char *root, const text *body, char *image, size_t cap,
                                char *detail, size_t detail_cap)
{
	hull_sha256       named;
	uint8_t           digest[32];
	char              tree[65];
	char              hex[65];
	char              where[2u * PATH_MAX + 1u];
	char              path[400];
	char              id[80];
	char             *canonical = realpath(root, NULL);
	const char       *at = canonical != NULL ? canonical : root;
	char             *answer = NULL;
	const char       *found;
	text              head = { 0 };
	int               status = 0;
	int               fd;
	size_t            n = 0u;

	hull_sha256_init(&named);
	hull_sha256_update(&named, (const uint8_t *)at, strlen(at));
	hull_sha256_final(&named, digest);
	hull_sha256_hex(digest, tree);
	hull_sha256_init(&named);
	hull_sha256_update(&named, (const uint8_t *)"hull direct\n", 12u);
	hull_sha256_update(&named, (const uint8_t *)at, strlen(at) + 1u);
	hull_sha256_update(&named, (const uint8_t *)body->data, body->length);
	hull_sha256_final(&named, digest);
	hull_sha256_hex(digest, hex);
	where[0] = '\0';
	for (size_t i = 0u; at[i] != '\0' && 2u * i + 2u < sizeof where; i++)
		snprintf(where + 2u * i, 3u, "%02x", (unsigned char)at[i]);
	free(canonical);
	snprintf(image, cap, "hull-dir:%s", hex);
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
	fd = dial();
	addf(&head, "POST " API "/images/create?fromSrc=-&changes=LABEL%%20hull.tree%%3D%s"
	     "&changes=LABEL%%20hull.path%%3D%s HTTP/1.1\r\n"
	     "Host: docker\r\nConnection: close\r\nContent-Type: application/x-tar\r\n"
	     "Content-Length: %zu\r\n\r\n", tree, where, body->length);
	if (fd < 0 || head.failed || send_all(fd, head.data, head.length) != 0 ||
	    send_all(fd, body->data, body->length) != 0 || read_answer(fd, &status, &answer) != 0) {
		free(head.data);
		if (fd >= 0)
			close(fd);
		free(answer);
		snprintf(detail, detail_cap, "the image of the directory's frame could not be imported");
		return detail;
	}
	free(head.data);
	if (status != 200 || json_string(answer, "error", detail, detail_cap)) {
		if (status != 200)
			daemon_says(detail, detail_cap, "import", status, answer);
		free(answer);
		return detail;
	}
	found = strstr(answer, "\"status\":\"sha256:");
	if (found != NULL)
		for (found += strlen("\"status\":\""); *found != '"' && *found != '\0' && n + 1u < sizeof id;)
			id[n++] = *found++;
	id[n] = '\0';
	free(answer);
	answer = NULL;
	if (n == 0u) {
		snprintf(detail, detail_cap, "the import named no image");
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

/* The volumes of (layers ...), each its guest path relative and its
 * stack, its upper made where missing and held open for the writes it
 * keeps, as the root's is. A reason, or NULL. */
static const char *read_stacks(hull_machine *m, const hull_sexpr_doc *doc,
                               const hull_sexpr_form *machine)
{
	hull_sexpr_form entry;
	hull_sexpr_form item;

	if (!hull_access_find(doc, machine, "volumes", &entry))
		return NULL;
	hull_sexpr_child(doc, &entry, &item);
	for (int more = hull_sexpr_next(doc, &item, &item); more;
	     more = hull_sexpr_next(doc, &item, &item)) {
		hull_sexpr_form layers;
		char           *guest;
		char           *mode;
		const char     *at;
		const char     *why;
		unsigned        k = m->stack_count;

		if (!hull_access_find(doc, &item, "layers", &layers))
			continue;
		if (k == MAX_STACKS)
			return "there are too many volumes of layers";
		guest = hull_access_value(doc, &item, "guest");
		mode  = hull_access_value(doc, &item, "mode");
		m->stacks[k].upper_fd = -1;
		why = hull_access_stack(doc, &item, mode != NULL && strcmp(mode, "ro") == 0,
		                        &m->stacks[k].stack);
		free(mode);
		m->stack_count++;
		for (at = guest != NULL ? guest : ""; *at == '/'; at++)
			;
		if (why == NULL && (guest == NULL || *at == '\0' || strstr(at, "..") != NULL ||
		                    at[strlen(at) - 1u] == '/'))
			why = "a volume of layers is a guest path beneath /, not / itself";
		if (why == NULL && (m->stacks[k].guest = strdup(at)) == NULL)
			why = "out of memory";
		free(guest);
		if (why == NULL && m->stacks[k].stack.upper != NULL &&
		    (((mkdir(m->stacks[k].stack.upper, 0755) != 0 && errno != EEXIST)) ||
		     (m->stacks[k].upper_fd = open(m->stacks[k].stack.upper,
		                                   O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC)) < 0)) {
			snprintf(create_detail, sizeof create_detail, "a volume's upper %s: %s",
			         m->stacks[k].stack.upper, strerror(errno));
			return create_detail;
		}
		if (why != NULL)
			return why;
	}
	return NULL;
}

/* The root's trees, `count` of them, and above them each volume's stack
 * beneath its guest path, as one image; each directory above a guest
 * path given the topmost root tree's metadata for it, or the user's 0755
 * where none has one. A reason, or NULL. */
static const char *import_stacked(const hull_machine *m, char *const *trees, unsigned count,
                                  const hull_tree_owner *owners, size_t owner_count, char *image,
                                  size_t cap)
{
	layer_in   *in = calloc(MAX_LAYERS, sizeof *in);
	unsigned    n = 0u;
	const char *reason;

	if (in == NULL)
		return "out of memory";
	for (unsigned i = 0u; i < count; i++)
		in[n++].tree = trees[i];
	for (unsigned k = 0u; k < m->stack_count; k++) {
		const char   *guest = m->stacks[k].guest;
		const hull_stack *stack = &m->stacks[k].stack;
		hull_tree_dir parents[MAX_PREFIX];
		unsigned      depth = 0u;

		for (size_t i = 0u; guest[i] != '\0'; i++) {
			char at[4200];

			if (guest[i] != '/')
				continue;
			if (depth == MAX_PREFIX) {
				free(in);
				return "a volume's guest path is too deep";
			}
			parents[depth].mode  = 0755u;
			parents[depth].uid   = getuid();
			parents[depth].gid   = getgid();
			parents[depth].mtime = 0u;
			for (unsigned t = count; t-- > 0u;) {
				struct stat there;

				snprintf(at, sizeof at, "%s/%.*s", trees[t], (int)i, guest);
				if (lstat(at, &there) == 0 && S_ISDIR(there.st_mode)) {
					parents[depth].mode  = there.st_mode & 07777u;
					parents[depth].uid   = there.st_uid;
					parents[depth].gid   = there.st_gid;
					parents[depth].mtime = (unsigned long long)there.st_mtime;
					break;
				}
			}
			depth++;
		}
		for (unsigned i = 0u; i < stack->count + (stack->upper != NULL); i++) {
			if (n == MAX_LAYERS) {
				free(in);
				return "there are too many layers, the root's and its volumes' together";
			}
			in[n].tree   = i < stack->count ? stack->layer[i] : stack->upper;
			in[n].prefix = guest;
			in[n].opaque = i == 0u;
			memcpy(in[n].parents, parents, sizeof parents);
			n++;
		}
	}
	reason = import_layers(in, n, owners, owner_count, image, cap, create_detail,
	                       sizeof create_detail);
	free(in);
	return reason;
}

static const char *source_image(hull_machine *m, const hull_sexpr_doc *doc,
                                const hull_sexpr_form *machine, const char *upper, char *image,
                                size_t cap)
{
	char            *source = hull_access_value(doc, machine, "source");
	char            *given = hull_access_value(doc, machine, "owners");
	char            *root;
	char            *data = NULL;
	hull_tree_owner *owners = NULL;
	size_t           count = 0u;
	const char      *reason = NULL;

	char            *trees[MAX_LAYERS] = { NULL };
	unsigned         layers = 0u;
	hull_sexpr_form  entry;
	char            *mutable_mode = hull_access_value(doc, machine, "mutable");
	int              direct = mutable_mode != NULL && strcmp(mutable_mode, "direct") == 0;

	free(mutable_mode);

	if (given != NULL) {
		char *at = hull_access_path(doc, given);

		reason = at == NULL ? "out of memory" : read_owners(at, &data, &owners, &count);
		free(at);
	}
	/* (layers "dir:..." ...): each above the source, the last on top. */
	if (reason == NULL && hull_access_find(doc, machine, "layers", &entry)) {
		hull_sexpr_form item;
		int             more;

		hull_sexpr_child(doc, &entry, &item);
		for (more = hull_sexpr_next(doc, &item, &item); more && reason == NULL;
		     more = hull_sexpr_next(doc, &item, &item)) {
			char *text = hull_access_text(doc, &item);

			if (layers + 2u == MAX_LAYERS)   /* room for an upper above them */
				reason = "there are too many layers";
			else if (text == NULL || strncmp(text, "dir:", 4u) != 0)
				reason = "a layer is a dir: source";
			else if ((trees[1u + layers++] = hull_access_path(doc, text + 4)) == NULL)
				reason = "out of memory";
			free(text);
		}
	}
	/* (upper PATH): the top layer, its whiteouts docker's. */
	if (reason == NULL && upper != NULL &&
	    (trees[1u + layers++] = strdup(upper)) == NULL)
		reason = "out of memory";
	if (reason != NULL) {
		/* said below */
	} else if (source != NULL && is_ref(source)) {
		snprintf(image, cap, "%s", source);
		if (given != NULL)
			reason = "owners are given to a dir's import: an image ref keeps its own";
		else if (layers != 0u)
			reason = "layers and an upper lie above a dir's import: an image ref has its own";
		else if (m->stack_count != 0u)
			reason = "a volume's stack of layers is laid into a dir's import: an image ref "
			         "is the daemon's";
	} else if (source != NULL && strncmp(source, "dir:", 4u) == 0 && source[4] != '\0' &&
	           direct) {
		/* In place: an empty image, the directory bound in (add_direct);
		 * what lies above an overlay has none to lie above. */
		root = hull_access_path(doc, source + 4);
		if (given != NULL)
			reason = "owners are given above an overlay: (owners ...) needs (mutable overlay)";
		else if (layers != 0u)
			reason = "layers and an upper lie under an overlay: they need (mutable overlay)";
		else if (m->stack_count != 0u)
			reason = "a volume's stack is laid into an overlay's import: it needs "
			         "(mutable overlay)";
		else if (hull_access_find(doc, machine, "export", &entry))
			reason = "an export is of an overlay's tree: (export ...) needs (mutable overlay)";
		else if (root == NULL || (m->direct = strdup(root)) == NULL)
			reason = "out of memory";
		else {
			hull_sexpr_form top;
			hull_sexpr_form security;
			char           *secrets;
			int             readonly;

			hull_sexpr_first(doc, &top);
			secrets  = hull_access_value(doc, &top, "secrets");
			readonly = hull_access_find(doc, machine, "security", &security) &&
			           hull_access_true(doc, &security, "readonly_root");
			text binds = { 0 };

			reason = frame_direct(m, secrets != NULL, readonly, &binds, image, cap);
			m->direct_binds        = binds.data;
			m->direct_binds_length = binds.length;
			free(secrets);
		}
		free(root);
	} else if (source != NULL && strncmp(source, "dir:", 4u) == 0 && source[4] != '\0') {
		root = hull_access_path(doc, source + 4);
		trees[0] = root;
		if (root == NULL)
			reason = "out of memory";
		else if (layers == 0u && m->stack_count == 0u)
			reason = import_dir(root, owners, count, image, cap, create_detail,
			                    sizeof create_detail);
		else
			reason = import_stacked(m, trees, 1u + layers, owners, count, image, cap);
	} else {
		reason = "the docker adapter runs an image ref or a dir";
	}
	/* Kept for the export, which takes docker's files over /etc from them. */
	for (unsigned i = 0u; i < MAX_LAYERS; i++) {
		if (reason == NULL && trees[0] != NULL && trees[i] != NULL)
			m->trees[m->tree_count++] = trees[i];
		else
			free(trees[i]);
	}
	free(owners);
	free(data);
	free(given);
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
	char           *export;
	char           *upper;
	char           *upper_at = NULL;
	char            path[160];
	char            image[256];
	int             status = 0;

	if (!hull_access_machine(&doc, plan.data, plan.len, &machine))
		return fail(error, HULL_ERR_UNSUPPORTED, "the plan is not canonical");
	m = calloc(1u, sizeof *m);
	if (m == NULL)
		return fail(error, HULL_ERR_INTERNAL, "out of memory");
	m->core      = core;
	m->attach    = -1;
	m->export_fd = -1;
	m->upper_fd  = -1;
	pthread_mutex_init(&m->lock, NULL);
	pthread_cond_init(&m->done_cond, NULL);
	/* (upper PATH), made where missing as chroot makes it, and held open
	 * for the writes it keeps; by its path the import's top layer. */
	if ((upper = hull_access_value(&doc, &machine, "upper")) != NULL) {
		upper_at = hull_access_path(&doc, upper);
		free(upper);
		if (upper_at == NULL ||
		    (mkdir(upper_at, 0755) != 0 && errno != EEXIST) ||
		    (m->upper_fd = open(upper_at, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC)) < 0) {
			snprintf(create_detail, sizeof create_detail, "the upper %s: %s",
			         upper_at != NULL ? upper_at : "", upper_at != NULL ? strerror(errno) :
			                                                                "out of memory");
			free(upper_at);
			release(m);
			return fail(error, HULL_ERR_RUNTIME, create_detail);
		}
	}
	/* The export's file opened now, as chroot does before its workload,
	 * so a path that cannot be written is said before anything runs; in
	 * place there is no overlay to export, refused before any file is
	 * made (source_image says so). */
	export = in_place(&doc, &machine) ? NULL : hull_access_value(&doc, &machine, "export");
	if (export != NULL) {
		char *at = hull_access_path(&doc, export);

		if (at == NULL ||
		    (m->export_fd = open(at, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC,
		                         0600)) < 0) {
			snprintf(create_detail, sizeof create_detail, "the export's file %s: %s",
			         at != NULL ? at : export, at != NULL ? strerror(errno) : "no path");
			free(at);
			free(export);
			free(upper_at);
			release(m);
			return fail(error, HULL_ERR_RUNTIME, create_detail);
		}
		free(at);
		free(export);
	}
	hostname = hull_access_value(&doc, &machine, "hostname");
	snprintf(m->name, sizeof m->name, "hull-%s-%ld-%u", hostname != NULL ? hostname : "machine",
	         (long)getpid(), machines_made++);
	free(hostname);

	reason = read_stacks(m, &doc, &machine);
	if (reason == NULL)
		reason = source_image(m, &doc, &machine, upper_at, image, sizeof image);
	free(upper_at);
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
	m->core      = core;
	m->attach    = -1;
	m->export_fd = -1;   /* the export's file was the old core's, and went with it */
	m->upper_fd  = -1;   /* and the upper's writes, which that core was to keep */
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
	{ "source.dir.direct",  HULL_SUP_EMULATED,
	  "the directory's entries bound into an image of its frame, each directory docker "
	  "mounts something in bound entry by entry, so a write lands in the directory as it "
	  "is made", NULL },
	{ "source.dir.overlay", HULL_SUP_EMULATED,
	  "a docker import of the tree, kept by its SHA-256; / is root's, as docker makes it",
	  NULL },
	{ "source.ref.overlay", HULL_SUP_NATIVE,   NULL, NULL },

	/* "ports/volumes/env map directly" (5.0). */
	{ "command",            HULL_SUP_NATIVE,   NULL, NULL },
	{ "workdir",            HULL_SUP_NATIVE,   NULL, NULL },
	{ "env",                HULL_SUP_NATIVE,   NULL, NULL },
	{ "network.host",       HULL_SUP_NATIVE,   NULL, NULL },
	{ "network.nat",        HULL_SUP_NATIVE,   "docker's own bridge, as no network key gives", NULL },
	{ "network.bridge",     HULL_SUP_NATIVE,
	  "docker's own bridge, or the docker network (bridge NAME) names", NULL },
	{ "bridge",             HULL_SUP_NATIVE,   "a docker network, by its name", NULL },
	{ "network.none",       HULL_SUP_NATIVE,   NULL, NULL },
	{ "export",             HULL_SUP_NATIVE,
	  "docker export, once the workload ends", NULL },
	{ "layers",             HULL_SUP_EMULATED,
	  "an image of as many layers, each dir's tar one, loaded into the daemon", NULL },
	{ "upper",              HULL_SUP_EMULATED,
	  "the import's top layer, and the container's changes written back into it as it ends, "
	  "the user's", NULL },
	{ "owners",             HULL_SUP_EMULATED,
	  "given in the dir's import, its tar carrying each listed owner", NULL },
	{ "init",               HULL_SUP_REJECTED,
	  "the workload runs as the user, and an image's init wants root as PID 1", NULL },
	{ "segments",           HULL_SUP_REJECTED,
	  "a container's NIC is the daemon's, never on the core's switch", NULL },
	{ "impair",             HULL_SUP_REJECTED,
	  "a link is impaired in the core's switch, which a container is never on", NULL },
	{ "tamper",             HULL_SUP_REJECTED,
	  "a fault on a link in the core's switch, which a container is never on", NULL },
	{ "ports",              HULL_SUP_NATIVE,
	  "bound on the machine's address", NULL },
	{ "volumes",            HULL_SUP_NATIVE,   NULL, NULL },
	{ "volumes.layers",     HULL_SUP_EMULATED,
	  "laid into the dir's import beneath its guest path, hiding what is there; its upper's "
	  "writes given back, and a read-only one docker's ReadonlyPaths", NULL },
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
	{ "devices.usb",            HULL_SUP_NATIVE,
	  "its usbfs node by --device, found when the container is made", NULL },
	{ "devices.serial",         HULL_SUP_NATIVE,    "the host's node by --device", NULL },
	{ "devices.parallel",       HULL_SUP_NATIVE,    "the host's node by --device", NULL },
	{ "devices.node",           HULL_SUP_NATIVE,    "the host's node by --device", NULL },
	{ "devices.pci",            HULL_SUP_NATIVE,
	  "its VFIO group's node and /dev/vfio/vfio by --device", NULL },
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
