/*
 * registry_test.c -- registering engines, and what `(backend ...)` picks.
 *
 * The engines are declarations only: nothing here calls an operation, so
 * every engine shares one empty operations table.
 */

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "registry.h"

static unsigned cases;
static unsigned failures;

#define FAIL(...) do { \
	failures++; \
	printf("FAIL %s:%d: ", __FILE__, __LINE__); \
	printf(__VA_ARGS__); \
	printf("\n"); \
} while (0)

static const hull_engine_ops  no_ops = { .struct_size = sizeof (hull_engine_ops) };
static const hull_cap_entry   no_entries[] = { { NULL, HULL_SUP_NATIVE, NULL, NULL } };

static const char *const ns_only[]   = { "ns", NULL };
static const char *const vm_emu[]    = { "vm", "emu", NULL };
static const char *const vm_only[]   = { "vm", NULL };
static const char *const bad_class[] = { "vm", "container", NULL };
static const char *const none[]      = { NULL };

#define CAPS(name, list) { .struct_size = sizeof (hull_capabilities), .engine_name = name, \
                           .classes = list, .entries = no_entries }
#define ENGINE(caps_name) { .struct_size = sizeof (hull_engine), .abi_major = HULL_ABI_MAJOR, \
                            .abi_minor = HULL_ABI_MINOR, .caps = &caps_name, .ops = &no_ops }

static const hull_capabilities docker_caps     = CAPS("docker", ns_only);
static const hull_capabilities crun_caps       = CAPS("crun", ns_only);
static const hull_capabilities qemu_caps       = CAPS("qemu", vm_emu);
static const hull_capabilities virtualbox_caps = CAPS("virtualbox", vm_only);
static const hull_engine       docker     = ENGINE(docker_caps);
static const hull_engine       crun       = ENGINE(crun_caps);
static const hull_engine       qemu       = ENGINE(qemu_caps);
static const hull_engine       virtualbox = ENGINE(virtualbox_caps);

static void expect_add(int line, hull_registry *registry, const hull_engine *engine,
                       hull_registry_status want)
{
	const char          *reason = NULL;
	hull_registry_status got    = hull_registry_add(registry, engine, &reason);

	cases++;
	if (got != want) {
		failures++;
		printf("FAIL %s:%d: status %d (%s), wanted %d\n", __FILE__, line, (int)got,
		       reason != NULL ? reason : "no reason", (int)want);
	} else if (got != HULL_REGISTRY_OK && reason == NULL) {
		failures++;
		printf("FAIL %s:%d: refused without a reason\n", __FILE__, line);
	}
}

