/*
 * spec_test.c -- hull.conf held to Appendix J.
 *
 * Every refusal is checked for its status, its offset and its path, so a
 * refusal of the right file for the wrong reason fails. The examples in
 * project.md are read from it, given as the first argument: each
 * ```scheme block is a whole file and must open, so the document cannot
 * show a hull.conf the parser refuses.
 */

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "spec.h"

static unsigned cases;
static unsigned failures;

#define FAIL(...) do { \
	failures++; \
	printf("FAIL %s:%d: ", __FILE__, __LINE__); \
	printf(__VA_ARGS__); \
	printf("\n"); \
} while (0)

static void accept(int line, const char *text, size_t length)
{
	hull_spec       spec;
	hull_spec_error error;

	cases++;
	if (hull_spec_open(&spec, (const uint8_t *)text, length, &error) != HULL_SPEC_OK) {
		failures++;
		printf("FAIL %s:%d: refused at %u, %s: %s\n", __FILE__, line, error.offset,
		       error.path, error.reason);
	}
}

/* Refused with `want`, at `offset`, naming `path`. */
static void refuse(int line, const char *text, size_t length, hull_spec_status want,
                   uint32_t offset, const char *path)
{
	hull_spec        spec;
	hull_spec_error  error;
	hull_spec_status got;

	cases++;
	got = hull_spec_open(&spec, (const uint8_t *)text, length, &error);
	if (got != want || error.offset != offset || strcmp(error.path, path) != 0) {
		failures++;
		printf("FAIL %s:%d: status %d at %u, %s (%s); wanted %d at %u, %s\n",
		       __FILE__, line, (int)got, error.offset, error.path,
		       error.reason != NULL ? error.reason : "no reason", (int)want, offset, path);
	}
}

/* Refused at the first place `at` appears in `text`. */
static void refuse_at(int line, const char *text, size_t length, hull_spec_status want,
                      const char *at, const char *path)
{
	const char *where = strstr(text, at);

	if (where == NULL) {
		cases++;
		failures++;
		printf("FAIL %s:%d: `%s` is not in the case\n", __FILE__, line, at);
		return;
	}
	refuse(line, text, length, want, (uint32_t)(where - text), path);
}

