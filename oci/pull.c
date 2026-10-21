/*
 * pull.c -- an image from an OCI registry into the store (section 3.5),
 * by the distribution spec's GETs: the manifest, by tag or digest; an
 * index's manifest for linux on the arch asked for; the config; and each
 * layer, applied in order into a tree under the store's tmp/, which is
 * then committed. See pull.h.
 *
 * Every body is hashed as it arrives and refused unless it is the digest
 * that named it. A registry that wants a token says so with a 401 and a
 * Bearer challenge, and an anonymous one is asked for at its realm; one
 * that wants credentials is refused, since hull reads none yet. A
 * redirect is followed five deep, and the token goes only to the
 * registry's own host, not to wherever its blobs are served from.
 */

#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include "https.h"
#include "json.h"
#include "pull.h"
#include "sha256.h"
#include "store_private.h"

#define MAX_DOCUMENT  (4u << 20)
#define MAX_REDIRECTS 5
#define ACCEPT_MANIFESTS                                                     \
	"Accept: application/vnd.oci.image.index.v1+json, "                  \
	"application/vnd.docker.distribution.manifest.list.v2+json, "         \
	"application/vnd.oci.image.manifest.v1+json, "                        \
	"application/vnd.docker.distribution.manifest.v2+json"

static int say(char *why, size_t cap, const char *what, const char *detail)
{
	if (detail != NULL)
		snprintf(why, cap, "%s: %s", what, detail);
	else
		snprintf(why, cap, "%s", what);
	return -1;
}

/* --- the image's name --------------------------------------------------- */

typedef struct image {
	char host[256];        /* what the API is reached at */
	char repository[512];
	char reference[128];   /* a tag, or sha256:HEX */
	int  by_digest;
} image;

static int is_digest(const char *text)
{
	return strncmp(text, "sha256:", 7u) == 0 && strlen(text) == 71u &&
	       strspn(text + 7, "0123456789abcdef") == 64u;
}

