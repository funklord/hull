/*
 * push.c -- a stored tree to an OCI registry (section 3.5), by the
 * distribution spec's uploads: each blob asked for with a HEAD, and sent
 * where the registry has it not, a POST opening the upload and a PUT to
 * where it says closing it with the blob's digest; then the image
 * manifest, PUT under its tag. See push.h.
 *
 * The layer is the store's tar of the manifest (store.h), through zlib's
 * gzip, whose header carries no time, so that the same tree is the same
 * bytes and the same digests each time it is pushed.
 */

#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <zlib.h>

#include "push.h"
#include "remote.h"
#include "sexpr.h"

#define OCI_MANIFEST "application/vnd.oci.image.manifest.v1+json"
#define OCI_CONFIG   "application/vnd.oci.image.config.v1+json"
#define OCI_LAYER    "application/vnd.oci.image.layer.v1.tar+gzip"

static int say(char *why, size_t cap, const char *what, const char *detail)
{
	if (detail != NULL)
		snprintf(why, cap, "%s: %s", what, detail);
	else
		snprintf(why, cap, "%s", what);
	return -1;
}

static void hex_digest(hull_sha256 *h, char out[80])
{
	uint8_t raw[32];
	char    hex[65];

	hull_sha256_final(h, raw);
	hull_sha256_hex(raw, hex);
	snprintf(out, 80, "sha256:%s", hex);
}

/* --- JSON, written ------------------------------------------------------- */

typedef struct text {
	char  *data;
	size_t length;
	size_t cap;
	int    failed;
} text;

static void add(text *t, const char *bytes, size_t n)
{
	if (t->failed)
		return;
	if (t->length + n + 1u > t->cap) {
		size_t want = (t->length + n + 1u) * 2u + 256u;
		char  *grown = realloc(t->data, want);

		if (grown == NULL) {
			t->failed = 1;
			return;
		}
		t->data = grown;
		t->cap = want;
	}
	memcpy(t->data + t->length, bytes, n);
	t->length += n;
	t->data[t->length] = '\0';
}

static void add_text(text *t, const char *s)
{
	add(t, s, strlen(s));
}

/* `bytes` as a JSON string: `"` and `\` escaped, a control byte as \u. */
static void add_string(text *t, const uint8_t *bytes, size_t n)
{
	add_text(t, "\"");
	for (size_t i = 0u; i < n; i++) {
		char escaped[8];

		if (bytes[i] == '"' || bytes[i] == '\\') {
			escaped[0] = '\\';
			escaped[1] = (char)bytes[i];
			add(t, escaped, 2u);
		} else if (bytes[i] < 0x20u) {
			snprintf(escaped, sizeof escaped, "\\u%04x", bytes[i]);
			add_text(t, escaped);
		} else {
			add(t, (const char *)bytes + i, 1u);
		}
	}
	add_text(t, "\"");
}

static int record_find(const hull_sexpr_doc *doc, const hull_sexpr_form *record, const char *key,
                       hull_sexpr_form *found)
{
	hull_sexpr_form item;
	hull_sexpr_form name;
	int             more;

	hull_sexpr_child(doc, record, &item);
	for (more = hull_sexpr_next(doc, &item, &item); more; more = hull_sexpr_next(doc, &item, &item))
		if (item.kind == HULL_SEXPR_LIST && hull_sexpr_child(doc, &item, &name) &&
		    hull_sexpr_is(doc, &name, key)) {
			*found = item;
			return 1;
		}
	return 0;
}

/* `"Key":` and the runtime's `(key ...)` after it, its values as `shape`
 * says: 'a' an array of strings, 'o' an object of their names, 's' one
 * string, 'p' an array of its pairs as NAME=value. */
