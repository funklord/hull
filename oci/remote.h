/*
 * remote.h -- a registry as `hull pull` and `hull push` speak to it
 * (section 3.5): what an image's name says, where a body goes, and a
 * request that answers a registry's challenge and follows its redirects.
 */

#ifndef HULL_REMOTE_H
#define HULL_REMOTE_H

#include <stddef.h>
#include <stdint.h>

#include "https.h"
#include "sha256.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The most of a registry's document, a manifest, an index, a config or a
 * token's answer, that is read into memory. */
#define HULL_REMOTE_DOCUMENT (4u << 20)

/* [REGISTRY/]REPOSITORY[:TAG][@sha256:HEX], as docker reads one. */
typedef struct hull_remote_image {
	char host[256];        /* what the API is reached at */
	char repository[512];
	char reference[128];   /* a tag, or sha256:HEX */
	int  by_digest;
} hull_remote_image;

/* Where a body goes: into memory, at most `cap` of it, or into `fd`; hashed
 * either way. */
typedef struct hull_remote_sink {
	uint8_t    *data;
	size_t      length;
	size_t      cap;
	int         fd;
	long long   written;
	hull_sha256 hash;
} hull_remote_sink;

/* One registry's session: its image, and the token it gave, if it has. */
typedef struct hull_remote {
	hull_remote_image img;
	char              token[8192];
	int               authenticating;  /* a token's own GET asks for none */
} hull_remote;

/* Whether `text` is sha256: and 64 lowercase hex digits. */
int hull_remote_is_digest(const char *text);

/* `text` as an image's name: docker.io and `library/` where none is named,
 * `latest` where no tag is. 0, or -1 with `why`. */
int hull_remote_parse(const char *text, hull_remote_image *out, char *why, size_t cap);

/* A body into a hull_remote_sink, as hull_https_request() gives it. */
int hull_remote_take(void *context, const uint8_t *data, size_t n, char *why, size_t cap);

/* `method` to `url`, sending `upload` where it is not NULL, its body into
 * `into`: a 401 answered with a token once, a redirect followed, a 307 or
 * 308 with its method and body, the token sent only to the registry's own
 * host. The last answer into `out`, whatever its status: 0, or -1 with
 * `why` where no answer was had. */
int hull_remote_request(hull_remote *r, const char *method, const char *url,
                        const char *const *headers, const hull_https_upload *upload,
                        hull_remote_sink *into, hull_https_response *out, char *why, size_t cap);

/* A GET of `url` into `into` that must answer 200, `accept` sent where it
 * is not NULL. 0, or -1 with `why`. */
int hull_remote_fetch(hull_remote *r, const char *url, const char *accept, hull_remote_sink *into,
                      char *why, size_t cap);

/* A body's digest, as `into` hashed it, against `digest`. 0, or -1. */
int hull_remote_verify(hull_remote_sink *into, const char *digest, const char *what, char *why,
                       size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* HULL_REMOTE_H */