static int parse_image(const char *text, image *out, char *why, size_t cap)
{
	char        name[1024];
	char       *at;
	char       *slash;
	char       *colon;
	const char *repository;

	memset(out, 0, sizeof *out);
	if (snprintf(name, sizeof name, "%s", text) >= (int)sizeof name)
		return say(why, cap, text, "too long to be an image's name");
	if ((at = strchr(name, '@')) != NULL) {
		*at = '\0';
		if (!is_digest(at + 1))
			return say(why, cap, text, "its digest is not sha256: and 64 lowercase hex digits");
		snprintf(out->reference, sizeof out->reference, "%s", at + 1);
		out->by_digest = 1;
	}
	slash = strrchr(name, '/');
	colon = strrchr(name, ':');
	if (colon != NULL && (slash == NULL || colon > slash)) {
		*colon = '\0';
		if (!out->by_digest)
			snprintf(out->reference, sizeof out->reference, "%s", colon + 1);
		if (colon[1] == '\0' || strlen(colon + 1) > 128u ||
		    strspn(colon + 1, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_.-") !=
		        strlen(colon + 1))
			return say(why, cap, text, "its tag is not one");
	}
	if (out->reference[0] == '\0')
		snprintf(out->reference, sizeof out->reference, "latest");
	/* A first part with a dot or a colon, or localhost, is a registry. */
	slash = strchr(name, '/');
	if (slash != NULL && (memchr(name, '.', (size_t)(slash - name)) != NULL ||
	                      memchr(name, ':', (size_t)(slash - name)) != NULL ||
	                      strncmp(name, "localhost/", 10u) == 0)) {
		*slash = '\0';
		if (snprintf(out->host, sizeof out->host, "%s", name) >= (int)sizeof out->host)
			return say(why, cap, text, "its registry's name is too long");
		repository = slash + 1;
	} else {
		snprintf(out->host, sizeof out->host, "docker.io");
		repository = name;
	}
	if (strcmp(out->host, "docker.io") == 0 || strcmp(out->host, "index.docker.io") == 0) {
		snprintf(out->host, sizeof out->host, "registry-1.docker.io");
		if (snprintf(out->repository, sizeof out->repository, "%s%s",
		             strchr(repository, '/') == NULL ? "library/" : "", repository) >=
		    (int)sizeof out->repository)
			return say(why, cap, text, "its repository's name is too long");
	} else if (snprintf(out->repository, sizeof out->repository, "%s", repository) >=
	           (int)sizeof out->repository) {
		return say(why, cap, text, "its repository's name is too long");
	}
	if (out->repository[0] == '\0' ||
	    strspn(out->repository, "abcdefghijklmnopqrstuvwxyz0123456789._-/") !=
	        strlen(out->repository) ||
	    strstr(out->repository, "//") != NULL || out->repository[0] == '/' ||
	    out->repository[strlen(out->repository) - 1u] == '/')
		return say(why, cap, text, "its repository is not one");
	return 0;
}

/* --- fetching ----------------------------------------------------------- */

/* Where a body goes: into memory, at most `cap` of it, or into `fd`; hashed
 * either way. */
typedef struct sink {
	uint8_t    *data;
	size_t      length;
	size_t      cap;
	int         fd;
	long long   written;
	hull_sha256 hash;
} sink;

static int take(void *context, const uint8_t *data, size_t n, char *why, size_t cap)
{
	sink *s = context;

	hull_sha256_update(&s->hash, data, n);
	if (s->fd >= 0) {
		while (n > 0u) {
			ssize_t wrote = write(s->fd, data, n);

			if (wrote < 0 && errno == EINTR)
				continue;
			if (wrote <= 0)
				return say(why, cap, "a layer could not be written", strerror(errno));
			data += wrote;
			n -= (size_t)wrote;
			s->written += wrote;
		}
		return 0;
	}
	if (s->length + n > s->cap)
		return say(why, cap, "a registry's document is larger than 4 MiB", NULL);
	memcpy(s->data + s->length, data, n);
	s->length += n;
	return 0;
}

typedef struct session {
	image img;
	char  token[8192];
	int   authenticating;  /* a token's own GET asks for none */
} session;

/* Bearer realm="...",service="...",scope="..." into its parts. */
static int challenge(const char *text, char *realm, char *service, char *scope, size_t cap)
{
	const char *at = text;

	realm[0] = service[0] = scope[0] = '\0';
	if (strncasecmp(text, "Bearer ", 7u) != 0)
		return -1;
	at += 7;
	while (*at != '\0') {
		const char *equals = strchr(at, '=');
		const char *end;
		char       *into = NULL;
		size_t      n;

		while (*at == ' ' || *at == ',')
			at++;
		if ((equals = strchr(at, '=')) == NULL || equals[1] != '"')
			break;
		if ((end = strchr(equals + 2, '"')) == NULL)
			return -1;
		if (equals - at == 5 && strncasecmp(at, "realm", 5u) == 0)
			into = realm;
		else if (equals - at == 7 && strncasecmp(at, "service", 7u) == 0)
			into = service;
		else if (equals - at == 5 && strncasecmp(at, "scope", 5u) == 0)
			into = scope;
		n = (size_t)(end - equals - 2);
		if (into != NULL) {
			if (n >= cap)
				return -1;
			memcpy(into, equals + 2, n);
			into[n] = '\0';
		}
		at = end + 1;
	}
	return realm[0] != '\0' ? 0 : -1;
}

static void url_encode(const char *text, char *out, size_t cap)
{
	size_t n = 0u;

	for (const unsigned char *p = (const unsigned char *)text; *p != '\0' && n + 4u < cap; p++) {
		if ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') ||
		    strchr("-_.~", *p) != NULL)
			out[n++] = (char)*p;
		else
			n += (size_t)snprintf(out + n, cap - n, "%%%02X", *p);
	}
	out[n] = '\0';
}

static int fetch(session *s, const char *url, const char *accept, sink *into, char *why, size_t cap);

