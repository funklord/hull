/*
 * sexpr_test.c -- the reader, held to what project.md section 4.1 and A.1
 * say the two syntaxes are.
 *
 * Every refusal is checked for its offset as well as its status: a reader
 * that refuses the right document at the wrong byte points a person at
 * the wrong line. The cases situ was told about (situ suggestion/hull.md)
 * are here by name, so the walk is held to the cases where situ's own
 * framing is not enough.
 */

#include <stdio.h>
#include <string.h>

#include "sexpr.h"

static unsigned cases;
static unsigned failures;

#define FAIL(...) do { \
	failures++; \
	printf("FAIL %s:%d: ", __FILE__, __LINE__); \
	printf(__VA_ARGS__); \
	printf("\n"); \
} while (0)

static hull_sexpr_status open_text(hull_sexpr_doc *doc, const char *text, size_t length,
                                   hull_sexpr_syntax syntax, hull_sexpr_error *error)
{
	memset(error, 0, sizeof *error);
	return hull_sexpr_open(doc, (const uint8_t *)text, length, syntax, error);
}

static void accept(hull_sexpr_syntax syntax, const char *text, size_t length)
{
	hull_sexpr_doc   doc;
	hull_sexpr_error error;

	cases++;
	if (open_text(&doc, text, length, syntax, &error) != HULL_SEXPR_OK)
		FAIL("refused %.*s: %s at %u", (int)length, text, error.reason, error.offset);
}

static void refuse(hull_sexpr_syntax syntax, const char *text, size_t length,
                   hull_sexpr_status want, uint32_t offset)
{
	hull_sexpr_doc    doc;
	hull_sexpr_error  error;
	hull_sexpr_status got;

	cases++;
	got = open_text(&doc, text, length, syntax, &error);
	if (got != want)
		FAIL("%.*s: status %d, wanted %d", (int)length, text, (int)got, (int)want);
	else if (error.offset != offset)
		FAIL("%.*s: refused at %u (%s), wanted %u", (int)length, text,
		     error.offset, error.reason, offset);
}

#define READABLE_OK(text) accept(HULL_SEXPR_READABLE, text, sizeof text - 1)
#define READABLE_NO(text, status, at) \
	refuse(HULL_SEXPR_READABLE, text, sizeof text - 1, status, at)
#define CANONICAL_OK(text) accept(HULL_SEXPR_CANONICAL, text, sizeof text - 1)
#define CANONICAL_NO(text, status, at) \
	refuse(HULL_SEXPR_CANONICAL, text, sizeof text - 1, status, at)

static unsigned children(const hull_sexpr_doc *doc, const hull_sexpr_form *list)
{
	hull_sexpr_form form;
	unsigned        n = 0;

	if (!hull_sexpr_child(doc, list, &form))
		return 0;
	do {
		n++;
	} while (hull_sexpr_next(doc, &form, &form));
	return n;
}

static unsigned count_children(const char *text)
{
	hull_sexpr_doc   doc;
	hull_sexpr_error error;
	hull_sexpr_form  top;

	if (open_text(&doc, text, strlen(text), HULL_SEXPR_READABLE, &error) != HULL_SEXPR_OK)
		return 999;
	if (!hull_sexpr_first(&doc, &top))
		return 998;
	return children(&doc, &top);
}

static void test_readable(void)
{
	READABLE_OK("(a b)");
	READABLE_OK("(a(b))");
	READABLE_OK("()");
	READABLE_OK("(a\n\t(b\t\"c\"))");
	READABLE_OK("(run \"say \\\"hi\\\"\")");
	READABLE_OK("(a ; a comment ( with parens \" and a quote\n b)");
	READABLE_OK("; a comment on its own\n(a)\n");
	READABLE_OK("(x #7b22# ##)");
	READABLE_OK("(a) ");
	READABLE_OK("(a)\n(b)\n");
	READABLE_OK("");
	READABLE_OK("; only a comment\n");
	READABLE_OK("(machine\n  (backend ns)\n)\n");
	/* `before` ends a symbol at the end of the input too, where `until`
	 * does not end a comment there (situ suggestion/hull.md, 8). */
	READABLE_OK("symbol");

	READABLE_NO("(a b", HULL_SEXPR_ERR_SYNTAX, 0);
	READABLE_NO("(a (b)", HULL_SEXPR_ERR_SYNTAX, 0);
	READABLE_NO("(a (b c)", HULL_SEXPR_ERR_SYNTAX, 0);
	READABLE_NO("(a (b c", HULL_SEXPR_ERR_SYNTAX, 3);
	READABLE_NO(")", HULL_SEXPR_ERR_SYNTAX, 0);
	READABLE_NO("(a))", HULL_SEXPR_ERR_SYNTAX, 3);
	READABLE_NO("(s \"x)", HULL_SEXPR_ERR_SYNTAX, 3);
	READABLE_NO("(a)\n; no newline", HULL_SEXPR_ERR_SYNTAX, 4);
	READABLE_NO("(x #7b2#)", HULL_SEXPR_ERR_SYNTAX, 4);
	READABLE_NO("(x #7z#)", HULL_SEXPR_ERR_SYNTAX, 5);
	READABLE_NO("(x #7b22)", HULL_SEXPR_ERR_SYNTAX, 3);
	READABLE_NO("(s \"a\\qb\")", HULL_SEXPR_ERR_SYNTAX, 5);
}