#define OK(text) accept(__LINE__, text, sizeof text - 1)
#define NO(text, status, at, path) \
	refuse_at(__LINE__, text, sizeof text - 1, HULL_SPEC_ERR_##status, at, path)

/* Most cases need a machine around them; this one has its source. */
#define M(rest) "(machine (source \"dir:.\")" rest ")"

static void test_file(void)
{
	OK("(machine (source \"dir:./rootfs\"))");
	OK("; a comment\n(machine\n  (source \"dir:.\")  ; and another\n)\n");
	OK("(build (base \"dir:.\"))");
	OK("(machine (source \"a\")) (build (base \"b\"))");
	OK("(machines (web (source \"a\")) (db (source \"b\")))");

	NO("", MISSING, "", "");
	NO("; nothing but a comment\n", MISSING, "", "");
	refuse(__LINE__, "(machine", 8u, HULL_SPEC_ERR_SYNTAX, 0u, "");
	NO("machine", SHAPE, "machine", "");
	NO("(volume)", UNKNOWN, "volume", "volume");
	NO("(machine (source \"a\")) (machine (source \"b\"))", DUPLICATE,
	   "(machine (source \"b\"", "machine");
	NO("(machine (source \"a\")) (machines (web (source \"b\")))", SHAPE,
	   "(machines", "machines");
	NO("(machines)", SHAPE, "(machines", "machines");
}

static void test_values(void)
{
	/* The key's own shape. */
	NO("(machine (source \"a\") (colour red))", UNKNOWN, "colour", "machine.colour");
	NO("(machine (source \"a\") (source \"b\"))", DUPLICATE, "(source \"b\")",
	   "machine.source");
	NO("(machine (ssh true))", MISSING, "(machine", "machine.source");
	NO("(machine (source \"a\") ssh)", SHAPE, "ssh", "machine");
	NO("(machine (source \"a\") ())", SHAPE, "()", "machine");
	NO("(machine (source \"a\") (\"ssh\" true))", SHAPE, "(\"ssh\"", "machine");

	/* One string. */
	NO("(machine (source dir))", SHAPE, "dir", "machine.source");
	NO("(machine (source \"a\" \"b\"))", SHAPE, "\"b\"", "machine.source");
	NO("(machine (source))", SHAPE, "(source)", "machine.source");
	NO("(machine (source \"\"))", VALUE, "\"\"", "machine.source");
	OK(M("(workdir \"/app\")"));
	NO(M("(workdir \"app\")"), VALUE, "\"app\"", "machine.workdir");
	OK(M("(hostname \"web-1\")"));
	NO(M("(hostname \"Web\")"), VALUE, "\"Web\"", "machine.hostname");
	NO(M("(hostname \"-web\")"), VALUE, "\"-web\"", "machine.hostname");

	/* Strings. */
	OK(M("(command \"./server\" \"--port\" \"80\" \"\")"));
	NO(M("(command)"), SHAPE, "(command)", "machine.command");
	NO(M("(command \"a\" b)"), SHAPE, "b)", "machine.command");

	/* Symbols, from an open set and from a closed one. */
	OK(M("(arch arm64) (backend qemu) (fidelity inotify flock)"));
	OK(M("(fidelity)"));
	NO(M("(arch Arm64)"), VALUE, "Arm64", "machine.arch");
	NO(M("(arch \"arm64\")"), SHAPE, "\"arm64\"", "machine.arch");
	OK(M("(security (allow CAP_NET_ADMIN 0000:01:00.0))"));
	OK(M("(mutable overlay) (restart on-failure) (on_disconnect keep)"));
	NO(M("(mutable sometimes)"), VALUE, "sometimes", "machine.mutable");
	NO(M("(restart)"), SHAPE, "(restart)", "machine.restart");

	/* Booleans. */
	OK(M("(ssh false)"));
	NO(M("(ssh yes)"), VALUE, "yes", "machine.ssh");
	NO(M("(ssh true false)"), SHAPE, "false", "machine.ssh");

	/* Numbers. */
	OK(M("(cpus 1)"));
	OK(M("(cpus 4294967295)"));
	NO(M("(cpus 4294967296)"), VALUE, "4294967296", "machine.cpus");
	NO(M("(cpus 0)"), VALUE, "0)", "machine.cpus");
	NO(M("(cpus 02)"), VALUE, "02", "machine.cpus");
	NO(M("(cpus -2)"), VALUE, "-2", "machine.cpus");
	OK(M("(tuning (emu (cores_scale 1.5)))"));
	OK(M("(tuning (emu (cores_scale 0.5)))"));
	NO(M("(tuning (emu (cores_scale 0.0)))"), VALUE, "0.0", "machine.tuning.emu.cores_scale");
	NO(M("(tuning (emu (cores_scale 1.)))"), VALUE, "1.", "machine.tuning.emu.cores_scale");
	NO(M("(tuning (emu (cores_scale 01)))"), VALUE, "01", "machine.tuning.emu.cores_scale");

	/* Sizes and durations. */
	OK(M("(memory \"2G\")"));
	OK(M("(memory \"1024\")"));
	NO(M("(memory \"2g\")"), VALUE, "\"2g\"", "machine.memory");
	NO(M("(memory \"0G\")"), VALUE, "\"0G\"", "machine.memory");
	NO(M("(memory \"G\")"), VALUE, "\"G\"", "machine.memory");
	NO(M("(memory \"2GB\")"), VALUE, "\"2GB\"", "machine.memory");
	NO(M("(memory 2G)"), SHAPE, "2G", "machine.memory");

	/* The kernel's two shapes. */
	OK(M("(tuning (vm (kernel bundled)))"));
	OK(M("(tuning (vm (kernel \"path:./bzImage\")))"));
	NO(M("(tuning (vm (kernel \"./bzImage\")))"), VALUE, "\"./bzImage\"",
	   "machine.tuning.vm.kernel");
	NO(M("(tuning (vm (kernel \"path:\")))"), VALUE, "\"path:\"", "machine.tuning.vm.kernel");
	NO(M("(tuning (vm (kernel other)))"), VALUE, "other", "machine.tuning.vm.kernel");
	NO(M("(tuning (vm (kernel)))"), SHAPE, "(kernel)", "machine.tuning.vm.kernel");
}

static void test_records(void)
{
	/* A plural key holds its singular entries. */
	OK(M("(ports (port (host 8080) (guest 80)) (port (host 53) (guest 53) (proto udp)))"));
	OK(M("(ports)"));
	NO(M("(ports (volume (host \"a\") (guest \"/b\")))"), UNKNOWN, "volume",
	   "machine.ports.volume");
	NO(M("(ports 80)"), SHAPE, "80", "machine.ports");
	NO(M("(ports (port (host 8080)))"), MISSING, "(port ", "machine.ports.port.guest");
	NO(M("(ports (port (host 65536) (guest 80)))"), VALUE, "65536", "machine.ports.port.host");
	NO(M("(ports (port (host 0) (guest 80)))"), VALUE, "0)", "machine.ports.port.host");
	NO(M("(ports (port (host 80) (guest 80) (proto sctp)))"), VALUE, "sctp",
	   "machine.ports.port.proto");
	OK(M("(ports (port (host 65535) (guest 1)))"));
	OK(M("(volumes (volume (host \"./data\") (guest \"/var/data\") (mode ro)))"));
	NO(M("(volumes (volume (host \"./data\") (guest \"data\")))"), VALUE, "\"data\"",
	   "machine.volumes.volume.guest");
	/* A volume that is a stack of layers, 3.7. */
	OK(M("(volumes (volume (guest \"/etc\") (layers \"dir:./t\" \"dir:./s\") (upper \"./d\")))"));
	OK(M("(volumes (volume (guest \"/etc\") (layers \"dir:./t\" \"dir:./s\") (mode ro)))"));
	NO(M("(volumes (volume (guest \"/etc\")))"), MISSING, "(volume ",
	   "machine.volumes.volume");
	NO(M("(volumes (volume (host \"./a\") (guest \"/etc\") (layers \"dir:./t\")))"), SHAPE,
	   "(layers", "machine.volumes.volume.layers");
	NO(M("(volumes (volume (host \"./a\") (guest \"/etc\") (upper \"./d\")))"), SHAPE,
	   "(upper", "machine.volumes.volume.upper");
	NO(M("(volumes (volume (guest \"/etc\") (layers \"dir:./t\") (upper \"./d\") (mode ro)))"),
	   SHAPE, "(upper", "machine.volumes.volume.upper");
	NO(M("(volumes (volume (guest \"/etc\") (layers)))"), SHAPE, "(layers)",
	   "machine.volumes.volume.layers");
	OK(M("(media (medium (type cdrom) (source \"dir:./cd\")))"));
	NO(M("(media (medium (type cdrom) (source \"./cd\")))"), VALUE, "\"./cd\"",
	   "machine.media.medium.source");

	/* Names the user chooses. */
	OK(M("(env (RUST_LOG \"info\") (PATH \"/bin\") (_x \"\"))"));
	NO(M("(env (RUST_LOG \"info\") (RUST_LOG \"debug\"))"), DUPLICATE,
	   "(RUST_LOG \"debug\")", "machine.env.RUST_LOG");
	NO(M("(env (1X \"a\"))"), VALUE, "1X", "machine.env.1X");
	NO(M("(env (A info))"), SHAPE, "info", "machine.env.A");
	NO(M("(env (A))"), SHAPE, "(A)", "machine.env.A");
	NO(M("(env A)"), SHAPE, "A)", "machine.env");
	OK(M("(secrets (db_password (source \"file:./pw\") (expose env)))"));
	NO(M("(secrets (db_password (source \"./pw\")))"), VALUE, "\"./pw\"",
	   "machine.secrets.db_password.source");
	NO(M("(secrets (db.password (source \"file:./pw\")))"), VALUE, "db.password",
	   "machine.secrets.db.password");
	NO(M("(secrets (pw))"), MISSING, "(pw)", "machine.secrets.pw.source");
	NO("(machines (Web (source \"a\")))", VALUE, "Web", "machines.Web");
	NO("(machines (web (source \"a\")) (web (source \"b\")))", DUPLICATE,
	   "(web (source \"b\")", "machines.web");
	NO("(machines (web (ssh true)))", MISSING, "(web", "machines.web.source");

	/* Tuning: classes, engines, and the reserved names. */
	OK(M("(tuning (vm (profile micro) (cpu_profile migratable)) (qemu (accel tcg))"
	     " (virtualbox (gui false)) (ns (binfmt true)))"));
	NO(M("(tuning (docker))"), UNKNOWN, "docker", "machine.tuning.docker");
	NO(M("(tuning (lxc))"), UNKNOWN, "lxc", "machine.tuning.lxc");
	NO(M("(tuning (qemu (gui true)))"), UNKNOWN, "gui", "machine.tuning.qemu.gui");

	/* A build, and the one rule across keys. */
	OK("(build (base \"a\") (steps (step (name \"x\") (network true) (refresh \"12h\"))))");
	NO("(build (base \"a\") (steps (step (name \"x\") (refresh \"12h\"))))", VALUE,
	   "(refresh", "build.steps.step.refresh");
	NO("(build (base \"a\") (steps (step (name \"x\") (network false) (refresh \"12h\"))))",
	   VALUE, "(refresh", "build.steps.step.refresh");
	NO("(build (base \"a\") (steps (step (name \"x\") (network true) (refresh \"12\"))))",
	   VALUE, "\"12\"", "build.steps.step.refresh");
	NO("(build (steps))", MISSING, "(build", "build.base");
	NO("(build (base \"a\") (steps (step (copy (path (from \"./src\"))))))", MISSING,
	   "(path", "build.steps.step.copy.path.to");
	NO("(build (base \"a\") (steps (step (copy (path (from \"./src\") (to \"/a\"))))))",
	   MISSING, "(step (copy", "build.steps.step.name");
	NO("(build (base \"a\") (steps (step (name \"x\") (discard \"tmp\"))))", VALUE,
	   "\"tmp\"", "build.steps.step.discard");
}

static void test_reporting(void)
{
	static const char text[] = "(machine\n  (source \"a\")\n  (ssh maybe))\n";
	static char       longer[400];
	hull_spec         spec;
	hull_spec_error   error;
	hull_sexpr_form   entry;
	size_t            n;

	cases++;
	if (hull_spec_open(&spec, (const uint8_t *)text, sizeof text - 1, &error) !=
	    HULL_SPEC_ERR_VALUE || error.line != 3u || error.column != 8u)
		FAIL("(ssh maybe) at line %u, column %u; wanted 3, 8", error.line, error.column);

	/* A path longer than the error holds is cut, and still ends in a NUL. */
	cases++;
	n = (size_t)snprintf(longer, sizeof longer, "(machine (source \"a\") (env (");
	memset(longer + n, 'A', 200);
	n += 200;
	n += (size_t)snprintf(longer + n, sizeof longer - n, " x)))");
	if (hull_spec_open(&spec, (const uint8_t *)longer, n, &error) != HULL_SPEC_ERR_SHAPE ||
	    strlen(error.path) != sizeof error.path - 1u ||
	    strncmp(error.path, "machine.env.AAA", 15) != 0)
		FAIL("a long path came back as %zu bytes: %.20s...", strlen(error.path), error.path);

	/* The error may be NULL. */
	cases++;
	if (hull_spec_open(&spec, (const uint8_t *)"(volume)", 8u, NULL) != HULL_SPEC_ERR_UNKNOWN)
		FAIL("a NULL error changed the status");

	/* Finding a key, and not finding one. */
	cases++;
	if (hull_spec_open(&spec, (const uint8_t *)text, 24u, &error) != HULL_SPEC_ERR_SYNTAX)
		FAIL("a file cut inside its machine opened");
	cases++;
	if (hull_spec_open(&spec, (const uint8_t *)"(machine (ssh true) (source \"a\"))", 33u,
	                   &error) != HULL_SPEC_OK || !spec.has_machine || spec.has_build ||
	    !hull_spec_find(&spec, &spec.machine, "source", &entry) || entry.offset != 20u ||
	    hull_spec_find(&spec, &spec.machine, "workdir", &entry))
		FAIL("finding source in a machine");
}

/* Every ```scheme block in `path` is a whole hull.conf and must open. */
static void test_examples(const char *path)
{
	static char  text[1u << 20];
	FILE        *file = fopen(path, "rb");
	size_t       length;
	unsigned     found = 0u;
	const char  *at;

	if (file == NULL) {
		FAIL("cannot open %s", path);
		return;
	}
	length = fread(text, 1u, sizeof text - 1u, file);
	if (!feof(file)) {
		FAIL("%s is larger than %zu bytes", path, sizeof text - 1u);
		fclose(file);
		return;
	}
	fclose(file);
	text[length] = '\0';
	for (at = strstr(text, "\n```scheme\n"); at != NULL; at = strstr(at, "\n```scheme\n")) {
		const char      *start = at + 11;
		const char      *end   = strstr(start, "\n```");
		hull_spec        spec;
		hull_spec_error  error;
		unsigned         line  = 1u;

		if (end == NULL) {
			FAIL("%s: a ```scheme block is never closed", path);
			return;
		}
		for (const char *c = text; c < start; c++)
			line += *c == '\n';
		found++;
		cases++;
		if (hull_spec_open(&spec, (const uint8_t *)start, (size_t)(end - start) + 1u,
		                   &error) != HULL_SPEC_OK)
			FAIL("%s:%u: the example is refused at its line %u, %s: %s", path, line,
			     error.line, error.path, error.reason);
		at = end;
	}
	/* Four when this was written: sections 3.6, 3.10 and 4, and Appendix D.
	 * Fewer means the blocks were not found, not that they passed. */
	if (found < 4u)
		FAIL("%s: %u ```scheme blocks, and there are at least 4", path, found);
}

int main(int argc, char **argv)
{
	alarm(60);
	if (argc != 2) {
		fprintf(stderr, "usage: %s project.md\n", argv[0]);
		return 2;
	}
	test_file();
	test_values();
	test_records();
	test_reporting();
	test_examples(argv[1]);
	printf("spec: %u cases, %u failed\n", cases, failures);
	return failures == 0u ? 0 : 1;
}