/* An anonymous token from the challenge's realm, for the pull's scope. */
static int authenticate(session *s, const char *text, char *why, size_t cap)
{
	char            realm[2048];
	char            service[512];
	char            scope[1024];
	char            encoded_service[1600];
	char            encoded_scope[3200];
	char            url[8192];
	uint8_t        *data = malloc(MAX_DOCUMENT);
	sink            body = { .data = data, .cap = MAX_DOCUMENT, .fd = -1 };
	hull_json       doc;
	hull_json_value root;
	const char     *bad;
	int             result = -1;

	if (data == NULL)
		return say(why, cap, "out of memory", NULL);
	if (challenge(text, realm, service, scope, sizeof realm) != 0) {
		free(data);
		return say(why, cap, s->img.host, strncasecmp(text, "Basic", 5u) == 0 ?
		           "it asks for credentials, which hull does not yet read" :
		           "it asks for authentication hull cannot give");
	}
	if (scope[0] == '\0')
		snprintf(scope, sizeof scope, "repository:%s:pull", s->img.repository);
	url_encode(service, encoded_service, sizeof encoded_service);
	url_encode(scope, encoded_scope, sizeof encoded_scope);
	snprintf(url, sizeof url, "%s%sservice=%s&scope=%s", realm, strchr(realm, '?') != NULL ? "&" : "?",
	         encoded_service, encoded_scope);
	hull_sha256_init(&body.hash);
	s->token[0] = '\0';
	s->authenticating = 1;
	if (fetch(s, url, NULL, &body, why, cap) == 0) {
		if (hull_json_open(&doc, (const char *)data, body.length, &root, &bad) != 0 ||
		    (hull_json_member_string(&doc, &root, "token", s->token, sizeof s->token) <= 0 &&
		     hull_json_member_string(&doc, &root, "access_token", s->token, sizeof s->token) <= 0))
			say(why, cap, realm, "its answer holds no token");
		else
			result = 0;
	}
	s->authenticating = 0;
	free(data);
	return result;
}

/* GET `url` into `into`, as a registry answers it: a 401 answered with a
 * token once, a redirect followed, the token sent only to the registry. */
static int fetch(session *s, const char *url, const char *accept, sink *into, char *why, size_t cap)
{
	char                here[8192];
	int                 tried_token = 0;

	snprintf(here, sizeof here, "%s", url);
	for (int hops = 0; hops <= MAX_REDIRECTS; hops++) {
		hull_https_response response;
		char                host[300];
		char                authorization[8300];
		const char         *headers[3];
		unsigned            n = 0u;

		if (hull_https_host(here, host, sizeof host) != 0)
			return say(why, cap, here, "not an https:// URL; hull reaches registries by TLS only");
		if (accept != NULL)
			headers[n++] = accept;
		if (s->token[0] != '\0' && strcmp(host, s->img.host) == 0) {
			snprintf(authorization, sizeof authorization, "Authorization: Bearer %s", s->token);
			headers[n++] = authorization;
		}
		headers[n] = NULL;
		into->length = 0u;
		hull_sha256_init(&into->hash);
		if (into->fd >= 0 && (ftruncate(into->fd, 0) != 0 || lseek(into->fd, 0, SEEK_SET) != 0))
			return say(why, cap, "a layer could not be written", strerror(errno));
		into->written = 0;
		if (hull_https_get(here, headers, &response, take, into, why, cap) != 0)
			return -1;
		if (response.status == 401 && !tried_token && !s->authenticating &&
		    response.authenticate[0] != '\0') {
			tried_token = 1;
			if (authenticate(s, response.authenticate, why, cap) != 0)
				return -1;
			hops--;
			continue;
		}
		if (response.status >= 300 && response.status < 400 && response.location[0] != '\0') {
			char next[8192];

			if (response.location[0] == '/')
				snprintf(next, sizeof next, "https://%s%s", host, response.location);
			else
				snprintf(next, sizeof next, "%s", response.location);
			snprintf(here, sizeof here, "%s", next);
			continue;
		}
		if (response.status != 200) {
			char status[64];

			snprintf(status, sizeof status, "the registry answered %d", response.status);
			return say(why, cap, here, response.status == 404 ? "not found there" : status);
		}
		return 0;
	}
	return say(why, cap, url, "redirected more than five times");
}

