/*
 * remote.c -- a registry as `hull pull` and `hull push` speak to it
 * (section 3.5): an image's name read as docker reads one, and requests
 * that answer a 401's Bearer challenge with an anonymous token once,
 * follow a redirect five deep, and send the token only to the registry's
 * own host, not to wherever its blobs are served from. See remote.h.
 */

#define _GNU_SOURCE

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "json.h"
#include "remote.h"

#define MAX_REDIRECTS 5

static int say(char *why, size_t cap, const char *what, const char *detail)
{
	if (detail != NULL)
		snprintf(why, cap, "%s: %s", what, detail);
	else
		snprintf(why, cap, "%s", what);
	return -1;
}

/* --- the image's name --------------------------------------------------- */

int hull_remote_is_digest(const char *text)
{
	return strncmp(text, "sha256:", 7u) == 0 && strlen(text) == 71u &&
	       strspn(text + 7, "0123456789abcdef") == 64u;
}

int hull_remote_parse(const char *text, hull_remote_image *out, char *why, size_t cap)
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
		if (!hull_remote_is_digest(at + 1))
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

int hull_remote_take(void *context, const uint8_t *data, size_t n, char *why, size_t cap)
{
	hull_remote_sink *s = context;

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


/* An anonymous token from the challenge's realm, for the pull's scope. */
static int authenticate(hull_remote *s, const char *text, char *why, size_t cap)
{
	char            realm[2048];
	char            service[512];
	char            scope[1024];
	char            encoded_service[1600];
	char            encoded_scope[3200];
	char            url[8192];
	uint8_t        *data = malloc(HULL_REMOTE_DOCUMENT);
	hull_remote_sink body = { .data = data, .cap = HULL_REMOTE_DOCUMENT, .fd = -1 };
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
	if (hull_remote_fetch(s, url, NULL, &body, why, cap) == 0) {
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

int hull_remote_request(hull_remote *s, const char *method, const char *url,
                        const char *const *extra, const hull_https_upload *upload,
                        hull_remote_sink *into, hull_https_response *out, char *why, size_t cap)
{
	char here[8192];
	int  tried_token = 0;

	snprintf(here, sizeof here, "%s", url);
	for (int hops = 0; hops <= MAX_REDIRECTS; hops++) {
		char        host[300];
		char        authorization[8300];
		const char *headers[8];
		unsigned    n = 0u;

		if (hull_https_host(here, host, sizeof host) != 0)
			return say(why, cap, here, "not an https:// URL; hull reaches registries by TLS only");
		for (const char *const *h = extra; h != NULL && *h != NULL && n < 6u; h++)
			headers[n++] = *h;
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
		if (hull_https_request(method, here, headers, upload, out, hull_remote_take, into, why,
		                       cap) != 0)
			return -1;
		if (out->status == 401 && !tried_token && !s->authenticating &&
		    out->authenticate[0] != '\0') {
			tried_token = 1;
			if (authenticate(s, out->authenticate, why, cap) != 0)
				return -1;
			hops--;
			continue;
		}
		if (out->status >= 300 && out->status < 400 && out->location[0] != '\0') {
			char next[8192];

			if (out->location[0] == '/')
				snprintf(next, sizeof next, "https://%s%s", host, out->location);
			else
				snprintf(next, sizeof next, "%s", out->location);
			snprintf(here, sizeof here, "%s", next);
			/* 307 and 308 keep the method and its body; the others are a
			 * GET of where they point, as HTTP has them. */
			if (out->status != 307 && out->status != 308) {
				method = "GET";
				upload = NULL;
			}
			continue;
		}
		return 0;
	}
	return say(why, cap, url, "redirected more than five times");
}

int hull_remote_fetch(hull_remote *s, const char *url, const char *accept, hull_remote_sink *into,
                      char *why, size_t cap)
{
	hull_https_response response;
	const char         *headers[2] = { accept, NULL };

	if (hull_remote_request(s, "GET", url, accept != NULL ? headers : NULL, NULL, into, &response,
	                        why, cap) != 0)
		return -1;
	if (response.status != 200) {
		char status[64];

		snprintf(status, sizeof status, "the registry answered %d", response.status);
		return say(why, cap, url, response.status == 404 ? "not found there" : status);
	}
	return 0;
}

/* A body's digest, against the one that named it. */
int hull_remote_verify(hull_remote_sink *s, const char *digest, const char *what, char *why,
                       size_t cap)
{
	uint8_t raw[32];
	char    hex[65];

	hull_sha256_final(&s->hash, raw);
	hull_sha256_hex(raw, hex);
	if (!hull_remote_is_digest(digest) || strcmp(digest + 7, hex) != 0)
		return say(why, cap, what, "its content is not the digest that names it");
	return 0;
}
