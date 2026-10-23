/*
 * matrix_test.c -- machines resolved against two engine columns.
 *
 * The columns are made up for the test, shaped like section 5.0's: a
 * qemu-like engine implementing vm and emu, and a docker-like one
 * implementing ns that refuses a dir source written in place. Each case
 * states every field it expects reported, in order, as `id:outcome` --
 * n, e, a, r or i -- so a field reported that should not be fails as
 * surely as one missing.
 */

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "matrix.h"

static unsigned cases;
static unsigned failures;

#define FAIL(...) do { \
	failures++; \
	printf("FAIL %s:%d: ", __FILE__, __LINE__); \
	printf(__VA_ARGS__); \
	printf("\n"); \
} while (0)

static const hull_cap_entry qemu_entries[] = {
	{ "source.dir.direct",      HULL_SUP_NATIVE,   NULL, NULL },
	{ "source.ref.overlay",     HULL_SUP_NATIVE,   NULL, NULL },
	{ "command",                HULL_SUP_NATIVE,   NULL, NULL },
	{ "env",                    HULL_SUP_NATIVE,   NULL, NULL },
	{ "ports",                  HULL_SUP_EMULATED, "host forwarder", NULL },
	{ "volumes",                HULL_SUP_NATIVE,   NULL, NULL },
	{ "cpus",                   HULL_SUP_NATIVE,   NULL, NULL },
	{ "memory",                 HULL_SUP_NATIVE,   NULL, NULL },
	{ "ssh",                    HULL_SUP_NATIVE,   NULL, NULL },
	{ "media",                  HULL_SUP_NATIVE,   NULL, NULL },
	{ "projection.block",       HULL_SUP_REJECTED, "block projection writes nothing back", NULL },
	{ "security.readonly_root", HULL_SUP_NATIVE,   NULL, NULL },
	{ "fidelity.inotify",       HULL_SUP_REJECTED, "virtiofs misses events", "vm:inotify-poll" },
	{ "vm.profile.micro",       HULL_SUP_NATIVE,   NULL, NULL },
	{ "vm.kernel.bundled",      HULL_SUP_NATIVE,   NULL, NULL },
	{ "vm.kernel.path",         HULL_SUP_NATIVE,   NULL, NULL },
	{ "qemu.accel.tcg",         HULL_SUP_NATIVE,   NULL, NULL },
	{ "emu.cores_scale",        HULL_SUP_NATIVE,   NULL, NULL },
	{ NULL,                     HULL_SUP_NATIVE,   NULL, NULL },
};

static const char *const qemu_classes[] = { "vm", "emu", NULL };
static const char *const qemu_media[]   = { "cdrom", "floppy", NULL };
static const char *const qemu_arches[]  = { "amd64", "arm64", NULL };

static const hull_capabilities qemu = {
	.struct_size  = sizeof (hull_capabilities),
	.engine_name  = "qemu",
	.classes      = qemu_classes,
	.entries      = qemu_entries,
	.media_types  = qemu_media,
	.arches       = qemu_arches,
};

static const hull_cap_entry docker_entries[] = {
	{ "source.dir.direct",  HULL_SUP_REJECTED, "docker cannot run a directory in place", NULL },
	{ "source.dir.overlay", HULL_SUP_EMULATED, "a cached docker import of the tree", NULL },
	{ "source.ref.overlay", HULL_SUP_NATIVE,   NULL, NULL },
	{ "command",            HULL_SUP_NATIVE,   NULL, NULL },
	{ "ports",              HULL_SUP_NATIVE,   NULL, NULL },
	{ NULL,                 HULL_SUP_NATIVE,   NULL, NULL },
};

static const char *const docker_classes[] = { "ns", NULL };

/* No media types and no arches: host-only, and no media at all. */
static const hull_capabilities docker = {
	.struct_size  = sizeof (hull_capabilities),
	.engine_name  = "docker",
	.classes      = docker_classes,
	.entries      = docker_entries,
};