/* A body's digest, against the one that named it. */
static int verify(sink *s, const char *digest, const char *what, char *why, size_t cap)
{
	uint8_t raw[32];
	char    hex[65];

	hull_sha256_final(&s->hash, raw);
	hull_sha256_hex(raw, hex);
	if (!is_digest(digest) || strcmp(digest + 7, hex) != 0)
		return say(why, cap, what, "its content is not the digest that names it");
	return 0;
}

/* A document by tag or digest: a manifest or an index into `doc`, its
 * digest into `digest`. */
static int fetch_manifest(session *s, const char *reference, sink *body, hull_json *doc,
                          hull_json_value *root, char digest[80], char *why, size_t cap)
{
	char        url[2048];
	const char *bad;
	uint8_t     raw[32];
	char        hex[65];

	snprintf(url, sizeof url, "https://%s/v2/%s/manifests/%s", s->img.host, s->img.repository,
	         reference);
	if (fetch(s, url, ACCEPT_MANIFESTS, body, why, cap) != 0)
		return -1;
	hull_sha256_final(&body->hash, raw);
	hull_sha256_hex(raw, hex);
	snprintf(digest, 80, "sha256:%s", hex);
	/* By digest, the body must be it; by tag, it is what the registry
	 * says it sent, where it says. */
	if (is_digest(reference) && strcmp(reference, digest) != 0)
		return say(why, cap, reference, "the manifest is not the digest that names it");
	if (hull_json_open(doc, (const char *)body->data, body->length, root, &bad) != 0)
		return say(why, cap, reference, bad);
	return 0;
}

/* The index's manifest for linux on `arch`. */
static int choose(const hull_json *doc, const hull_json_value *root, const char *arch,
                  char digest[80], char *why, size_t cap)
{
	hull_json_value list;
	hull_json_value item;
	char            seen[512] = "";

	if (!hull_json_member(doc, root, "manifests", &list))
		return say(why, cap, "the index", "it lists no manifests");
	for (int more = hull_json_element(doc, &list, 1, &item); more;
	     more = hull_json_element(doc, &list, 0, &item)) {
		hull_json_value platform;
		char            os[64];
		char            architecture[64];

		if (!hull_json_member(doc, &item, "platform", &platform) ||
		    hull_json_member_string(doc, &platform, "os", os, sizeof os) < 0 ||
		    hull_json_member_string(doc, &platform, "architecture", architecture,
		                            sizeof architecture) < 0)
			continue;
		if (strcmp(os, "linux") == 0 && strcmp(architecture, arch) == 0)
			return hull_json_member_string(doc, &item, "digest", digest, 80) > 0 &&
			       is_digest(digest) ? 0 : say(why, cap, "the index", "its digest is not one");
		if (strlen(seen) + strlen(os) + strlen(architecture) + 4u < sizeof seen)
			snprintf(seen + strlen(seen), sizeof seen - strlen(seen), "%s%s/%s",
			         seen[0] != '\0' ? ", " : "", os, architecture);
	}
	return say(why, cap, arch, seen[0] != '\0' ? seen : "the index has no platform for it");
}

/* --- A.4 and A.5 --------------------------------------------------------- */

/* A canonical form, built up; `failed` once memory ran out. */
typedef struct canon {
	uint8_t *data;
	size_t   length;
	size_t   cap;
	int      failed;
} canon;

static void put_bytes(canon *c, const void *bytes, size_t n)
{
	if (c->failed)
		return;
	if (c->length + n > c->cap) {
		size_t   want = (c->length + n) * 2u + 256u;
		uint8_t *grown = realloc(c->data, want);

		if (grown == NULL) {
			c->failed = 1;
			return;
		}
		c->data = grown;
		c->cap = want;
	}
	memcpy(c->data + c->length, bytes, n);
	c->length += n;
}