/* The situ findings that bear on a reader, each as the walk must see it. */
static void test_situ_findings(void)
{
	unsigned n;

	/* 1: whitespace before `)` is not an element. */
	cases++;
	if ((n = count_children("(a )")) != 1)
		FAIL("(a ) has %u elements, wanted 1", n);
	cases++;
	if ((n = count_children("(machine\n  (backend ns)\n)\n")) != 2)
		FAIL("a list closed on its own line has %u elements, wanted 2", n);

	/* 2: an escaped quote does not end a string. */
	cases++;
	if ((n = count_children("(a \"x\\\"y\")")) != 2)
		FAIL("(a \"x\\\"y\") has %u elements, wanted 2", n);

	/* Comments are not elements, wherever they sit. */
	cases++;
	if ((n = count_children("(a ; one\n b ; two\n)")) != 2)
		FAIL("a list with comments has %u elements, wanted 2", n);
}

static void nest(char *out, unsigned levels)
{
	memset(out, '(', levels);
	memset(out + levels, ')', levels);
	out[2 * levels] = '\0';
}

static void test_depth(void)
{
	char             text[2 * (HULL_SEXPR_MAX_DEPTH + 1) + 1];
	hull_sexpr_doc   doc;
	hull_sexpr_error error;
	hull_sexpr_status got;

	for (int syntax = 0; syntax < 2; syntax++) {
		nest(text, HULL_SEXPR_MAX_DEPTH);
		cases++;
		got = open_text(&doc, text, strlen(text), (hull_sexpr_syntax)syntax, &error);
		if (got != HULL_SEXPR_OK)
			FAIL("%u levels refused in syntax %d: %s", HULL_SEXPR_MAX_DEPTH, syntax,
			     error.reason);
		nest(text, HULL_SEXPR_MAX_DEPTH + 1);
		cases++;
		got = open_text(&doc, text, strlen(text), (hull_sexpr_syntax)syntax, &error);
		if (got != HULL_SEXPR_ERR_DEPTH)
			FAIL("%u levels gave status %d in syntax %d, wanted ERR_DEPTH",
			     HULL_SEXPR_MAX_DEPTH + 1, (int)got, syntax);
	}
}

static void test_canonical(void)
{
	CANONICAL_OK("4:root");
	CANONICAL_OK("0:");
	CANONICAL_OK("()");
	CANONICAL_OK("(7:machine(3:ssh4:true))");
	accept(HULL_SEXPR_CANONICAL, "3:\0()", 5);

	CANONICAL_NO("04:root", HULL_SEXPR_ERR_SYNTAX, 0);
	CANONICAL_NO("5:root", HULL_SEXPR_ERR_SYNTAX, 0);
	CANONICAL_NO("4root", HULL_SEXPR_ERR_SYNTAX, 0);
	CANONICAL_NO(" 4:root", HULL_SEXPR_ERR_SYNTAX, 0);
	CANONICAL_NO("(4:root 3:abc)", HULL_SEXPR_ERR_SYNTAX, 7);
	CANONICAL_NO("(4:root )", HULL_SEXPR_ERR_SYNTAX, 7);
	CANONICAL_NO("(zzz)", HULL_SEXPR_ERR_SYNTAX, 1);
	CANONICAL_NO("(04:root)", HULL_SEXPR_ERR_SYNTAX, 1);
	CANONICAL_NO("(4:root", HULL_SEXPR_ERR_SYNTAX, 0);
	CANONICAL_NO("4:root4:root", HULL_SEXPR_ERR_SYNTAX, 6);
	CANONICAL_NO("(4:root)x", HULL_SEXPR_ERR_SYNTAX, 8);
	CANONICAL_NO("", HULL_SEXPR_ERR_SYNTAX, 0);
	CANONICAL_NO(")", HULL_SEXPR_ERR_SYNTAX, 0);
}