static void add_member(text *t, const hull_sexpr_doc *doc, const hull_sexpr_form *runtime,
                       const char *key, const char *json, char shape, int *first)
{
	hull_sexpr_form list;
	hull_sexpr_form item;
	int             more;
	int             n = 0;

	if (!record_find(doc, runtime, key, &list))
		return;
	add_text(t, *first ? "" : ",");
	*first = 0;
	add_string(t, (const uint8_t *)json, strlen(json));
	add_text(t, shape == 'a' || shape == 'p' ? ":[" : shape == 'o' ? ":{" : ":");
	hull_sexpr_child(doc, &list, &item);
	for (more = hull_sexpr_next(doc, &item, &item); more; more = hull_sexpr_next(doc, &item, &item)) {
		if (n++ > 0)
			add_text(t, ",");
		if (shape == 'p') {
			hull_sexpr_form name;
			hull_sexpr_form value;
			text            pair = { 0 };

			hull_sexpr_child(doc, &item, &name);
			hull_sexpr_next(doc, &name, &value);
			add(&pair, (const char *)doc->data + name.content, name.content_length);
			add_text(&pair, "=");
			add(&pair, (const char *)doc->data + value.content, value.content_length);
			add_string(t, (const uint8_t *)(pair.data != NULL ? pair.data : ""), pair.length);
			t->failed |= pair.failed;
			free(pair.data);
		} else {
			add_string(t, doc->data + item.content, item.content_length);
			if (shape == 'o')
				add_text(t, ":{}");
		}
	}
	add_text(t, shape == 'a' || shape == 'p' ? "]" : shape == 'o' ? "}" : "");
}

/* OCI's config: the arch, A.4's runtime in OCI's names, its keys in order,
 * and the one layer's diff_id. */
static int config_json(text *t, const char *arch, const uint8_t *runtime, size_t runtime_length,
                       const char *diff_id)
{
	hull_sexpr_doc  doc;
	hull_sexpr_form top;
	int             first = 1;

	add_text(t, "{\"architecture\":");
	add_string(t, (const uint8_t *)arch, strlen(arch));
	add_text(t, ",\"config\":{");
	if (runtime != NULL && hull_sexpr_open(&doc, runtime, runtime_length, HULL_SEXPR_CANONICAL,
	                                       NULL) == HULL_SEXPR_OK && hull_sexpr_first(&doc, &top)) {
		add_member(t, &doc, &top, "cmd", "Cmd", 'a', &first);
		add_member(t, &doc, &top, "entrypoint", "Entrypoint", 'a', &first);
		add_member(t, &doc, &top, "env", "Env", 'p', &first);
		add_member(t, &doc, &top, "ports", "ExposedPorts", 'o', &first);
		add_member(t, &doc, &top, "stop_signal", "StopSignal", 's', &first);
		add_member(t, &doc, &top, "user", "User", 's', &first);
		add_member(t, &doc, &top, "volumes", "Volumes", 'o', &first);
		add_member(t, &doc, &top, "workdir", "WorkingDir", 's', &first);
	}
	add_text(t, "},\"os\":\"linux\",\"rootfs\":{\"diff_ids\":[");
	add_string(t, (const uint8_t *)diff_id, strlen(diff_id));
	add_text(t, "],\"type\":\"layers\"}}");
	return t->failed ? -1 : 0;
}

/* --- the layer ----------------------------------------------------------- */

typedef struct layer {
	z_stream    z;
	int         fd;
	hull_sha256 tar;       /* the diff_id's */
	hull_sha256 blob;      /* the layer's own */
	long long   size;
	uint8_t     out[65536];
	int         failed;
} layer;

static int drain(layer *l, int flush)
{
	do {
		int status;

		l->z.next_out = l->out;
		l->z.avail_out = sizeof l->out;
		status = deflate(&l->z, flush);
		if (status == Z_STREAM_ERROR)
			return -1;
		{
			size_t   n = sizeof l->out - l->z.avail_out;
			uint8_t *at = l->out;

			hull_sha256_update(&l->blob, l->out, n);
			l->size += (long long)n;
			while (n > 0u) {
				ssize_t wrote = write(l->fd, at, n);

				if (wrote < 0 && errno == EINTR)
					continue;
				if (wrote <= 0)
					return -1;
				at += wrote;
				n -= (size_t)wrote;
			}
		}
		if (flush == Z_FINISH && status == Z_STREAM_END)
			return 0;
	} while (l->z.avail_out == 0u || flush == Z_FINISH);
	return 0;
}

