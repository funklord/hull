/*
 * https.c -- one GET over TLS: the connection, OpenSSL's verification of
 * the peer and its name, the request, and the response's status, headers
 * and body, framed by Content-Length, by chunks, or by a closed
 * connection, and each refused when it ends short. See https.h.
 */

#define _GNU_SOURCE

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>

#include "https.h"

#define CONNECT_SECONDS 30
#define READ_SECONDS    60
#define MAX_HEADERS     (64u << 10)

static int say(char *why, size_t cap, const char *what, const char *detail)
{
	if (detail != NULL)
		snprintf(why, cap, "%s: %s", what, detail);
	else
		snprintf(why, cap, "%s", what);
	return -1;
}

/* --- the URL ----------------------------------------------------------- */

typedef struct url {
	char host[256];
	char port[8];
	char path[8192];
	int  port_given;
} url;

static int parse_url(const char *text, url *out)
{
	const char *at;
	const char *slash;
	const char *colon;
	size_t      n;

	if (strncmp(text, "https://", 8u) != 0)
		return -1;
	at = text + 8;
	slash = strchr(at, '/');
	n = slash != NULL ? (size_t)(slash - at) : strlen(at);
	if (n == 0u || n >= sizeof out->host || memchr(at, '[', n) != NULL || memchr(at, '@', n) != NULL)
		return -1;
	colon = memchr(at, ':', n);
	out->port_given = colon != NULL;
	if (colon != NULL) {
		size_t digits = n - (size_t)(colon - at) - 1u;

		if (digits == 0u || digits >= sizeof out->port ||
		    strspn(colon + 1, "0123456789") < digits)
			return -1;
		memcpy(out->port, colon + 1, digits);
		out->port[digits] = '\0';
		n = (size_t)(colon - at);
	} else {
		strcpy(out->port, "443");
	}
	memcpy(out->host, at, n);
	out->host[n] = '\0';
	if (snprintf(out->path, sizeof out->path, "%s", slash != NULL ? slash : "/") >=
	    (int)sizeof out->path)
		return -1;
	/* Nothing a request line or a header could be broken by. */
	for (const char *p = out->path; *p != '\0'; p++)
		if ((unsigned char)*p <= 0x20u || *p == 0x7f)
			return -1;
	return 0;
}

int hull_https_host(const char *text, char *out, size_t cap)
{
	url u;

	if (parse_url(text, &u) != 0)
		return -1;
	return snprintf(out, cap, "%s%s%s", u.host, u.port_given ? ":" : "",
	                u.port_given ? u.port : "") < (int)cap ? 0 : -1;
}

/* --- the connection ---------------------------------------------------- */

typedef struct conn {
	int     fd;
	SSL    *ssl;
	uint8_t buffer[16384];
	size_t  at;
	size_t  length;
	int     clean;  /* the peer ended with a TLS close, not a cut */
} conn;

static int dial(const url *u, char *why, size_t cap)
{
	struct addrinfo  hints = { 0 };
	struct addrinfo *found;
	struct addrinfo *a;
	int              fd = -1;
	int              status;

	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	if ((status = getaddrinfo(u->host, u->port, &hints, &found)) != 0)
		return say(why, cap, u->host, gai_strerror(status));
	snprintf(why, cap, "%s: no address to reach", u->host);
	for (a = found; a != NULL && fd < 0; a = a->ai_next) {
		struct pollfd  p;
		struct timeval limit = { READ_SECONDS, 0 };
		int            error = 0;
		socklen_t      size = sizeof error;

		fd = socket(a->ai_family, a->ai_socktype | SOCK_CLOEXEC | SOCK_NONBLOCK, a->ai_protocol);
		if (fd < 0)
			continue;
		if (connect(fd, a->ai_addr, a->ai_addrlen) != 0 && errno != EINPROGRESS) {
			say(why, cap, u->host, strerror(errno));
			close(fd);
			fd = -1;
			continue;
		}
		p = (struct pollfd){ fd, POLLOUT, 0 };
		if (poll(&p, 1u, CONNECT_SECONDS * 1000) != 1 ||
		    getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &size) != 0 || error != 0) {
			say(why, cap, u->host, error != 0 ? strerror(error) : "the connection timed out");
			close(fd);
			fd = -1;
			continue;
		}
		fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) & ~O_NONBLOCK);
		setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &limit, sizeof limit);
		setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &limit, sizeof limit);
	}
	freeaddrinfo(found);
	return fd;
}

