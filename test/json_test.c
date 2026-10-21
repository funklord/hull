/*
 * json_test.c -- oci/json.c against documents of the shapes a registry
 * sends and against what RFC 8259 refuses: each refusal a byte away from
 * a document it takes, so that a check too loose shows as a pass.
 */

#include <stdio.h>
#include <string.h>

#include "json.h"

static unsigned cases;
static unsigned failures;

static void fail(const char *what, const char *detail)
{
	failures++;
	printf("FAIL %s: %s\n", what, detail);
}

static void takes(const char *text)
{
	hull_json       doc;
	hull_json_value root;
	const char     *why = "";

	cases++;
	if (hull_json_open(&doc, text, strlen(text), &root, &why) != 0)
		fail(text, why);
}

static void refuses(const char *text, size_t length)
{
	hull_json       doc;
	hull_json_value root;
	const char     *why = "";

	cases++;
	if (hull_json_open(&doc, text, length, &root, &why) == 0)
		fail("taken", text);
}

int main(void)
{
	static const char index[] =
	    "{\"schemaVersion\": 2,\n"
	    " \"mediaType\": \"application/vnd.oci.image.index.v1+json\",\n"
	    " \"manifests\": [\n"
	    "  {\"digest\": \"sha256:aa\", \"platform\": {\"architecture\": \"arm64\", \"os\": \"linux\","
	    " \"variant\": \"v8\"}, \"size\": 1},\n"
	    "  {\"digest\": \"sha256:bb\", \"platform\": {\"os\": \"linux\", \"architecture\": \"amd64\"},"
	    " \"size\": 529}\n"
	    " ],\n"
	    " \"annotations\": {\"a\\u00e9\\ud83d\\ude00\": \"tab\\there \\\"q\\\" \\\\ \\/\"}\n"
	    "}";
	hull_json       doc;
	hull_json_value root;
	hull_json_value list;
	hull_json_value item;
	hull_json_value value;
	const char     *why = "";
	char            text[256];
	long long       n = 0;
	int             count = 0;

	cases++;
	if (hull_json_open(&doc, index, strlen(index), &root, &why) != 0) {
		fail("the index", why);
	} else {
		cases++;
		if (!hull_json_member(&doc, &root, "manifests", &list) || list.kind != '[')
			fail("the index", "no manifests array");
		for (int more = hull_json_element(&doc, &list, 1, &item); more;
		     more = hull_json_element(&doc, &list, 0, &item)) {
			hull_json_value platform;

			count++;
			if (hull_json_member(&doc, &item, "platform", &platform) &&
			    hull_json_member_string(&doc, &platform, "architecture", text, sizeof text) >= 0 &&
			    strcmp(text, "amd64") == 0) {
				cases++;
				if (hull_json_member_string(&doc, &item, "digest", text, sizeof text) < 0 ||
				    strcmp(text, "sha256:bb") != 0)
					fail("the amd64 digest", text);
				cases++;
				if (!hull_json_member(&doc, &item, "size", &value) ||
				    hull_json_integer(&doc, &value, &n) != 0 || n != 529)
					fail("the amd64 size", "not 529");
			}
		}
		cases++;
		if (count != 2)
			fail("the index", "not two manifests");
		/* Escapes: \u to UTF-8, a surrogate pair to one code point. */
		cases++;
		if (!hull_json_member(&doc, &root, "annotations", &value) ||
		    hull_json_member_string(&doc, &value, "a\xc3\xa9\xf0\x9f\x98\x80", text,
		                            sizeof text) < 0 ||
		    strcmp(text, "tab\there \"q\" \\ /") != 0)
			fail("the escapes", text);
		/* The members in order, each name and value. */
		{
			hull_json_value key;
			char            names[256] = "";

			for (int more = hull_json_next_member(&doc, &root, 1, &key, &value); more;
			     more = hull_json_next_member(&doc, &root, 0, &key, &value)) {
				hull_json_string(&doc, &key, text, sizeof text);
				strcat(names, text);
				strcat(names, value.kind == '0' ? "=0 " : value.kind == '"' ? "=s " : "=c ");
			}
			cases++;
			if (strcmp(names, "schemaVersion=0 mediaType=s manifests=c annotations=c ") != 0)
				fail("the members in order", names);
		}
		cases++;
		if (hull_json_member(&doc, &root, "absent", &value))
			fail("a member absent", "found");
		cases++;
		if (hull_json_member_string(&doc, &root, "schemaVersion", text, sizeof text) >= 0)
			fail("a number", "read as a string");
		cases++;
		if (hull_json_member_string(&doc, &root, "mediaType", text, 8u) >= 0)
			fail("a string too long", "read into too small a buffer");
	}

	takes("[]");
	takes("{}");
	takes(" \t\r\n[1, -0, 0.5, -1.5e+10, 2E-3, true, false, null, \"\"] ");
	takes("\"\xe2\x82\xac\"");
	refuses("[1,]", 4u);
	refuses("[1 2]", 5u);
	refuses("{\"a\" 1}", 7u);
	refuses("{\"a\":1,}", 8u);
	refuses("{a:1}", 5u);
	refuses("[01]", 4u);
	refuses("[1.]", 4u);
	refuses("[.5]", 4u);
	refuses("[1e]", 4u);
	refuses("[tru]", 5u);
	refuses("[\"a\x01\"]", 5u);
	refuses("[\"\\x\"]", 6u);
	refuses("[\"\\u12g4\"]", 10u);
	refuses("[\"\xc3\"]", 4u);
	refuses("[\"\xc0\xaf\"]", 5u);
	refuses("[\"\xed\xa0\x80\"]", 6u);
	refuses("[\"open]", 7u);
	refuses("[1] [2]", 7u);
	refuses("", 0u);
	refuses("[1\0]", 4u);
	{
		char deep[200];

		memset(deep, '[', 65u);
		memset(deep + 65, ']', 65u);
		refuses(deep, 130u);
		memset(deep, '[', 64u);
		memset(deep + 64, ']', 64u);
		deep[128] = '\0';
		takes(deep);
	}
	/* A lone surrogate, and \u0000, are not read as a string. */
	{
		static const char lone[] = "[\"\\ud800\", \"a\\u0000b\"]";

		cases++;
		if (hull_json_open(&doc, lone, strlen(lone), &root, &why) != 0 ||
		    !hull_json_element(&doc, &root, 1, &item) ||
		    hull_json_string(&doc, &item, text, sizeof text) >= 0 ||
		    !hull_json_element(&doc, &root, 0, &item) ||
		    hull_json_string(&doc, &item, text, sizeof text) >= 0)
			fail("a lone surrogate or a NUL", "read");
	}
	/* Integers: not a fraction, not past 2^63. */
	{
		static const char numbers[] = "[9223372036854775807, 9223372036854775808, 1.0, -7]";
		long long         want[] = { 9223372036854775807LL, -1, -1, -7 };
		int               i = 0;

		hull_json_open(&doc, numbers, strlen(numbers), &root, &why);
		for (int more = hull_json_element(&doc, &root, 1, &item); more;
		     more = hull_json_element(&doc, &root, 0, &item), i++) {
			int got = hull_json_integer(&doc, &item, &n);

			cases++;
			if ((want[i] == -1 && got == 0) || (want[i] != -1 && (got != 0 || n != want[i])))
				fail("an integer", numbers);
		}
	}
	printf("json: %u cases, %u failed\n", cases, failures);
	return failures != 0;
}
