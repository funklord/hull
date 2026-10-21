/*
 * json.h -- a JSON reader for what a registry sends (section 3.5): the
 * image index, the image manifest, the config and a token, each a small
 * document read whole. It checks the whole document once, RFC 8259's
 * grammar, nesting at most 64 deep, and then answers by walking it, so
 * nothing is allocated and no value is read twice into memory.
 */

#ifndef HULL_JSON_H
#define HULL_JSON_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct hull_json {
	const char *text;
	size_t      length;
} hull_json;

/* A value: its first byte and the byte after it. `kind` is its first
 * byte's class: '{', '[', '"', '0' for a number, 't', 'f' or 'n'. */
typedef struct hull_json_value {
	size_t at;
	size_t end;
	char   kind;
} hull_json_value;

/* The document, checked whole, and its one top-level value: 0, or -1 with
 * `*why` saying what is not JSON. */
int hull_json_open(hull_json *doc, const char *text, size_t length, hull_json_value *root,
                   const char **why);

/* An object's member named `key`, the first if there are several: 1 found,
 * 0 not, or not an object. */
int hull_json_member(const hull_json *doc, const hull_json_value *object, const char *key,
                     hull_json_value *out);

/* An array's elements in turn: `*item` holds the first after a call with
 * `first`, and each later call moves it on. 1 while there is one. */
int hull_json_element(const hull_json *doc, const hull_json_value *array, int first,
                      hull_json_value *item);

/* An object's members in turn, as hull_json_element() walks an array:
 * `*key` the name, a string, and `*value` its value. */
int hull_json_next_member(const hull_json *doc, const hull_json_value *object, int first,
                          hull_json_value *key, hull_json_value *value);

/* A string, its escapes decoded, \u ones to UTF-8: its length, or -1 when
 * it is not a string, does not fit `cap` with its NUL, or holds a NUL. */
long hull_json_string(const hull_json *doc, const hull_json_value *value, char *out, size_t cap);

/* A string member's value, as hull_json_string(), or -1 when absent. */
long hull_json_member_string(const hull_json *doc, const hull_json_value *object, const char *key,
                             char *out, size_t cap);

/* A whole number not past 2^63: 0, or -1 when the value is no such number. */
int hull_json_integer(const hull_json *doc, const hull_json_value *value, long long *out);

#ifdef __cplusplus
}
#endif

#endif /* HULL_JSON_H */
