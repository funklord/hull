/*
 * https.h -- one HTTP/1.1 request over TLS, as `hull pull` and `hull push`
 * ask a registry (section 3.5). OpenSSL verifies the peer against the
 * host's trust store, which SSL_CERT_FILE and SSL_CERT_DIR may name
 * instead, and its name against the URL's host; TLS 1.2 is the least
 * taken. A peer on this machine's loopback that does not speak TLS at all
 * is met in plain HTTP; no other is, and a certificate refused is refused.
 * Each request is its own connection, closed after it. A redirect is the
 * caller's to follow, since only it knows whether its credentials go with
 * it.
 */

#ifndef HULL_HTTPS_H
#define HULL_HTTPS_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct hull_https_response {
	int       status;
	long long length;              /* Content-Length, or -1 */
	char      location[4096];      /* a redirect's, or empty */
	char      authenticate[2048];  /* WWW-Authenticate, or empty */
	char      content_type[256];
	char      digest[128];         /* Docker-Content-Digest, or empty */
} hull_https_response;

/* What the body is given to as it arrives: 0 to go on, or -1 to stop the
 * request, which then fails with the reason the callback put in `why`. */
typedef int (*hull_https_body)(void *context, const uint8_t *data, size_t n, char *why,
                               size_t cap);

/* What a request sends: `length` bytes, from `data`, or where it is NULL
 * from `fd` at its start. */
typedef struct hull_https_upload {
	const uint8_t *data;
	int            fd;
	long long      length;
} hull_https_upload;

/* `method`, GET, HEAD, POST or PUT, to `url`, https://, or where
 * hull_https_allow_loopback() was called http:// to a loopback peer, sending `headers`, each "Name: value" and the list NULL-ended, and
 * `upload` where it is not NULL, with its Content-Length. The status and
 * the headers above go into `out`; the body, whole and checked against its
 * framing, to `body`, which a HEAD has none of. 0, or -1 with `why`. */
int hull_https_request(const char *method, const char *url, const char *const *headers,
                       const hull_https_upload *upload, hull_https_response *out,
                       hull_https_body body, void *context, char *why, size_t cap);

/* hull_https_request() with GET and nothing to send. */
int hull_https_get(const char *url, const char *const *headers, hull_https_response *out,
                   hull_https_body body, void *context, char *why, size_t cap);

/* The host of an https:// or http:// URL, with its port where it names
 * one: 0, or -1 for a URL this does not take. */
int hull_https_host(const char *url, char *out, size_t cap);

/* Take plain HTTP from a loopback peer, in this process: a registry
 * session's to say. Nothing else that fetches over this does. */
void hull_https_allow_loopback(void);

/* Whether `host`, a URL's, was met in plain HTTP by this process: the only
 * registry whose redirects and realm may name an http:// URL. */
int hull_https_host_plain(const char *host);

#ifdef __cplusplus
}
#endif

#endif /* HULL_HTTPS_H */