static int into_layer(void *context, const uint8_t *data, size_t n)
{
	layer *l = context;

	hull_sha256_update(&l->tar, data, n);
	l->z.next_in = (Bytef *)data;
	l->z.avail_in = (uInt)n;
	if (drain(l, Z_NO_FLUSH) != 0) {
		l->failed = 1;
		return -1;
	}
	return 0;
}

/* --- uploading ----------------------------------------------------------- */

/* `location` made a URL on the registry's host where it is a path. */
static void resolve(const hull_remote *r, const char *location, char *out, size_t cap)
{
	if (location[0] == '/')
		snprintf(out, cap, "https://%s%s", r->img.host, location);
	else
		snprintf(out, cap, "%s", location);
}

static int expect(const hull_https_response *got, int status, const char *what, char *why,
                  size_t cap)
{
	char text[96];

	if (got->status == status)
		return 0;
	snprintf(text, sizeof text, "the registry answered %d where %d was wanted", got->status, status);
	return say(why, cap, what, text);
}

/* A blob into the repository, unless the registry has it: 1 sent, 0 there
 * already, or -1 with `why`. */
static int upload_blob(hull_remote *r, const char *digest, const hull_https_upload *body,
                       char *why, size_t cap)
{
	static const char *const octets[] = { "Content-Type: application/octet-stream", NULL };
	uint8_t                  small[4096];
	hull_remote_sink         into = { .data = small, .cap = sizeof small, .fd = -1 };
	hull_https_response      got;
	char                     url[8192];
	char                     put[8400];

	snprintf(url, sizeof url, "https://%s/v2/%s/blobs/%s", r->img.host, r->img.repository, digest);
	if (hull_remote_request(r, "HEAD", url, NULL, NULL, &into, &got, why, cap) != 0)
		return -1;
	if (got.status == 200)
		return 0;
	if (got.status != 404)
		return expect(&got, 404, digest, why, cap);
	snprintf(url, sizeof url, "https://%s/v2/%s/blobs/uploads/", r->img.host, r->img.repository);
	if (hull_remote_request(r, "POST", url, NULL, NULL, &into, &got, why, cap) != 0 ||
	    expect(&got, 202, "the upload", why, cap) != 0)
		return -1;
	if (got.location[0] == '\0')
		return say(why, cap, "the upload", "the registry gave it no Location");
	resolve(r, got.location, url, sizeof url);
	snprintf(put, sizeof put, "%s%sdigest=sha256%%3A%s", url, strchr(url, '?') != NULL ? "&" : "?",
	         digest + 7);
	if (hull_remote_request(r, "PUT", put, octets, body, &into, &got, why, cap) != 0 ||
	    expect(&got, 201, digest, why, cap) != 0)
		return -1;
	return 1;
}