typedef struct seen {
	char     text[1024];
	size_t   length;
	char     token[64];
	uint32_t first_rejected;
	int      rejected;
} seen;

static void collect(void *context, const hull_resolution *resolution)
{
	static const char letters[] = "neari";
	seen             *s         = context;
	int               n;

	n = snprintf(s->text + s->length, sizeof s->text - s->length, "%s%s:%c",
	             s->length == 0u ? "" : " ", resolution->id, letters[resolution->outcome]);
	if (n > 0 && (size_t)n < sizeof s->text - s->length)
		s->length += (size_t)n;
	if (resolution->token != NULL)
		snprintf(s->token, sizeof s->token, "%s", resolution->token);
	if (resolution->outcome == HULL_OUTCOME_REJECTED && !s->rejected) {
		s->first_rejected = resolution->offset;
		s->rejected = 1;
	}
}

static void expect(int line, const char *text, const hull_capabilities *caps,
                   const char *class_name, const char *const *accepted,
                   hull_matrix_status want, const char *want_text)
{
	hull_spec          spec;
	hull_spec_error    error;
	hull_matrix_status got;
	seen               s;

	cases++;
	memset(&s, 0, sizeof s);
	if (hull_spec_open(&spec, (const uint8_t *)text, strlen(text), &error) != HULL_SPEC_OK ||
	    !spec.has_machine) {
		failures++;
		printf("FAIL %s:%d: the spec is refused: %s, %s\n", __FILE__, line, error.path,
		       error.reason);
		return;
	}
	got = hull_matrix_resolve(&spec, &spec.machine, caps, class_name, "amd64", accepted,
	                          collect, &s);
	if (got != want || strcmp(s.text, want_text) != 0) {
		failures++;
		printf("FAIL %s:%d: status %d, [%s]\n     wanted %d, [%s]\n", __FILE__, line,
		       (int)got, s.text, (int)want, want_text);
	}
}

#define OK(text, caps, class_name, want) \
	expect(__LINE__, text, caps, class_name, NULL, HULL_MATRIX_OK, want)
#define NO(text, caps, class_name, want) \
	expect(__LINE__, text, caps, class_name, NULL, HULL_MATRIX_REJECTED, want)

static void test_ids(void)
{
	/* The source carries its scheme and its mutability, written or not. */
	OK("(machine (source \"dir:./rootfs\"))", &qemu, "vm", "source.dir.direct:n");
	OK("(machine (source \"debian:13\"))", &qemu, "vm", "source.ref.overlay:n");
	NO("(machine (source \"dir:.\"))", &docker, "ns", "source.dir.direct:r");
	OK("(machine (source \"dir:.\") (mutable overlay))", &docker, "ns", "source.dir.overlay:e");
	NO("(machine (mutable overlay) (source \"tar:./a.tar\"))", &docker, "ns",
	   "source.tar.overlay:r");

	/* A key by its name; a chosen value in the id; false and empty ask
	 * nothing; a number is not a choice. */
	OK("(machine (source \"dir:.\") (command \"x\") (cpus 2) (memory \"2G\") (ssh true))",
	   &qemu, "vm", "source.dir.direct:n command:n cpus:n memory:n ssh:n");
	OK("(machine (source \"dir:.\") (ssh false) (ports) (env) (fidelity) (security (allow)))",
	   &qemu, "vm", "source.dir.direct:n");
	NO("(machine (source \"dir:.\") (projection block))", &qemu, "vm",
	   "source.dir.direct:n projection.block:r");
	NO("(machine (source \"dir:.\") (projection bind))", &qemu, "vm",
	   "source.dir.direct:n projection.bind:r");
	OK("(machine (source \"dir:.\") (ports (port (host 80) (guest 80)))"
	   " (env (A \"b\")) (volumes (volume (host \"a\") (guest \"/b\"))))", &qemu, "vm",
	   "source.dir.direct:n ports:e env:n volumes:n");
	/* A volume that is a stack asks for that once, wherever it is. */
	NO("(machine (source \"dir:.\") (volumes (volume (host \"a\") (guest \"/b\"))"
	   " (volume (guest \"/c\") (layers \"dir:./t\")) (volume (guest \"/d\") (layers \"dir:./u\"))))",
	   &qemu, "vm", "source.dir.direct:n volumes:n volumes.layers:r");
	NO("(machine (source \"dir:.\") (security (readonly_root true) (no_new_privs false)"
	   " (allow net_admin)))", &qemu, "vm",
	   "source.dir.direct:n security.readonly_root:n security.allow:r");
	NO("(machine (source \"dir:.\") (security (allow net_admin sys_ptrace)))", &qemu, "vm",
	   "source.dir.direct:n security.allow:r");

	/* Hull's own keys are reported, and never asked of the engine. */
	OK("(machine (source \"dir:.\") (backend qemu) (restart always) (env_file \".env\"))",
	   &qemu, "vm", "source.dir.direct:n backend:n restart:n env_file:n");

	/* Each fidelity need is its own field. */
	NO("(machine (source \"dir:.\") (fidelity inotify flock))", &qemu, "vm",
	   "source.dir.direct:n fidelity.inotify:r fidelity.flock:r");
}