static void put_raw(canon *c, const char *text)
{
	put_bytes(c, text, strlen(text));
}

static void put_atom(canon *c, const char *bytes, size_t n)
{
	char length[24];

	snprintf(length, sizeof length, "%zu:", n);
	put_raw(c, length);
	put_bytes(c, bytes, n);
}

static void put_text_atom(canon *c, const char *text)
{
	put_atom(c, text, strlen(text));
}

typedef struct named {
	char *name;
	char *value;
} named;

static int name_order(const void *a, const void *b)
{
	return strcmp(((const named *)a)->name, ((const named *)b)->name);
}

/* An array of strings, each an atom, as `(KEY ...)`; nothing for one that
 * is absent, null or empty. */
static int put_words(canon *c, const hull_json *doc, const hull_json_value *object,
                     const char *key, const char *open)
{
	hull_json_value list;
	hull_json_value item;
	char           *word = malloc(1u << 16);
	int             any = 0;

	if (word == NULL)
		return -1;
	if (hull_json_member(doc, object, key, &list) && list.kind == '[') {
		for (int more = hull_json_element(doc, &list, 1, &item); more;
		     more = hull_json_element(doc, &list, 0, &item)) {
			long n = hull_json_string(doc, &item, word, 1u << 16);

			if (n < 0) {
				free(word);
				return -1;
			}
			if (!any)
				put_raw(c, open);
			any = 1;
			put_atom(c, word, (size_t)n);
		}
	}
	if (any)
		put_raw(c, ")");
	free(word);
	return 0;
}

/* An object's names, sorted, each an atom, as `(KEY ...)`: OCI's
 * ExposedPorts and Volumes. */
static int put_names(canon *c, const hull_json *doc, const hull_json_value *object,
                     const char *key, const char *open)
{
	hull_json_value map;
	hull_json_value name;
	hull_json_value value;
	named          *items = NULL;
	size_t          count = 0u;
	int             result = 0;

	if (!hull_json_member(doc, object, key, &map) || map.kind != '{')
		return 0;
	for (int more = hull_json_next_member(doc, &map, 1, &name, &value); more;
	     more = hull_json_next_member(doc, &map, 0, &name, &value)) {
		char  text[4096];
		named *grown;

		if (hull_json_string(doc, &name, text, sizeof text) < 0 ||
		    (grown = realloc(items, (count + 1u) * sizeof *grown)) == NULL ||
		    (grown[count].name = strdup(text)) == NULL) {
			result = -1;
			break;
		}
		items = grown;
		items[count++].value = NULL;
	}
	if (result == 0 && count > 0u) {
		qsort(items, count, sizeof items[0], name_order);
		put_raw(c, open);
		for (size_t i = 0u; i < count; i++)
			if (i == 0u || strcmp(items[i].name, items[i - 1u].name) != 0)
				put_text_atom(c, items[i].name);
		put_raw(c, ")");
	}
	for (size_t i = 0u; i < count; i++)
		free(items[i].name);
	free(items);
	return result;
}

/* OCI's Env, `NAME=value` each, as `(3:env (NAME value) ...)`, sorted by
 * name, a name given twice its last value, as docker takes it. */