static SSL_CTX *context(void)
{
	static SSL_CTX *made;

	if (made == NULL && (made = SSL_CTX_new(TLS_client_method())) != NULL) {
		SSL_CTX_set_min_proto_version(made, TLS1_2_VERSION);
		SSL_CTX_set_verify(made, SSL_VERIFY_PEER, NULL);
		if (SSL_CTX_set_default_verify_paths(made) != 1) {
			SSL_CTX_free(made);
			made = NULL;
		}
	}
	return made;
}

static void tls_reason(const url *u, SSL *ssl, char *why, size_t cap)
{
	long          verified = SSL_get_verify_result(ssl);
	unsigned long error = ERR_get_error();
	char          text[256];

	if (verified != X509_V_OK) {
		say(why, cap, u->host, X509_verify_cert_error_string(verified));
	} else {
		ERR_error_string_n(error, text, sizeof text);
		say(why, cap, u->host, error != 0u ? text : "the TLS handshake failed");
	}
	ERR_clear_error();
}

static int open_conn(conn *c, const url *u, char *why, size_t cap)
{
	SSL_CTX        *ctx = context();
	struct in6_addr address;
	int             literal = inet_pton(AF_INET, u->host, &address) == 1 ||
	                          inet_pton(AF_INET6, u->host, &address) == 1;

	memset(c, 0, sizeof *c);
	c->fd = -1;
	if (ctx == NULL)
		return say(why, cap, "TLS", "the host's trust store could not be read");
	if ((c->fd = dial(u, why, cap)) < 0)
		return -1;
	if ((c->ssl = SSL_new(ctx)) == NULL || SSL_set_fd(c->ssl, c->fd) != 1)
		return say(why, cap, u->host, "TLS could not be begun");
	/* The peer's certificate must name the host the URL does: by DNS
	 * name, which SNI also sends, or by address. */
	if (literal ? X509_VERIFY_PARAM_set1_ip_asc(SSL_get0_param(c->ssl), u->host) != 1 :
	              SSL_set_tlsext_host_name(c->ssl, u->host) != 1 || SSL_set1_host(c->ssl, u->host) != 1)
		return say(why, cap, u->host, "its name could not be set to be verified");
	if (SSL_connect(c->ssl) != 1 || SSL_get_verify_result(c->ssl) != X509_V_OK) {
		tls_reason(u, c->ssl, why, cap);
		return -1;
	}
	return 0;
}

static void close_conn(conn *c)
{
	if (c->ssl != NULL)
		SSL_free(c->ssl);
	if (c->fd >= 0)
		close(c->fd);
	c->ssl = NULL;
	c->fd = -1;
}

static int send_all(conn *c, const char *data, size_t n)
{
	while (n > 0u) {
		int sent = SSL_write(c->ssl, data, n > 65536u ? 65536 : (int)n);

		if (sent <= 0)
			return -1;
		data += sent;
		n -= (size_t)sent;
	}
	return 0;
}

/* More into the buffer: 1, 0 at the end, -1 on an error. */
static int fill(conn *c)
{
	int got;

	if (c->at < c->length)
		return 1;
	c->at = c->length = 0u;
	got = SSL_read(c->ssl, c->buffer, (int)sizeof c->buffer);
	if (got > 0) {
		c->length = (size_t)got;
		return 1;
	}
	switch (SSL_get_error(c->ssl, got)) {
	case SSL_ERROR_ZERO_RETURN:
		c->clean = 1;
		return 0;
	case SSL_ERROR_SYSCALL:
	case SSL_ERROR_SSL:
		/* A cut with no TLS close: an end, but not a clean one. */
		ERR_clear_error();
		return errno == EAGAIN || errno == EWOULDBLOCK ? -1 : 0;
	default:
		return -1;
	}
}

