/*
 * plan_test.c -- machines resolved for an engine: the text the plan
 * writes, the projection auto chooses, section 3.9's writeback rule, and
 * the offsets reports carry back to the user's file.
 *
 * The engines are made up so that each choice can be forced both ways.
 * Every case states the whole resolved text, so a default added that
 * should not be fails as surely as one left out.
 */

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "plan.h"

static unsigned cases;
static unsigned failures;

#define FAIL(...) do { \
	failures++; \
	printf("FAIL %s:%d: ", __FILE__, __LINE__); \
	printf(__VA_ARGS__); \
	printf("\n"); \
} while (0)

/* What every made-up engine honours: the defaults themselves. */
#define COMMON \
	{ "source.dir.direct",      HULL_SUP_NATIVE, NULL, NULL }, \
	{ "source.dir.overlay",     HULL_SUP_NATIVE, NULL, NULL }, \
	{ "source.ref.overlay",     HULL_SUP_NATIVE, NULL, NULL }, \
	{ "workdir",                HULL_SUP_NATIVE, NULL, NULL }, \
	{ "hostname",               HULL_SUP_NATIVE, NULL, NULL }, \
	{ "writeback.none",         HULL_SUP_NATIVE, NULL, NULL }, \
	{ "writeback.sync-on-stop", HULL_SUP_NATIVE, NULL, NULL }, \
	{ "on_disconnect.stop",     HULL_SUP_NATIVE, NULL, NULL }, \
	{ "security.no_new_privs",  HULL_SUP_NATIVE, NULL, NULL }, \
	{ "ports",                  HULL_SUP_NATIVE, NULL, NULL }, \
	{ "ssh",                    HULL_SUP_NATIVE, NULL, NULL }, \
	{ "security.readonly_root", HULL_SUP_NATIVE, NULL, NULL }, \
	{ "vm.profile.micro",       HULL_SUP_NATIVE, NULL, NULL }, \
	{ "vm.kernel.bundled",      HULL_SUP_NATIVE, NULL, NULL }, \
	{ "vm.cpu_profile.host",    HULL_SUP_NATIVE, NULL, NULL }, \
	{ "emu.cores_scale",        HULL_SUP_NATIVE, NULL, NULL }

/* ns, bind only. */
static const hull_cap_entry binder_entries[] = {
	COMMON,
	{ "projection.bind", HULL_SUP_NATIVE, NULL, NULL },
	{ NULL, HULL_SUP_NATIVE, NULL, NULL },
};

/* vm and emu, passthrough and block, and passthrough loses inotify. */
static const hull_cap_entry both_entries[] = {
	COMMON,
	{ "projection.passthrough", HULL_SUP_NATIVE, NULL, NULL },
	{ "projection.block",       HULL_SUP_NATIVE, NULL, NULL },
	{ "fidelity.inotify",       HULL_SUP_NATIVE, NULL, NULL },
	{ NULL, HULL_SUP_NATIVE, NULL, NULL },
};

/* vm, block only, like virtualbox. */
static const hull_cap_entry blocky_entries[] = {
	COMMON,
	{ "projection.block", HULL_SUP_NATIVE, NULL, NULL },
	{ NULL, HULL_SUP_NATIVE, NULL, NULL },
};

/* No projection at all. */
static const hull_cap_entry bare_entries[] = {
	COMMON,
	{ NULL, HULL_SUP_NATIVE, NULL, NULL },
};

static const char *const ns_only[] = { "ns", NULL };
static const char *const vm_emu[]  = { "vm", "emu", NULL };
static const char *const vm_only[] = { "vm", NULL };