static void test_navigation(void)
{
	static const char text[] =
	    "; the spec\n"
	    "(machine\n"
	    "  (source \"dir:./rootfs\")  ; a directory\n"
	    "  (ports (port (host 8080) (guest 80)))\n"
	    "  (ssh true))\n";
	hull_sexpr_doc   doc;
	hull_sexpr_error error;
	hull_sexpr_form  top, form, source, value;
	uint8_t          buf[64];
	size_t           n;

	cases++;
	if (open_text(&doc, text, sizeof text - 1, HULL_SEXPR_READABLE, &error) != HULL_SEXPR_OK) {
		FAIL("the spec was refused: %s at %u", error.reason, error.offset);
		return;
	}
	cases++;
	if (!hull_sexpr_first(&doc, &top) || top.kind != HULL_SEXPR_LIST)
		FAIL("the first form is not the machine list");
	cases++;
	if (!hull_sexpr_child(&doc, &top, &form) || !hull_sexpr_is(&doc, &form, "machine"))
		FAIL("the list does not open with `machine`");
	cases++;
	if (children(&doc, &top) != 4)
		FAIL("machine has %u elements, wanted 4", children(&doc, &top));
	cases++;
	if (!hull_sexpr_next(&doc, &form, &source) || !hull_sexpr_child(&doc, &source, &form) ||
	    !hull_sexpr_is(&doc, &form, "source") || !hull_sexpr_next(&doc, &form, &value) ||
	    value.kind != HULL_SEXPR_STRING) {
		FAIL("(source \"...\") was not found where it is");
		return;
	}
	cases++;
	if (hull_sexpr_value(&doc, &value, buf, sizeof buf, &n) != HULL_SEXPR_OK ||
	    n != 12 || memcmp(buf, "dir:./rootfs", 12) != 0)
		FAIL("the source's value is not dir:./rootfs");
	cases++;
	if (hull_sexpr_next(&doc, &value, &form))
		FAIL("a form after the source's value, inside its list");
	cases++;
	if (hull_sexpr_next(&doc, &top, &form))
		FAIL("a second top-level form where there is none");
}

static void value_of(const char *text, hull_sexpr_status want_status,
                     const char *want, size_t want_length, size_t capacity)
{
	hull_sexpr_doc    doc;
	hull_sexpr_error  error;
	hull_sexpr_form   top, form;
	uint8_t           buf[64];
	size_t            n;
	hull_sexpr_status got;

	cases++;
	if (open_text(&doc, text, strlen(text), HULL_SEXPR_READABLE, &error) != HULL_SEXPR_OK ||
	    !hull_sexpr_first(&doc, &top) || !hull_sexpr_child(&doc, &top, &form)) {
		FAIL("%s did not open", text);
		return;
	}
	got = hull_sexpr_value(&doc, &form, buf, capacity, &n);
	if (got != want_status || n != want_length ||
	    (got == HULL_SEXPR_OK && memcmp(buf, want, n) != 0))
		FAIL("%s: status %d length %zu, wanted %d and %zu", text, (int)got, n,
		     (int)want_status, want_length);
}

static void test_values(void)
{
	value_of("(name)", HULL_SEXPR_OK, "name", 4, 64);
	value_of("(\"a\\\"b\\\\c\\nd\\te\")", HULL_SEXPR_OK, "a\"b\\c\nd\te", 9, 64);
	value_of("(#7b22FF#)", HULL_SEXPR_OK, "{\"\xff", 3, 64);
	value_of("(##)", HULL_SEXPR_OK, "", 0, 64);
	value_of("(\"abcdef\")", HULL_SEXPR_ERR_SPACE, "", 6, 3);
	value_of("((a))", HULL_SEXPR_ERR_KIND, "", 0, 64);
}

int main(void)
{
	test_readable();
	test_situ_findings();
	test_depth();
	test_canonical();
	test_navigation();
	test_values();

	/* A suite that ran nothing passes exactly as loudly as one that ran
	 * everything, so an empty run is a failure of its own. */
	if (cases == 0) {
		printf("FAIL: no cases ran\n");
		return 1;
	}
	printf("sexpr: %u cases, %u failed\n", cases, failures);
	return failures == 0 ? 0 : 1;
}