/* A line, CRLF or LF ended, its end taken off: its length, or -1. */
static long read_line(conn *c, char *out, size_t cap)
{
	size_t n = 0u;

	for (;;) {
		int more = fill(c);

		if (more <= 0)
			return -1;
		while (c->at < c->length) {
			char ch = (char)c->buffer[c->at++];

			if (ch == '\n') {
				if (n > 0u && out[n - 1u] == '\r')
					n--;
				out[n] = '\0';
				return (long)n;
			}
			if (n + 1u >= cap)
				return -1;
			out[n++] = ch;
		}
	}
}

/* `n` bytes of body to `body`, or to the end where `n` is -1. */
static int read_body(conn *c, long long n, hull_https_body body, void *context, char *why,
                     size_t cap, const char *host)
{
	while (n != 0) {
		size_t take;
		int    more = fill(c);

		if (more < 0)
			return say(why, cap, host, "the response could not be read");
		if (more == 0) {
			if (n > 0)
				return say(why, cap, host, "the response ended before its length");
			if (!c->clean)
				return say(why, cap, host, "the response was cut off, with no TLS close");
			return 0;
		}
		take = c->length - c->at;
		if (n > 0 && (unsigned long long)take > (unsigned long long)n)
			take = (size_t)n;
		if (body(context, c->buffer + c->at, take, why, cap) != 0)
			return -1;
		c->at += take;
		if (n > 0)
			n -= (long long)take;
	}
	return 0;
}

static int read_chunks(conn *c, hull_https_body body, void *context, char *why, size_t cap,
                       const char *host)
{
	char line[1024];

	for (;;) {
		char              *end;
		unsigned long long size;

		if (read_line(c, line, sizeof line) < 0)
			return say(why, cap, host, "a chunk's size could not be read");
		errno = 0;
		size = strtoull(line, &end, 16);
		if (end == line || errno != 0 || (*end != '\0' && *end != ';' && *end != ' ') ||
		    size > (1ull << 40))
			return say(why, cap, host, "a chunk's size is not one");
		if (size == 0u)
			break;
		if (read_body(c, (long long)size, body, context, why, cap, host) != 0)
			return -1;
		if (read_line(c, line, sizeof line) != 0)
			return say(why, cap, host, "a chunk is not ended where its size says");
	}
	/* Trailers, read and dropped, to the empty line. */
	for (unsigned i = 0u; i < 64u; i++) {
		long n = read_line(c, line, sizeof line);

		if (n < 0)
			return say(why, cap, host, "the chunks' trailer could not be read");
		if (n == 0)
			return 0;
	}
	return say(why, cap, host, "the chunks' trailer is too long");
}

static void header_value(const char *line, const char *name, char *out, size_t cap)
{
	size_t n = strlen(name);

	if (strncasecmp(line, name, n) != 0 || line[n] != ':' || out[0] != '\0')
		return;
	line += n + 1u;
	while (*line == ' ' || *line == '\t')
		line++;
	snprintf(out, cap, "%s", line);
}

/* The upload's bytes after the request's head. */
static int send_upload(conn *c, const hull_https_upload *upload, const char *host, char *why,
                       size_t cap)
{
	uint8_t   buffer[65536];
	long long left = upload->length;

	if (upload->data != NULL)
		return send_all(c, (const char *)upload->data, (size_t)upload->length) == 0 ? 0 :
		       say(why, cap, host, "the request's body could not be sent");
	if (lseek(upload->fd, 0, SEEK_SET) != 0)
		return say(why, cap, "the body to send", strerror(errno));
	while (left > 0) {
		size_t  want = left > (long long)sizeof buffer ? sizeof buffer : (size_t)left;
		ssize_t got = read(upload->fd, buffer, want);

		if (got < 0 && errno == EINTR)
			continue;
		if (got <= 0)
			return say(why, cap, "the body to send", got < 0 ? strerror(errno) : "it ended short");
		if (send_all(c, (const char *)buffer, (size_t)got) != 0)
			return say(why, cap, host, "the request's body could not be sent");
		left -= got;
	}
	return 0;
}

int hull_https_get(const char *text, const char *const *headers, hull_https_response *out,
                   hull_https_body body, void *context, char *why, size_t cap)
{
	return hull_https_request("GET", text, headers, NULL, out, body, context, why, cap);
}