#define ENGINE(name, var, list, table) \
	static const hull_capabilities var##_caps = { .struct_size = sizeof (hull_capabilities), \
		.engine_name = name, .classes = list, .entries = table }; \
	static const hull_engine var = { .struct_size = sizeof (hull_engine), \
		.abi_major = HULL_ABI_MAJOR, .caps = &var##_caps }

ENGINE("binder", binder, ns_only, binder_entries);
ENGINE("both", both, vm_emu, both_entries);
ENGINE("blocky", blocky, vm_only, blocky_entries);
ENGINE("bare", bare, vm_only, bare_entries);

typedef struct seen {
	char   text[2048];
	size_t length;
} seen;

/* `id:letter@offset`, in order. */
static void collect(void *context, const hull_resolution *resolution)
{
	static const char letters[] = "neari";
	seen             *s         = context;
	int               n;

	n = snprintf(s->text + s->length, sizeof s->text - s->length, "%s%s:%c@%u",
	             s->length == 0u ? "" : " ", resolution->id, letters[resolution->outcome],
	             resolution->offset);
	if (n > 0 && (size_t)n < sizeof s->text - s->length)
		s->length += (size_t)n;
}

static hull_plan plan;

/* Plan the file's machine, or its machine named `name`. */
static hull_plan_status run(int line, const char *text, const char *name,
                            const hull_engine *engine, const char *class_name, seen *s)
{
	static hull_spec spec;
	hull_spec_error  error;
	hull_choice      choice;
	hull_sexpr_form  machine;

	memset(s, 0, sizeof *s);
	if (hull_spec_open(&spec, (const uint8_t *)text, strlen(text), &error) != HULL_SPEC_OK) {
		failures++;
		printf("FAIL %s:%d: the spec is refused: %s %s\n", __FILE__, line, error.path,
		       error.reason);
		return HULL_PLAN_ERR_INTERNAL;
	}
	if (name != NULL)
		hull_spec_find(&spec, &spec.machines, name, &machine);
	else
		machine = spec.machine;
	choice.engine     = engine;
	choice.class_name = class_name;
	return hull_plan_build(&plan, &spec, &machine, name, &choice, "amd64", NULL, collect, s);
}

/* The resolved text, whole, and the status. */
static void expect(int line, const char *text, const char *name, const hull_engine *engine,
                   const char *class_name, hull_plan_status want, const char *want_text)
{
	seen             s;
	hull_plan_status got;

	cases++;
	got = run(line, text, name, engine, class_name, &s);
	if (got != want || plan.length != strlen(want_text) ||
	    memcmp(plan.text, want_text, plan.length) != 0) {
		failures++;
		printf("FAIL %s:%d: status %d, wanted %d\n  got    %.*s  wanted %s  [%s]\n",
		       __FILE__, line, (int)got, (int)want, (int)plan.length, plan.text, want_text,
		       s.text);
	}
}

/* The reports, whole, and the status. */
static void expect_reports(int line, const char *text, const hull_engine *engine,
                           const char *class_name, hull_plan_status want, const char *want_text)
{
	seen             s;
	hull_plan_status got;

	cases++;
	got = run(line, text, NULL, engine, class_name, &s);
	if (got != want || strcmp(s.text, want_text) != 0) {
		failures++;
		printf("FAIL %s:%d: status %d, wanted %d\n  got    [%s]\n  wanted [%s]\n",
		       __FILE__, line, (int)got, (int)want, s.text, want_text);
	}
}

#define OK(text, engine, class_name, want) \
	expect(__LINE__, text, NULL, engine, class_name, HULL_PLAN_OK, want)
#define NO(text, engine, class_name, want) \
	expect(__LINE__, text, NULL, engine, class_name, HULL_PLAN_REJECTED, want)

/* What every resolved machine below ends with on ns. */
#define NS_TAIL " (security (readonly_root false) (no_new_privs true))" \
                " (tuning (ns (binfmt false))) (projection bind))\n"
#define VM_TUNING " (tuning (vm (profile micro) (kernel bundled) (cpu_profile host)))"

static void test_defaults(void)
{
	/* A dir source is direct and gets "/"; a ref is overlay and gets no
	 * workdir, which is its manifest's to give. */
	OK("(machine (source \"dir:.\"))", &binder, "ns",
	   "(machine (source \"dir:.\") (mutable direct) (writeback none) (ssh false)"
	   " (on_disconnect stop) (workdir \"/\")" NS_TAIL);
	OK("(machine (source \"debian:13\"))", &binder, "ns",
	   "(machine (source \"debian:13\") (mutable overlay) (writeback none) (ssh false)"
	   " (on_disconnect stop)" NS_TAIL);

	/* What was written is kept as written, comments and all. */
	OK("(machine (ssh true) ; mine\n (source \"dir:.\") (workdir \"/srv\")"
	   " (mutable overlay) (on_disconnect stop) (writeback none))", &binder, "ns",
	   "(machine (ssh true) (source \"dir:.\") (workdir \"/srv\") (mutable overlay)"
	   " (on_disconnect stop) (writeback none)" NS_TAIL);

	/* A record that was written gains only what it left out. */
	OK("(machine (source \"debian:13\") (security (readonly_root true))"
	   " (ports (port (host 80) (guest 80)) (port (host 53) (guest 53) (proto udp))))",
	   &binder, "ns",
	   "(machine (source \"debian:13\") (security (readonly_root true) (no_new_privs true))"
	   " (ports (port (host 80) (guest 80) (proto tcp)) (port (host 53) (guest 53)"
	   " (proto udp))) (mutable overlay) (writeback none) (ssh false) (on_disconnect stop)"
	   " (tuning (ns (binfmt false))) (projection bind))\n");

	/* A machine of a (machines ...) file is named for its hostname. */
	expect(__LINE__, "(machines (web (source \"debian:13\")))", "web", &binder, "ns",
	       HULL_PLAN_OK,
	       "(machine (source \"debian:13\") (mutable overlay) (writeback none) (ssh false)"
	       " (on_disconnect stop) (hostname \"web\")" NS_TAIL);
	expect(__LINE__, "(machines (web (source \"debian:13\") (hostname \"www\")))", "web",
	       &binder, "ns", HULL_PLAN_OK,
	       "(machine (source \"debian:13\") (hostname \"www\") (mutable overlay)"
	       " (writeback none) (ssh false) (on_disconnect stop)" NS_TAIL);
}

static void test_tuning(void)
{
	/* The sections that apply are written in whole; others stay as they
	 * were, for the matrix to report ignored. */
	OK("(machine (source \"debian:13\") (tuning (vm (profile micro)) (virtualbox (gui true))))",
	   &both, "vm",
	   "(machine (source \"debian:13\") (tuning (vm (profile micro) (kernel bundled)"
	   " (cpu_profile host)) (virtualbox (gui true))) (mutable overlay) (writeback none)"
	   " (ssh false) (on_disconnect stop) (security (readonly_root false)"
	   " (no_new_privs true)) (projection passthrough))\n");

	/* A section for another engine gains nothing, even a key it lacks. */
	OK("(machine (source \"debian:13\") (tuning (virtualbox)))", &both, "vm",
	   "(machine (source \"debian:13\") (tuning (virtualbox) (vm (profile micro)"
	   " (kernel bundled) (cpu_profile host))) (mutable overlay) (writeback none)"
	   " (ssh false) (on_disconnect stop) (security (readonly_root false)"
	   " (no_new_privs true)) (projection passthrough))\n");

	/* emu adds to vm's tuning, and gets both. */
	OK("(machine (source \"debian:13\"))", &both, "emu",
	   "(machine (source \"debian:13\") (mutable overlay) (writeback none) (ssh false)"
	   " (on_disconnect stop) (security (readonly_root false) (no_new_privs true))"
	   " (tuning (vm (profile micro) (kernel bundled) (cpu_profile host))"
	   " (emu (cores_scale 1))) (projection passthrough))\n");
}

static void test_projection(void)
{
	/* auto: the first that leaves nothing rejected. */
	OK("(machine (source \"debian:13\") (projection auto))", &blocky, "vm",
	   "(machine (source \"debian:13\") (mutable overlay) (writeback none) (ssh false)"
	   " (on_disconnect stop) (security (readonly_root false) (no_new_privs true))"
	   VM_TUNING " (projection block))\n");
	/* A fidelity need is not judged per projection yet (section 4.7), so
	 * it does not move auto off passthrough. */
	OK("(machine (source \"debian:13\") (fidelity inotify))", &both, "vm",
	   "(machine (source \"debian:13\") (fidelity inotify) (mutable overlay) (writeback none)"
	   " (ssh false) (on_disconnect stop) (security (readonly_root false)"
	   " (no_new_privs true))" VM_TUNING " (projection passthrough))\n");
	/* A projection written by name is not second-guessed. */
	NO("(machine (source \"debian:13\") (projection bind))", &both, "vm",
	   "(machine (source \"debian:13\") (mutable overlay) (writeback none) (ssh false)"
	   " (on_disconnect stop) (security (readonly_root false) (no_new_privs true))"
	   VM_TUNING " (projection bind))\n");
	/* Nothing declared: bind, so the report says so. */
	NO("(machine (source \"debian:13\"))", &bare, "vm",
	   "(machine (source \"debian:13\") (mutable overlay) (writeback none) (ssh false)"
	   " (on_disconnect stop) (security (readonly_root false) (no_new_privs true))"
	   VM_TUNING " (projection bind))\n");
}

static void test_writeback(void)
{
	/* Section 3.9, rule 2: a dir written in place under block goes stale
	 * unless it is synced back. auto falls to block on blocky, so the
	 * rule decides whether the plan stands. */
	expect_reports(__LINE__, "(machine (source \"dir:.\"))", &blocky, "vm",
	               HULL_PLAN_REJECTED,
	               "source.dir.direct:n@9 writeback.none:n@0 on_disconnect.stop:n@0 workdir:n@0"
	               " security.no_new_privs:n@0 vm.profile.micro:n@0 vm.kernel.bundled:n@0"
	               " vm.cpu_profile.host:n@0 projection.block:n@0 writeback:r@9");
	expect_reports(__LINE__, "(machine (source \"dir:.\") (writeback sync-on-stop))", &blocky,
	               "vm", HULL_PLAN_OK,
	               "source.dir.direct:n@9 writeback.sync-on-stop:n@26 on_disconnect.stop:n@0"
	               " workdir:n@0 security.no_new_privs:n@0 vm.profile.micro:n@0"
	               " vm.kernel.bundled:n@0 vm.cpu_profile.host:n@0 projection.block:n@0");
	expect_reports(__LINE__, "(machine (source \"dir:.\") (mutable overlay))", &blocky, "vm",
	               HULL_PLAN_OK,
	               "source.dir.overlay:n@9 writeback.none:n@0 on_disconnect.stop:n@0 workdir:n@0"
	               " security.no_new_privs:n@0 vm.profile.micro:n@0 vm.kernel.bundled:n@0"
	               " vm.cpu_profile.host:n@0 projection.block:n@0");
	/* passthrough writes in place, so the rule does not reach it. */
	expect_reports(__LINE__, "(machine (source \"dir:.\"))", &both, "vm", HULL_PLAN_OK,
	               "source.dir.direct:n@9 writeback.none:n@0 on_disconnect.stop:n@0 workdir:n@0"
	               " security.no_new_privs:n@0 vm.profile.micro:n@0 vm.kernel.bundled:n@0"
	               " vm.cpu_profile.host:n@0 projection.passthrough:n@0");
}

static void test_offsets(void)
{
	/* Inside a copied entry every byte maps to its own; an added default
	 * maps to the entry it was added for. */
	static const char text[] = "(machine\n  (source \"dir:.\")\n  (ports (port (host 80)"
	                           " (guest 80))))";
	seen              s;
	const char       *proto;

	cases++;
	run(__LINE__, text, NULL, &binder, "ns", &s);
	proto = strstr(plan.text, "(proto tcp)");
	if (proto == NULL ||
	    hull_plan_source(&plan, (uint32_t)(strstr(plan.text, "(host 80)") - plan.text)) !=
	    (uint32_t)(strstr(text, "(host 80)") - text) ||
	    hull_plan_source(&plan, (uint32_t)(strstr(plan.text, "80) (guest") - plan.text)) !=
	    (uint32_t)(strstr(text, "80) (guest") - text) ||
	    hull_plan_source(&plan, (uint32_t)(proto - plan.text)) !=
	    (uint32_t)(strstr(text, "(port (") - text) ||
	    hull_plan_source(&plan, (uint32_t)plan.length + 5u) != 0u)
		FAIL("offsets: %.*s", (int)plan.length, plan.text);

	/* A machine too large for the plan is refused, not cut. */
	cases++;
	{
		static char big[HULL_PLAN_TEXT + 64u];
		size_t      n = (size_t)snprintf(big, sizeof big, "(machine (source \"dir:.\") (env (A \"");

		memset(big + n, 'x', HULL_PLAN_TEXT);
		n += HULL_PLAN_TEXT;
		snprintf(big + n, sizeof big - n, "\")))");
		if (run(__LINE__, big, NULL, &binder, "ns", &s) != HULL_PLAN_ERR_SIZE)
			FAIL("a machine larger than the plan was not refused");
	}

	/* A class the engine lacks. */
	cases++;
	if (run(__LINE__, "(machine (source \"dir:.\"))", NULL, &binder, "vm", &s) !=
	    HULL_PLAN_ERR_CLASS)
		FAIL("vm on an ns engine");
}

/* What crosses the ABI: the machine in canonical form, sorted, with its
 * class, and the same bytes however the user spelled it. */
static void test_canonical(void)
{
	static const char want[] =
	        "(4:plan(5:class2:ns)(7:machine(7:mutable6:direct)(13:on_disconnect4:stop)"
	        "(10:projection4:bind)(8:security(12:no_new_privs4:true)(13:readonly_root5:false))"
	        "(6:source5:dir:.)(3:ssh5:false)(6:tuning(2:ns(6:binfmt5:false)))(7:workdir1:/)"
	        "(9:writeback4:none)))";
	static const char *const spellings[] = {
		"(machine (source \"dir:.\"))",
		"; the same machine\n(machine (ssh false)\n  (source \"dir:.\") (projection auto)"
		" (security (no_new_privs true)))\n",
	};
	seen s;

	for (unsigned i = 0u; i < sizeof spellings / sizeof spellings[0]; i++) {
		cases++;
		if (run(__LINE__, spellings[i], NULL, &binder, "ns", &s) != HULL_PLAN_OK ||
		    plan.canonical_length != sizeof want - 1u ||
		    memcmp(plan.canonical, want, sizeof want - 1u) != 0)
			FAIL("spelling %u: %.*s", i, (int)plan.canonical_length,
			     (const char *)plan.canonical);
	}

	/* The class is the one it runs as. */
	cases++;
	run(__LINE__, "(machine (source \"debian:13\"))", NULL, &both, "emu", &s);
	if (memcmp(plan.canonical, "(4:plan(5:class3:emu)(7:machine", 31) != 0)
		FAIL("emu: %.*s", (int)plan.canonical_length, (const char *)plan.canonical);
}

int main(void)
{
	alarm(60);
	test_defaults();
	test_tuning();
	test_projection();
	test_writeback();
	test_offsets();
	test_canonical();
	printf("plan: %u cases, %u failed\n", cases, failures);
	return failures == 0u ? 0 : 1;
}