static int put_env(canon *c, const hull_json *doc, const hull_json_value *object)
{
	hull_json_value list;
	hull_json_value item;
	named          *items = NULL;
	size_t          count = 0u;
	int             result = 0;
	char           *text = malloc(1u << 16);

	if (text == NULL)
		return -1;
	if (hull_json_member(doc, object, "Env", &list) && list.kind == '[') {
		for (int more = hull_json_element(doc, &list, 1, &item); more;
		     more = hull_json_element(doc, &list, 0, &item)) {
			char  *equals;
			named *grown;

			if (hull_json_string(doc, &item, text, 1u << 16) < 0) {
				result = -1;
				break;
			}
			if ((equals = strchr(text, '=')) == NULL || equals == text)
				continue;
			*equals = '\0';
			for (size_t i = 0u; i < count; i++)
				if (strcmp(items[i].name, text) == 0) {
					free(items[i].name);
					free(items[i].value);
					items[i] = items[--count];
					break;
				}
			if ((grown = realloc(items, (count + 1u) * sizeof *grown)) == NULL) {
				result = -1;
				break;
			}
			items = grown;
			items[count].name = strdup(text);
			items[count].value = strdup(equals + 1);
			if (items[count].name == NULL || items[count].value == NULL) {
				free(items[count].name);
				free(items[count].value);
				result = -1;
				break;
			}
			count++;
		}
	}
	if (result == 0 && count > 0u) {
		qsort(items, count, sizeof items[0], name_order);
		put_raw(c, "(3:env");
		for (size_t i = 0u; i < count; i++) {
			put_raw(c, "(");
			put_text_atom(c, items[i].name);
			put_text_atom(c, items[i].value);
			put_raw(c, ")");
		}
		put_raw(c, ")");
	}
	for (size_t i = 0u; i < count; i++) {
		free(items[i].name);
		free(items[i].value);
	}
	free(items);
	free(text);
	return result;
}

/* A.4's runtime from the config's `config`, its keys in canonical order;
 * the workdir `/` where the image names none, the schema requiring one. */
static int runtime_of(const hull_json *doc, const hull_json_value *root, canon *c)
{
	hull_json_value config = { 0 };
	char            text[4096];
	int             has = hull_json_member(doc, root, "config", &config) && config.kind == '{';

	put_raw(c, "(7:runtime");
	if (has) {
		if (put_words(c, doc, &config, "Cmd", "(3:cmd") != 0 ||
		    put_words(c, doc, &config, "Entrypoint", "(10:entrypoint") != 0 ||
		    put_env(c, doc, &config) != 0 ||
		    put_names(c, doc, &config, "ExposedPorts", "(5:ports") != 0)
			return -1;
		if (hull_json_member_string(doc, &config, "StopSignal", text, sizeof text) > 0) {
			put_raw(c, "(11:stop_signal");
			put_text_atom(c, text);
			put_raw(c, ")");
		}
		if (hull_json_member_string(doc, &config, "User", text, sizeof text) > 0) {
			put_raw(c, "(4:user");
			put_text_atom(c, text);
			put_raw(c, ")");
		}
		if (put_names(c, doc, &config, "Volumes", "(7:volumes") != 0)
			return -1;
	}
	put_raw(c, "(7:workdir");
	if (has && hull_json_member_string(doc, &config, "WorkingDir", text, sizeof text) > 0)
		put_text_atom(c, text);
	else
		put_text_atom(c, "/");
	put_raw(c, "))");
	return c->failed ? -1 : 0;
}

/* --- the pull ----------------------------------------------------------- */