int hull_https_request(const char *method, const char *text, const char *const *headers,
                       const hull_https_upload *upload, hull_https_response *out,
                       hull_https_body body, void *context, char *why, size_t cap)
{
	int    sends = strcmp(method, "POST") == 0 || strcmp(method, "PUT") == 0;
	url    u;
	conn   c;
	char  *request;
	size_t length = 0u;
	size_t room = 16384u;
	char   line[8192];
	char   encoding[256] = "";
	char   content_length[64] = "";
	size_t header_bytes = 0u;
	int    result = -1;
	int    chunked;

	memset(out, 0, sizeof *out);
	out->length = -1;
	if (parse_url(text, &u) != 0)
		return say(why, cap, text, "not an https:// URL this reads");
	for (const char *const *h = headers; h != NULL && *h != NULL; h++) {
		room += strlen(*h) + 2u;
		if (strpbrk(*h, "\r\n") != NULL)
			return say(why, cap, *h, "a header holding a line break");
	}
	if ((request = malloc(room)) == NULL)
		return say(why, cap, "out of memory", NULL);
	if (strcmp(method, "GET") != 0 && strcmp(method, "HEAD") != 0 && !sends) {
		free(request);
		return say(why, cap, method, "not a method this sends");
	}
	length = (size_t)snprintf(request, room,
	                          "%s %s HTTP/1.1\r\nHost: %s%s%s\r\nUser-Agent: hull\r\n"
	                          "Accept-Encoding: identity\r\nConnection: close\r\n",
	                          method, u.path, u.host, u.port_given ? ":" : "",
	                          u.port_given ? u.port : "");
	for (const char *const *h = headers; h != NULL && *h != NULL; h++)
		length += (size_t)snprintf(request + length, room - length, "%s\r\n", *h);
	if (sends)
		length += (size_t)snprintf(request + length, room - length, "Content-Length: %lld\r\n",
		                           upload != NULL ? upload->length : 0);
	length += (size_t)snprintf(request + length, room - length, "\r\n");
	if (open_conn(&c, &u, why, cap) != 0)
		goto done;
	if (send_all(&c, request, length) != 0) {
		say(why, cap, u.host, "the request could not be sent");
		goto done;
	}
	if (sends && upload != NULL && upload->length > 0 &&
	    send_upload(&c, upload, u.host, why, cap) != 0)
		goto done;
	if (read_line(&c, line, sizeof line) < 0 || strncmp(line, "HTTP/1.", 7u) != 0 ||
	    strlen(line) < 12u || line[8] != ' ' || strspn(line + 9, "0123456789") < 3u) {
		say(why, cap, u.host, "it did not answer with an HTTP status");
		goto done;
	}
	out->status = atoi(line + 9);
	for (;;) {
		long n = read_line(&c, line, sizeof line);

		if (n < 0 || (header_bytes += (size_t)n) > MAX_HEADERS) {
			say(why, cap, u.host, "its headers could not be read, or are too long");
			goto done;
		}
		if (n == 0)
			break;
		header_value(line, "Content-Length", content_length, sizeof content_length);
		header_value(line, "Transfer-Encoding", encoding, sizeof encoding);
		header_value(line, "Location", out->location, sizeof out->location);
		header_value(line, "WWW-Authenticate", out->authenticate, sizeof out->authenticate);
		header_value(line, "Content-Type", out->content_type, sizeof out->content_type);
		header_value(line, "Docker-Content-Digest", out->digest, sizeof out->digest);
	}
	if (content_length[0] != '\0') {
		char *end;

		errno = 0;
		out->length = strtoll(content_length, &end, 10);
		if (end == content_length || *end != '\0' || errno != 0 || out->length < 0) {
			say(why, cap, u.host, "a Content-Length that is not one");
			goto done;
		}
	}
	/* A redirect's body is nobody's, nor a HEAD's, which has none; the
	 * connection goes with it. */
	if (strcmp(method, "HEAD") == 0 || out->status == 301 || out->status == 302 || out->status == 303 || out->status == 307 ||
	    out->status == 308 || out->status == 204 || out->status == 304) {
		result = 0;
		goto done;
	}
	chunked = strcasestr(encoding, "chunked") != NULL;
	if (chunked)
		result = read_chunks(&c, body, context, why, cap, u.host);
	else
		result = read_body(&c, out->length, body, context, why, cap, u.host);
done:
	close_conn(&c);
	free(request);
	return result;
}