int hull_push(hull_store *store, const char *name, const char *image, char digest[80], char *why,
              size_t cap)
{
	static const char *const manifest_type[] = { "Content-Type: " OCI_MANIFEST, NULL };
	hull_remote              r = { 0 };
	layer                   *l = calloc(1u, sizeof *l);
	text                     config = { 0 };
	text                     manifest = { 0 };
	uint8_t                 *runtime = NULL;
	size_t                   runtime_length = 0u;
	char                     arch[64];
	char                     diff_id[80];
	char                     layer_digest[80];
	char                     config_digest[80];
	char                     tmp[4400];
	hull_sha256              h;
	hull_https_upload        layer_body = { .fd = -1 };
	hull_https_upload        config_body = { .fd = -1 };
	hull_https_upload        manifest_body = { .fd = -1 };
	int                      sent;
	int                      result = -1;

	if (l == NULL)
		return say(why, cap, "out of memory", NULL);
	l->fd = -1;
	if (hull_remote_parse(image, &r.img, why, cap) != 0)
		goto done;
	if (r.img.by_digest) {
		say(why, cap, image, "a push names a tag, which the image is put under");
		goto done;
	}
	if (hull_store_runtime(store, name, &runtime, &runtime_length, why, cap) != 0)
		goto done;
	/* No name in tmp/, so that gc, which empties it, has nothing to take. */
	snprintf(tmp, sizeof tmp, "%s/tmp", store->root);
	if ((l->fd = open(tmp, O_TMPFILE | O_RDWR | O_CLOEXEC, 0600)) < 0) {
		say(why, cap, tmp, strerror(errno));
		goto done;
	}
	if (deflateInit2(&l->z, Z_DEFAULT_COMPRESSION, Z_DEFLATED, 15 + 16, 8, Z_DEFAULT_STRATEGY) !=
	    Z_OK) {
		say(why, cap, "the layer", "zlib could not begin it");
		goto done;
	}
	hull_sha256_init(&l->tar);
	hull_sha256_init(&l->blob);
	if (hull_store_layer(store, name, into_layer, l, arch, why, cap) != 0) {
		if (l->failed)
			say(why, cap, "the layer", "it could not be written");
		deflateEnd(&l->z);
		goto done;
	}
	l->z.next_in = NULL;
	l->z.avail_in = 0u;
	if (drain(l, Z_FINISH) != 0) {
		say(why, cap, "the layer", "it could not be written");
		deflateEnd(&l->z);
		goto done;
	}
	deflateEnd(&l->z);
	hex_digest(&l->tar, diff_id);
	hex_digest(&l->blob, layer_digest);
	if (config_json(&config, arch[0] != '\0' ? arch : "amd64", runtime, runtime_length,
	                diff_id) != 0) {
		say(why, cap, "out of memory", NULL);
		goto done;
	}
	hull_sha256_init(&h);
	hull_sha256_update(&h, config.data, config.length);
	hex_digest(&h, config_digest);
	{
		char text_size[48];

		add_text(&manifest, "{\"schemaVersion\":2,\"mediaType\":\"" OCI_MANIFEST "\","
		                    "\"config\":{\"mediaType\":\"" OCI_CONFIG "\",\"digest\":\"");
		add_text(&manifest, config_digest);
		snprintf(text_size, sizeof text_size, "\",\"size\":%zu},", config.length);
		add_text(&manifest, text_size);
		add_text(&manifest, "\"layers\":[{\"mediaType\":\"" OCI_LAYER "\",\"digest\":\"");
		add_text(&manifest, layer_digest);
		snprintf(text_size, sizeof text_size, "\",\"size\":%lld}]}", l->size);
		add_text(&manifest, text_size);
	}
	if (manifest.failed) {
		say(why, cap, "out of memory", NULL);
		goto done;
	}
	hull_sha256_init(&h);
	hull_sha256_update(&h, manifest.data, manifest.length);
	hex_digest(&h, digest);
	fprintf(stderr, "hull: push: layer %s, %lld bytes\n", layer_digest, l->size);
	layer_body.fd = l->fd;
	layer_body.length = l->size;
	config_body.data = (const uint8_t *)config.data;
	config_body.length = (long long)config.length;
	if ((sent = upload_blob(&r, layer_digest, &layer_body, why, cap)) < 0)
		goto done;
	if (sent == 0)
		fprintf(stderr, "hull: push: the registry has the layer already\n");
	if (upload_blob(&r, config_digest, &config_body, why, cap) < 0)
		goto done;
	{
		uint8_t             small[4096];
		hull_remote_sink    into = { .data = small, .cap = sizeof small, .fd = -1 };
		hull_https_response got;
		char                url[8192];

		snprintf(url, sizeof url, "https://%s/v2/%s/manifests/%s", r.img.host, r.img.repository,
		         r.img.reference);
		manifest_body.data = (const uint8_t *)manifest.data;
		manifest_body.length = (long long)manifest.length;
		if (hull_remote_request(&r, "PUT", url, manifest_type, &manifest_body, &into, &got, why,
		                        cap) != 0 ||
		    expect(&got, 201, "the manifest", why, cap) != 0)
			goto done;
		if (got.digest[0] != '\0' && strcmp(got.digest, digest) != 0) {
			say(why, cap, "the manifest", "the registry names it by another digest");
			goto done;
		}
	}
	result = 0;
done:
	if (l->fd >= 0)
		close(l->fd);
	free(l);
	free(runtime);
	free(config.data);
	free(manifest.data);
	return result;
}
