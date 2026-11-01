/*
 * json_fuzz.c -- libFuzzer over oci/json.c, the reader of what a registry
 * sends: every input opened, and every value of one that opens walked,
 * each string and number decoded. `make check-fuzz` runs it.
 */

#include <stddef.h>
#include <stdint.h>
#include "json.h"

static void walk(const hull_json *doc, const hull_json_value *v, int depth)
{
	char             text[512];
	long long        n;
	hull_json_value  key;
	hull_json_value  value;

	if (depth > 64)
		return;
	(void)hull_json_string(doc, v, text, sizeof text);
	(void)hull_json_integer(doc, v, &n);
	(void)hull_json_member(doc, v, "mediaType", &value);
	(void)hull_json_member_string(doc, v, "digest", text, sizeof text);
	for (int more = hull_json_element(doc, v, 1, &value); more;
	     more = hull_json_element(doc, v, 0, &value))
		walk(doc, &value, depth + 1);
	for (int more = hull_json_next_member(doc, v, 1, &key, &value); more;
	     more = hull_json_next_member(doc, v, 0, &key, &value)) {
		(void)hull_json_string(doc, &key, text, sizeof text);
		walk(doc, &value, depth + 1);
	}
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	hull_json        doc;
	hull_json_value  root;
	const char      *why;

	if (hull_json_open(&doc, (const char *)data, size, &root, &why) == 0)
		walk(&doc, &root, 0);
	return 0;
}