static void test_lists(void)
{
	/* Arches: the host's asks nothing, by either name; another is emu's
	 * alone, and then must be listed. */
	OK("(machine (source \"debian:13\") (arch host))", &docker, "ns", "source.ref.overlay:n");
	OK("(machine (source \"debian:13\") (arch amd64))", &docker, "ns", "source.ref.overlay:n");
	OK("(machine (source \"debian:13\") (arch amd64))", &qemu, "vm", "source.ref.overlay:n");
	NO("(machine (source \"debian:13\") (arch arm64))", &qemu, "vm",
	   "source.ref.overlay:n arch:r");
	OK("(machine (source \"debian:13\") (arch arm64))", &qemu, "emu",
	   "source.ref.overlay:n arch:n");
	NO("(machine (source \"debian:13\") (arch riscv64))", &qemu, "emu",
	   "source.ref.overlay:n arch:r");
	NO("(machine (source \"debian:13\") (arch arm64))", &docker, "ns",
	   "source.ref.overlay:n arch:r");

	/* Media: the key, then each type against media_types. */
	OK("(machine (source \"dir:.\") (media (medium (type cdrom) (source \"dir:./cd\"))))",
	   &qemu, "vm", "source.dir.direct:n media:n");
	NO("(machine (source \"dir:.\") (media (medium (type cdrom) (source \"dir:./cd\"))"
	   " (medium (type tape) (source \"dir:./t\"))))", &qemu, "vm",
	   "source.dir.direct:n media:n media:r");
	NO("(machine (source \"debian:13\") (media (medium (type cdrom) (source \"dir:./cd\"))))",
	   &docker, "ns", "source.ref.overlay:n media:r media:r");
}

static void test_tuning(void)
{
	/* The class's entry and the engine's apply; the rest are ignored. */
	OK("(machine (source \"dir:.\") (tuning (vm (profile micro) (kernel bundled))"
	   " (qemu (accel tcg)) (virtualbox (gui true)) (ns (binfmt true))))", &qemu, "vm",
	   "source.dir.direct:n vm.profile.micro:n vm.kernel.bundled:n qemu.accel.tcg:n"
	   " virtualbox:i ns:i");
	OK("(machine (source \"dir:.\") (tuning (vm (kernel \"path:./bzImage\"))"
	   " (emu (cores_scale 2))))", &qemu, "emu",
	   "source.dir.direct:n vm.kernel.path:n emu.cores_scale:n");
	NO("(machine (source \"dir:.\") (tuning (emu (cores_scale 2)) (vm (profile full))))",
	   &qemu, "emu", "source.dir.direct:n emu.cores_scale:n vm.profile.full:r");
	OK("(machine (source \"dir:.\") (tuning (emu (cores_scale 2))))", &qemu, "vm",
	   "source.dir.direct:n emu:i");
	NO("(machine (source \"dir:.\") (tuning (vm (profile full))))", &qemu, "vm",
	   "source.dir.direct:n vm.profile.full:r");
	OK("(machine (source \"dir:.\") (tuning (vm (kernel \"path:./bzImage\"))))", &qemu, "vm",
	   "source.dir.direct:n vm.kernel.path:n");
	/* An empty section asks nothing; false asks nothing. */
	OK("(machine (source \"dir:.\") (tuning (vm) (ns (binfmt false))))", &qemu, "vm",
	   "source.dir.direct:n ns:i");
}

