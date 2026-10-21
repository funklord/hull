/*
 * json.c -- RFC 8259, read: one checking walk over the whole document,
 * then walks that trust it. See json.h.
 */

#include <stdint.h>
#include <string.h>

#include "json.h"

#define MAX_DEPTH 64

static size_t blank(const hull_json *doc, size_t at)
{
	while (at < doc->length && strchr(" \t\n\r", doc->text[at]) != NULL && doc->text[at] != '\0')
		at++;
	return at;
}

static int hex_digit(char c)
{
	return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 :
	       c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

/* The length of the UTF-8 sequence at `at`, or 0 for one that is not:
 * overlong, a surrogate, past U+10FFFF, or cut short. */
static size_t utf8_length(const hull_json *doc, size_t at)
{
	const unsigned char *p = (const unsigned char *)doc->text + at;
	size_t               left = doc->length - at;
	size_t               n;
	uint32_t             code;

	if (p[0] < 0x80u)
		return 1u;
	if (p[0] >= 0xc2u && p[0] <= 0xdfu)
		n = 2u, code = p[0] & 0x1fu;
	else if (p[0] >= 0xe0u && p[0] <= 0xefu)
		n = 3u, code = p[0] & 0x0fu;
	else if (p[0] >= 0xf0u && p[0] <= 0xf4u)
		n = 4u, code = p[0] & 0x07u;
	else
		return 0u;
	if (left < n)
		return 0u;
	for (size_t i = 1u; i < n; i++) {
		if ((p[i] & 0xc0u) != 0x80u)
			return 0u;
		code = code << 6 | (p[i] & 0x3fu);
	}
	if ((n == 3u && code < 0x800u) || (n == 4u && (code < 0x10000u || code > 0x10ffffu)) ||
	    (code >= 0xd800u && code <= 0xdfffu))
		return 0u;
	return n;
}

static int scan(const hull_json *doc, size_t at, int depth, size_t *end, const char **why);

static int scan_string(const hull_json *doc, size_t at, size_t *end, const char **why)
{
	for (at++; at < doc->length; ) {
		unsigned char c = (unsigned char)doc->text[at];
		size_t        n;

		if (c == '"') {
			*end = at + 1u;
			return 0;
		}
		if (c < 0x20u) {
			*why = "a string holding a control character";
			return -1;
		}
		if (c == '\\') {
			if (at + 1u >= doc->length || strchr("\"\\/bfnrtu", doc->text[at + 1u]) == NULL ||
			    doc->text[at + 1u] == '\0') {
				*why = "a string holding an escape JSON has not";
				return -1;
			}
			if (doc->text[at + 1u] == 'u') {
				if (at + 6u > doc->length) {
					*why = "a \\u escape cut short";
					return -1;
				}
				for (size_t i = 2u; i < 6u; i++)
					if (hex_digit(doc->text[at + i]) < 0) {
						*why = "a \\u escape that is not four hexadecimal digits";
						return -1;
					}
				at += 6u;
			} else {
				at += 2u;
			}
			continue;
		}
		if ((n = utf8_length(doc, at)) == 0u) {
			*why = "a string that is not UTF-8";
			return -1;
		}
		at += n;
	}
	*why = "a string that is not closed";
	return -1;
}

static int scan_number(const hull_json *doc, size_t at, size_t *end, const char **why)
{
	const char *t = doc->text;
	size_t      n = doc->length;

	if (at < n && t[at] == '-')
		at++;
	if (at < n && t[at] == '0') {
		at++;
	} else if (at < n && t[at] >= '1' && t[at] <= '9') {
		while (at < n && t[at] >= '0' && t[at] <= '9')
			at++;
	} else {
		*why = "a number with no digits";
		return -1;
	}
	if (at < n && t[at] == '.') {
		if (++at >= n || t[at] < '0' || t[at] > '9') {
			*why = "a number with no digits after its point";
			return -1;
		}
		while (at < n && t[at] >= '0' && t[at] <= '9')
			at++;
	}
	if (at < n && (t[at] == 'e' || t[at] == 'E')) {
		if (++at < n && (t[at] == '+' || t[at] == '-'))
			at++;
		if (at >= n || t[at] < '0' || t[at] > '9') {
			*why = "a number with no digits in its exponent";
			return -1;
		}
		while (at < n && t[at] >= '0' && t[at] <= '9')
			at++;
	}
	*end = at;
	return 0;
}

/* `[...]` or `{...}`, whose members are each `"key": value` in an object. */
static int scan_container(const hull_json *doc, size_t at, int depth, size_t *end,
                          const char **why)
{
	char close = doc->text[at] == '{' ? '}' : ']';

	if (depth >= MAX_DEPTH) {
		*why = "nesting deeper than 64";
		return -1;
	}
	at = blank(doc, at + 1u);
	if (at < doc->length && doc->text[at] == close) {
		*end = at + 1u;
		return 0;
	}
	for (;;) {
		if (close == '}') {
			if (at >= doc->length || doc->text[at] != '"') {
				*why = "an object's member with no string for its name";
				return -1;
			}
			if (scan_string(doc, at, &at, why) != 0)
				return -1;
			at = blank(doc, at);
			if (at >= doc->length || doc->text[at] != ':') {
				*why = "an object's member with no colon";
				return -1;
			}
			at = blank(doc, at + 1u);
		}
		if (scan(doc, at, depth + 1, &at, why) != 0)
			return -1;
		at = blank(doc, at);
		if (at < doc->length && doc->text[at] == ',') {
			at = blank(doc, at + 1u);
			continue;
		}
		if (at < doc->length && doc->text[at] == close) {
			*end = at + 1u;
			return 0;
		}
		*why = close == '}' ? "an object that is not closed" : "an array that is not closed";
		return -1;
	}
}

static int scan(const hull_json *doc, size_t at, int depth, size_t *end, const char **why)
{
	static const char *const words[] = { "true", "false", "null" };

	if (at >= doc->length) {
		*why = "a value missing";
		return -1;
	}
	switch (doc->text[at]) {
	case '{':
	case '[':
		return scan_container(doc, at, depth, end, why);
	case '"':
		return scan_string(doc, at, end, why);
	default:
		break;
	}
	for (unsigned i = 0u; i < 3u; i++) {
		size_t n = strlen(words[i]);

		if (doc->length - at >= n && memcmp(doc->text + at, words[i], n) == 0) {
			*end = at + n;
			return 0;
		}
	}
	return scan_number(doc, at, end, why);
}

static void value_at(const hull_json *doc, size_t at, hull_json_value *out)
{
	const char *why;
	char        c = doc->text[at];

	scan(doc, at, 0, &out->end, &why);
	out->at = at;
	out->kind = c == '-' || (c >= '0' && c <= '9') ? '0' : c;
}

int hull_json_open(hull_json *doc, const char *text, size_t length, hull_json_value *root,
                   const char **why)
{
	size_t at;
	size_t end;

	doc->text = text;
	doc->length = length;
	at = blank(doc, 0u);
	if (scan(doc, at, 0, &end, why) != 0)
		return -1;
	if (blank(doc, end) != length) {
		*why = "more after the document's value";
		return -1;
	}
	value_at(doc, at, root);
	return 0;
}

/* The string at `at`, decoded into `out`: its length, or -1. */
static long decode(const hull_json *doc, size_t at, char *out, size_t cap)
{
	size_t n = 0u;

	for (at++; doc->text[at] != '"'; ) {
		uint32_t code;
		char     c = doc->text[at];

		if (c != '\\') {
			if (n + 1u >= cap)
				return -1;
			out[n++] = c;
			at++;
			continue;
		}
		c = doc->text[at + 1u];
		if (c != 'u') {
			if (n + 1u >= cap)
				return -1;
			out[n++] = c == 'b' ? '\b' : c == 'f' ? '\f' : c == 'n' ? '\n' : c == 'r' ? '\r' :
			           c == 't' ? '\t' : c;
			at += 2u;
			continue;
		}
		code = 0u;
		for (size_t i = 2u; i < 6u; i++)
			code = code << 4 | (uint32_t)hex_digit(doc->text[at + i]);
		at += 6u;
		/* A pair of surrogates is one code point; a lone one is none. */
		if (code >= 0xd800u && code <= 0xdbffu && doc->text[at] == '\\' &&
		    doc->text[at + 1u] == 'u') {
			uint32_t low = 0u;

			for (size_t i = 2u; i < 6u; i++)
				low = low << 4 | (uint32_t)hex_digit(doc->text[at + i]);
			if (low < 0xdc00u || low > 0xdfffu)
				return -1;
			code = 0x10000u + ((code - 0xd800u) << 10) + (low - 0xdc00u);
			at += 6u;
		} else if (code >= 0xd800u && code <= 0xdfffu) {
			return -1;
		}
		if (code == 0u || n + 5u >= cap)
			return -1;
		if (code < 0x80u) {
			out[n++] = (char)code;
		} else if (code < 0x800u) {
			out[n++] = (char)(0xc0u | code >> 6);
			out[n++] = (char)(0x80u | (code & 0x3fu));
		} else if (code < 0x10000u) {
			out[n++] = (char)(0xe0u | code >> 12);
			out[n++] = (char)(0x80u | (code >> 6 & 0x3fu));
			out[n++] = (char)(0x80u | (code & 0x3fu));
		} else {
			out[n++] = (char)(0xf0u | code >> 18);
			out[n++] = (char)(0x80u | (code >> 12 & 0x3fu));
			out[n++] = (char)(0x80u | (code >> 6 & 0x3fu));
			out[n++] = (char)(0x80u | (code & 0x3fu));
		}
	}
	out[n] = '\0';
	return (long)n;
}

int hull_json_member(const hull_json *doc, const hull_json_value *object, const char *key,
                     hull_json_value *out)
{
	const char *why;
	size_t      at;

	if (object->kind != '{')
		return 0;
	at = blank(doc, object->at + 1u);
	while (doc->text[at] == '"') {
		char   name[256];
		long   n = decode(doc, at, name, sizeof name);
		size_t value;

		scan_string(doc, at, &at, &why);
		value = blank(doc, blank(doc, at) + 1u);
		if (n >= 0 && strcmp(name, key) == 0) {
			value_at(doc, value, out);
			return 1;
		}
		scan(doc, value, 0, &at, &why);
		at = blank(doc, at);
		if (doc->text[at] != ',')
			break;
		at = blank(doc, at + 1u);
	}
	return 0;
}

int hull_json_element(const hull_json *doc, const hull_json_value *array, int first,
                      hull_json_value *item)
{
	size_t at;

	if (array->kind != '[')
		return 0;
	if (first) {
		at = blank(doc, array->at + 1u);
	} else {
		at = blank(doc, item->end);
		if (doc->text[at] != ',')
			return 0;
		at = blank(doc, at + 1u);
	}
	if (doc->text[at] == ']')
		return 0;
	value_at(doc, at, item);
	return 1;
}

long hull_json_string(const hull_json *doc, const hull_json_value *value, char *out, size_t cap)
{
	if (value->kind != '"' || cap == 0u)
		return -1;
	return decode(doc, value->at, out, cap);
}

long hull_json_member_string(const hull_json *doc, const hull_json_value *object, const char *key,
                             char *out, size_t cap)
{
	hull_json_value value;

	return hull_json_member(doc, object, key, &value) ? hull_json_string(doc, &value, out, cap) :
	                                                     -1;
}

int hull_json_integer(const hull_json *doc, const hull_json_value *value, long long *out)
{
	unsigned long long n = 0u;
	size_t             at = value->at;
	int                negative = 0;

	if (value->kind != '0')
		return -1;
	if (doc->text[at] == '-') {
		negative = 1;
		at++;
	}
	for (; at < value->end; at++) {
		if (doc->text[at] < '0' || doc->text[at] > '9')
			return -1;  /* a fraction or an exponent */
		if (n > (unsigned long long)INT64_MAX / 10u)
			return -1;
		n = n * 10u + (unsigned)(doc->text[at] - '0');
		if (n > (unsigned long long)INT64_MAX)
			return -1;
	}
	*out = negative ? -(long long)n : (long long)n;
	return 0;
}