int hull_pull(hull_store *store, const char *name, const char *ref, const char *arch,
              const char *builder, hull_commit *out, char *why, size_t cap)
{
	canon             runtime = { 0 };
	canon             digests = { 0 };   /* A.5's, in the order fetched */
	canon             provenance = { 0 };
	hull_commit_extra extra;
	session         s = { 0 };
	uint8_t        *data = malloc(MAX_DOCUMENT);
	sink            body = { .data = data, .cap = MAX_DOCUMENT, .fd = -1 };
	hull_json       doc;
	hull_json_value root;
	hull_json_value layers;
	hull_json_value layer;
	hull_json_value config;
	char            digest[80];
	char            config_digest[80];
	char            media[256] = "";
	char            scratch[4400];
	char            tree[4500];
	char            file[4500];
	char            image_arch[64] = "";
	uint64_t        skipped = 0u;
	hull_unpack    *u = NULL;
	int             count = 0;
	int             index = 0;
	int             result = -1;
	int             layer_fd = -1;

	scratch[0] = '\0';
	if (data == NULL)
		return say(why, cap, "out of memory", NULL);
	if (parse_image(name, &s.img, why, cap) != 0)
		goto done;
	if (fetch_manifest(&s, s.img.reference, &body, &doc, &root, digest, why, cap) != 0)
		goto done;
	hull_json_member_string(&doc, &root, "mediaType", media, sizeof media);
	if (strstr(media, "index") != NULL || strstr(media, "manifest.list") != NULL ||
	    (media[0] == '\0' && hull_json_member(&doc, &root, "manifests", &layers))) {
		char chosen[80];

		put_text_atom(&digests, digest);
		if (choose(&doc, &root, arch, chosen, why, cap) != 0)
			goto done;
		body.length = 0u;
		if (fetch_manifest(&s, chosen, &body, &doc, &root, digest, why, cap) != 0)
			goto done;
		media[0] = '\0';
		hull_json_member_string(&doc, &root, "mediaType", media, sizeof media);
	}
	if (media[0] != '\0' && strcmp(media, "application/vnd.oci.image.manifest.v1+json") != 0 &&
	    strcmp(media, "application/vnd.docker.distribution.manifest.v2+json") != 0) {
		say(why, cap, name, "its manifest is of a kind hull does not read");
		goto done;
	}
	if (!hull_json_member(&doc, &root, "config", &config) ||
	    hull_json_member_string(&doc, &config, "digest", config_digest, sizeof config_digest) < 0 ||
	    !is_digest(config_digest) || !hull_json_member(&doc, &root, "layers", &layers)) {
		say(why, cap, name, "its manifest names no config, or no layers");
		goto done;
	}
	fprintf(stderr, "hull: pull: %s is %s\n", name, digest);
	put_text_atom(&digests, digest);
	put_text_atom(&digests, config_digest);
	/* The store's lock, shared, from the first write under tmp/ to the
	 * commit, so that gc, which empties tmp/ under it alone, waits. */
	flock(store->lock, LOCK_SH);
	snprintf(scratch, sizeof scratch, "%s/tmp/pull-XXXXXX", store->root);
	if (mkdtemp(scratch) == NULL) {
		say(why, cap, scratch, strerror(errno));
		scratch[0] = '\0';
		goto done;
	}
	snprintf(tree, sizeof tree, "%s/tree", scratch);
	snprintf(file, sizeof file, "%s/layer", scratch);
	if (mkdir(tree, 0700) != 0 || (layer_fd = open(file, O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC, 0600)) < 0) {
		say(why, cap, scratch, strerror(errno));
		goto done;
	}
	/* The config, for the arch the image says it is. */
	{
		char            url[2048];
		hull_json       config_doc;
		hull_json_value config_root;
		const char     *bad;
		uint8_t        *keep = malloc(MAX_DOCUMENT);
		sink            config_body = { .data = keep, .cap = MAX_DOCUMENT, .fd = -1 };

		if (keep == NULL) {
			say(why, cap, "out of memory", NULL);
			goto done;
		}
		snprintf(url, sizeof url, "https://%s/v2/%s/blobs/%s", s.img.host, s.img.repository,
		         config_digest);
		if (fetch(&s, url, NULL, &config_body, why, cap) != 0 ||
		    verify(&config_body, config_digest, "the config", why, cap) != 0 ||
		    hull_json_open(&config_doc, (const char *)keep, config_body.length, &config_root,
		                   &bad) != 0 ||
		    hull_json_member_string(&config_doc, &config_root, "architecture", image_arch,
		                            sizeof image_arch) <= 0) {
			if (why[0] == '\0')
				say(why, cap, "the config", "it names no architecture");
			free(keep);
			goto done;
		}
		if (runtime_of(&config_doc, &config_root, &runtime) != 0) {
			say(why, cap, "the config", "its defaults could not be read");
			free(keep);
			goto done;
		}
		free(keep);
		if (strcmp(image_arch, arch) != 0) {
			char text[160];

			snprintf(text, sizeof text, "the image is for %s, not %s", image_arch, arch);
			say(why, cap, name, text);
			goto done;
		}
	}
	if ((u = hull_unpack_begin(tree, why, cap)) == NULL)
		goto done;
	for (int more = hull_json_element(&doc, &layers, 1, &layer); more;
	     more = hull_json_element(&doc, &layers, 0, &layer))
		count++;
	for (int more = hull_json_element(&doc, &layers, 1, &layer); more;
	     more = hull_json_element(&doc, &layers, 0, &layer)) {
		char            layer_digest[80];
		char            type[256] = "";
		char            url[2048];
		hull_json_value size;
		hull_json_value urls;
		long long       declared = -1;
		sink            to_file = { .fd = layer_fd };

		index++;
		hull_json_member_string(&doc, &layer, "mediaType", type, sizeof type);
		if (hull_json_member_string(&doc, &layer, "digest", layer_digest, sizeof layer_digest) < 0 ||
		    !is_digest(layer_digest) || !hull_json_member(&doc, &layer, "size", &size) ||
		    hull_json_integer(&doc, &size, &declared) != 0 || declared < 0) {
			say(why, cap, name, "a layer with no digest or size");
			goto done;
		}
		if (strstr(type, "zstd") != NULL || strstr(type, "nondistributable") != NULL ||
		    strstr(type, "foreign") != NULL || hull_json_member(&doc, &layer, "urls", &urls) ||
		    (type[0] != '\0' && strstr(type, "tar") == NULL)) {
			say(why, cap, layer_digest, strstr(type, "zstd") != NULL ?
			    "a zstd layer, which hull does not yet read" :
			    "a layer of a kind hull does not fetch");
			goto done;
		}
		fprintf(stderr, "hull: pull: layer %d of %d, %s, %lld bytes\n", index, count, layer_digest,
		        declared);
		put_text_atom(&digests, layer_digest);
		snprintf(url, sizeof url, "https://%s/v2/%s/blobs/%s", s.img.host, s.img.repository,
		         layer_digest);
		if (fetch(&s, url, NULL, &to_file, why, cap) != 0 ||
		    verify(&to_file, layer_digest, layer_digest, why, cap) != 0)
			goto done;
		if (to_file.written != declared) {
			say(why, cap, layer_digest, "its length is not the size the manifest gives");
			goto done;
		}
		if (hull_unpack_layer(u, layer_fd, &skipped, why, cap) != 0)
			goto done;
	}
	{
		hull_unpack *ended = u;

		u = NULL;
		if (hull_unpack_end(ended, why, cap) != 0)
			goto done;
	}
	/* A.5: who made it, and where it came from, the registry named as
	 * the image's name names it. */
	put_raw(&provenance, "(10:provenance(7:builder");
	put_text_atom(&provenance, builder);
	put_raw(&provenance, ")(3:oci(7:digests");
	put_bytes(&provenance, digests.data, digests.length);
	put_raw(&provenance, ")(8:registry");
	{
		char from[1024];

		snprintf(from, sizeof from, "%s/%s",
		         strcmp(s.img.host, "registry-1.docker.io") == 0 ? "docker.io" : s.img.host,
		         s.img.repository);
		put_text_atom(&provenance, from);
	}
	put_raw(&provenance, ")))");
	if (digests.failed || provenance.failed || runtime.failed) {
		say(why, cap, "out of memory", NULL);
		goto done;
	}
	extra = (hull_commit_extra){ provenance.data, provenance.length, runtime.data, runtime.length };
	if (hull_store_commit(store, tree, image_arch, ref, &extra, out, why, cap) != 0)
		goto done;
	out->skipped += skipped;
	result = 0;
done:
	{
		char ignored[64];

		hull_unpack_end(u, ignored, sizeof ignored);
	}
	if (layer_fd >= 0)
		close(layer_fd);
	if (scratch[0] != '\0')
		hull_store_remove_tree(AT_FDCWD, scratch);
	flock(store->lock, LOCK_UN);
	free(data);
	free(runtime.data);
	free(digests.data);
	free(provenance.data);
	return result;
}