static void test_accept(void)
{
	static const char *const tokens[] = { "vm:inotify-poll", NULL };
	static const char *const others[] = { "vm:something-else", NULL };
	static const char *const empty[]  = { "", NULL };
	static const char        text[]   = "(machine (source \"dir:.\") (fidelity inotify))";
	hull_spec                spec;
	hull_spec_error          error;
	seen                     s;

	/* A rejection with a token is honoured only when the token is given. */
	expect(__LINE__, text, &qemu, "vm", tokens, HULL_MATRIX_OK,
	       "source.dir.direct:n fidelity.inotify:a");
	expect(__LINE__, text, &qemu, "vm", others, HULL_MATRIX_REJECTED,
	       "source.dir.direct:n fidelity.inotify:r");

	/* The token is reported with the rejection, so the user is told it,
	 * and the offset is the need's own. */
	cases++;
	memset(&s, 0, sizeof s);
	hull_spec_open(&spec, (const uint8_t *)text, sizeof text - 1u, &error);
	hull_matrix_resolve(&spec, &spec.machine, &qemu, "vm", "amd64", NULL, collect, &s);
	if (strcmp(s.token, "vm:inotify-poll") != 0 ||
	    s.first_rejected != (uint32_t)(strstr(text, "inotify") - text))
		FAIL("token `%s` at %u", s.token, s.first_rejected);

	/* A token cannot honour a rejection that names none. */
	expect(__LINE__, "(machine (source \"dir:.\") (projection block))", &qemu, "vm", empty,
	       HULL_MATRIX_REJECTED, "source.dir.direct:n projection.block:r");

	/* A class the engine does not implement is refused before anything. */
	cases++;
	memset(&s, 0, sizeof s);
	if (hull_matrix_resolve(&spec, &spec.machine, &qemu, "ns", "amd64", NULL, collect, &s) !=
	    HULL_MATRIX_ERR_CLASS || s.length != 0u)
		FAIL("ns on qemu");
}

/* A named machine of (machines ...) resolves as (machine ...) does. */
static void test_machines(void)
{
	static const char text[] = "(machines (web (source \"dir:.\") (ports (port (host 80)"
	                           " (guest 80)))) (db (source \"debian:13\")))";
	hull_spec         spec;
	hull_spec_error   error;
	hull_sexpr_form   web;
	seen              s;

	cases++;
	memset(&s, 0, sizeof s);
	if (hull_spec_open(&spec, (const uint8_t *)text, sizeof text - 1u, &error) != HULL_SPEC_OK ||
	    !hull_spec_find(&spec, &spec.machines, "web", &web) ||
	    hull_matrix_resolve(&spec, &web, &qemu, "vm", "amd64", NULL, collect, &s) != HULL_MATRIX_OK ||
	    strcmp(s.text, "source.dir.direct:n ports:e") != 0)
		FAIL("[%s]", s.text);
}

int main(void)
{
	alarm(60);
	test_ids();
	test_lists();
	test_tuning();
	test_accept();
	test_machines();
	printf("matrix: %u cases, %u failed\n", cases, failures);
	return failures == 0u ? 0 : 1;
}