#define ADD(registry, engine, status) \
	expect_add(__LINE__, registry, engine, HULL_REGISTRY_##status)

static void test_add(void)
{
	hull_registry            registry;
	static hull_capabilities short_caps;
	static hull_capabilities unnamed;
	static hull_capabilities upper;
	static hull_capabilities classed;
	static hull_capabilities classless;
	static hull_capabilities wrong_class;
	static hull_capabilities tableless;
	static hull_engine       engine;

	hull_registry_init(&registry, "amd64");
	ADD(&registry, &docker, OK);
	ADD(&registry, &qemu, OK);
	ADD(&registry, &docker, ERR_DUPLICATE);
	ADD(&registry, NULL, ERR_ENGINE);

	/* Another major ABI, or structs shorter than this one's. */
	engine = docker;
	engine.abi_major = HULL_ABI_MAJOR + 1u;
	ADD(&registry, &engine, ERR_ABI);
	engine = crun;
	engine.struct_size = sizeof engine - 1u;
	ADD(&registry, &engine, ERR_ABI);
	short_caps = crun_caps;
	short_caps.struct_size = sizeof short_caps - 1u;
	engine = crun;
	engine.caps = &short_caps;
	ADD(&registry, &engine, ERR_ABI);
	engine = crun;
	engine.ops = NULL;
	ADD(&registry, &engine, ERR_ENGINE);

	/* A name `(backend ...)` can write, and not a class's. */
	unnamed = crun_caps;
	unnamed.engine_name = NULL;
	upper = crun_caps;
	upper.engine_name = "Crun";
	classed = crun_caps;
	classed.engine_name = "vm";
	engine = crun;
	engine.caps = &unnamed;
	ADD(&registry, &engine, ERR_ENGINE);
	engine.caps = &upper;
	ADD(&registry, &engine, ERR_ENGINE);
	engine.caps = &classed;
	ADD(&registry, &engine, ERR_ENGINE);

	/* At least one class, each one of the three, and an entry table. */
	classless = crun_caps;
	classless.classes = none;
	wrong_class = crun_caps;
	wrong_class.classes = bad_class;
	tableless = crun_caps;
	tableless.entries = NULL;
	engine.caps = &classless;
	ADD(&registry, &engine, ERR_ENGINE);
	engine.caps = &wrong_class;
	ADD(&registry, &engine, ERR_ENGINE);
	engine.caps = &tableless;
	ADD(&registry, &engine, ERR_ENGINE);

	/* Nothing refused was kept. */
	cases++;
	if (registry.count != 2u)
		FAIL("%u engines registered, wanted 2", registry.count);
}

static void test_full(void)
{
	static hull_capabilities caps[HULL_REGISTRY_MAX + 1u];
	static hull_engine       engines[HULL_REGISTRY_MAX + 1u];
	static char              names[HULL_REGISTRY_MAX + 1u][8];
	hull_registry            registry;

	hull_registry_init(&registry, "amd64");
	for (unsigned i = 0u; i <= HULL_REGISTRY_MAX; i++) {
		snprintf(names[i], sizeof names[i], "e%u", i);
		caps[i] = crun_caps;
		caps[i].engine_name = names[i];
		engines[i] = crun;
		engines[i].caps = &caps[i];
		if (i < HULL_REGISTRY_MAX)
			hull_registry_add(&registry, &engines[i], NULL);
	}
	ADD(&registry, &engines[HULL_REGISTRY_MAX], ERR_FULL);
}

/* `(backend ...)` in `text`, against a registry of docker, crun, qemu and
 * virtualbox in that order, after `setup` has run on it. */
static void expect_choice(int line, const char *text, const char *backend,
                          const char *host_arch, hull_registry_status want, const hull_engine *engine,
                          const char *class_name,
                          void (*setup)(hull_registry *registry))
{
	hull_registry        registry;
	hull_spec            spec;
	hull_spec_error      error;
	hull_choice          choice;
	hull_registry_status got;
	const char          *reason = NULL;

	cases++;
	hull_registry_init(&registry, host_arch);
	hull_registry_add(&registry, &docker, NULL);
	hull_registry_add(&registry, &crun, NULL);
	hull_registry_add(&registry, &qemu, NULL);
	hull_registry_add(&registry, &virtualbox, NULL);
	if (setup != NULL)
		setup(&registry);
	if (hull_spec_open(&spec, (const uint8_t *)text, strlen(text), &error) != HULL_SPEC_OK) {
		FAIL("line %d: the spec is refused: %s", line, error.reason);
		return;
	}
	memset(&choice, 0, sizeof choice);
	got = hull_registry_choose(&registry, &spec, &spec.machine, backend, &choice, &reason);
	if (got != want) {
		failures++;
		printf("FAIL %s:%d: status %d (%s), wanted %d\n", __FILE__, line, (int)got,
		       reason != NULL ? reason : "no reason", (int)want);
	} else if (got == HULL_REGISTRY_OK &&
	           (choice.engine != engine || strcmp(choice.class_name, class_name) != 0)) {
		failures++;
		printf("FAIL %s:%d: %s as %s, wanted %s as %s\n", __FILE__, line,
		       choice.engine->caps->engine_name, choice.class_name,
		       engine->caps->engine_name, class_name);
	} else if (got != HULL_REGISTRY_OK && reason == NULL) {
		failures++;
		printf("FAIL %s:%d: refused without a reason\n", __FILE__, line);
	}
}

#define CHOOSE(text, engine, class_name) \
	expect_choice(__LINE__, text, NULL, "amd64", HULL_REGISTRY_OK, engine, class_name, NULL)
#define REFUSE(text, status) \
	expect_choice(__LINE__, text, NULL, "amd64", HULL_REGISTRY_##status, NULL, NULL, NULL)

static void crun_for_ns(hull_registry *registry)
{
	if (hull_registry_set_default(registry, "ns", "crun") != HULL_REGISTRY_OK)
		FAIL("crun could not be made ns's default");
}

static void only_docker(hull_registry *registry)
{
	registry->count = 1u;
}

static void test_choose(void)
{
	/* A class gets the first engine registered that implements it. */
	CHOOSE("(machine (source \"a\"))", &docker, "ns");
	CHOOSE("(machine (source \"a\") (backend ns))", &docker, "ns");
	CHOOSE("(machine (source \"a\") (backend vm))", &qemu, "vm");
	CHOOSE("(machine (source \"a\") (backend emu))", &qemu, "emu");

	/* Until host policy names another. */
	expect_choice(__LINE__, "(machine (source \"a\"))", NULL, "amd64", HULL_REGISTRY_OK,
	              &crun, "ns", crun_for_ns);

	/* An engine by name runs as its first class... */
	CHOOSE("(machine (source \"a\") (backend qemu))", &qemu, "vm");
	CHOOSE("(machine (source \"a\") (backend virtualbox))", &virtualbox, "vm");
	CHOOSE("(machine (source \"a\") (backend qemu) (arch amd64))", &qemu, "vm");
	CHOOSE("(machine (source \"a\") (backend qemu) (arch host))", &qemu, "vm");
	/* ...unless the arch is foreign and it can emulate. */
	CHOOSE("(machine (source \"a\") (backend qemu) (arch arm64))", &qemu, "emu");
	expect_choice(__LINE__, "(machine (source \"a\") (backend qemu) (arch arm64))", NULL, "arm64",
	              HULL_REGISTRY_OK, &qemu, "vm", NULL);
	/* An engine that cannot is left for the matrix to refuse the arch. */
	CHOOSE("(machine (source \"a\") (backend virtualbox) (arch arm64))", &virtualbox, "vm");
	/* A class named outright is not second-guessed. */
	CHOOSE("(machine (source \"a\") (backend vm) (arch arm64))", &qemu, "vm");

	/* A backend named from outside wins over the spec's own. */
	expect_choice(__LINE__, "(machine (source \"a\") (backend docker))", "vm", "amd64",
	              HULL_REGISTRY_OK, &qemu, "vm", NULL);
	expect_choice(__LINE__, "(machine (source \"a\") (arch arm64))", "qemu", "amd64",
	              HULL_REGISTRY_OK, &qemu, "emu", NULL);
	expect_choice(__LINE__, "(machine (source \"a\"))", "lxc", "amd64",
	              HULL_REGISTRY_ERR_UNKNOWN, NULL, NULL, NULL);

	REFUSE("(machine (source \"a\") (backend hyperv))", ERR_UNKNOWN);
	REFUSE("(machine (source \"a\") (backend dock))", ERR_UNKNOWN);
	REFUSE("(machine (source \"a\") (backend dockers))", ERR_UNKNOWN);
	expect_choice(__LINE__, "(machine (source \"a\") (backend vm))", NULL, "amd64",
	              HULL_REGISTRY_ERR_NO_ENGINE, NULL, NULL, only_docker);
}

static void test_set_default(void)
{
	hull_registry registry;

	hull_registry_init(&registry, "amd64");
	hull_registry_add(&registry, &docker, NULL);
	hull_registry_add(&registry, &qemu, NULL);
	cases++;
	if (hull_registry_set_default(&registry, "vm", "docker") != HULL_REGISTRY_ERR_NO_ENGINE ||
	    hull_registry_set_default(&registry, "vm", "crun") != HULL_REGISTRY_ERR_UNKNOWN ||
	    hull_registry_set_default(&registry, "container", "qemu") != HULL_REGISTRY_ERR_UNKNOWN ||
	    hull_registry_set_default(&registry, "emu", "qemu") != HULL_REGISTRY_OK ||
	    registry.defaults[2] != &qemu || registry.defaults[1] != NULL)
		FAIL("set_default");
}

int main(void)
{
	alarm(60);
	test_add();
	test_full();
	test_choose();
	test_set_default();
	printf("registry: %u cases, %u failed\n", cases, failures);
	return failures == 0u ? 0 : 1;
}
