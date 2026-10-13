/*
 * registry.c -- loaded engines, and the choice `(backend ...)` makes.
 *
 * A declaration is checked once, here, so that the matrix and everything
 * after it may read it without checking it again: a name, at least one
 * class from the three section 5.0 defines, and an entry table, however
 * empty.
 */

#include <string.h>

#include "registry.h"

static const char *const classes[] = { "ns", "vm", "emu" };

#define CLASS_COUNT (sizeof classes / sizeof classes[0])

static int set_reason(const char **reason, const char *text)
{
	if (reason != NULL)
		*reason = text;
	return 0;
}

/* The index of a class named `name`, or CLASS_COUNT. */
static unsigned class_index(const char *name)
{
	unsigned i;

	for (i = 0u; i < CLASS_COUNT; i++)
		if (strcmp(classes[i], name) == 0)
			break;
	return i;
}

static int implements(const hull_engine *engine, const char *class_name)
{
	for (const char *const *c = engine->caps->classes; *c != NULL; c++)
		if (strcmp(*c, class_name) == 0)
			return 1;
	return 0;
}

/* Lowercase words, as `(backend ...)` can name them (J.1). */
static int is_name(const char *name)
{
	if (name == NULL || name[0] == '\0' || name[0] == '-')
		return 0;
	for (; *name != '\0'; name++)
		if (!(*name >= 'a' && *name <= 'z') && !(*name >= '0' && *name <= '9') &&
		    *name != '_' && *name != '-')
			return 0;
	return 1;
}

static const hull_engine *find(const hull_registry *registry, const char *name, size_t length)
{
	for (uint32_t i = 0u; i < registry->count; i++) {
		const char *engine_name = registry->engines[i]->caps->engine_name;

		if (strlen(engine_name) == length && memcmp(engine_name, name, length) == 0)
			return registry->engines[i];
	}
	return NULL;
}

void hull_registry_init(hull_registry *registry, const char *host_arch)
{
	memset(registry, 0, sizeof *registry);
	registry->host_arch = host_arch;
}

hull_registry_status hull_registry_add(hull_registry *registry, const hull_engine *engine,
                                       const char **reason)
{
	const hull_capabilities *caps;

	if (engine == NULL || engine->caps == NULL || engine->ops == NULL) {
		set_reason(reason, "the engine returned no declaration or no operations");
		return HULL_REGISTRY_ERR_ENGINE;
	}
	/* Minor 1 is the first, so nothing older exists to be read short. */
	if (engine->abi_major != HULL_ABI_MAJOR || engine->struct_size < sizeof (hull_engine) ||
	    engine->caps->struct_size < sizeof (hull_capabilities)) {
		set_reason(reason, "the engine was built for another ABI");
		return HULL_REGISTRY_ERR_ABI;
	}
	caps = engine->caps;
	if (!is_name(caps->engine_name) || class_index(caps->engine_name) != CLASS_COUNT) {
		set_reason(reason, "an engine's name is a lowercase word and not a class's");
		return HULL_REGISTRY_ERR_ENGINE;
	}
	if (caps->classes == NULL || caps->classes[0] == NULL || caps->entries == NULL) {
		set_reason(reason, "an engine implements a class and declares an entry table");
		return HULL_REGISTRY_ERR_ENGINE;
	}
	for (const char *const *c = caps->classes; *c != NULL; c++) {
		if (class_index(*c) == CLASS_COUNT) {
			set_reason(reason, "an engine's classes are ns, vm and emu");
			return HULL_REGISTRY_ERR_ENGINE;
		}
	}
	if (find(registry, caps->engine_name, strlen(caps->engine_name)) != NULL) {
		set_reason(reason, "an engine of that name is registered already");
		return HULL_REGISTRY_ERR_DUPLICATE;
	}
	if (registry->count == HULL_REGISTRY_MAX) {
		set_reason(reason, "the registry is full");
		return HULL_REGISTRY_ERR_FULL;
	}
	registry->engines[registry->count++] = engine;
	return HULL_REGISTRY_OK;
}

hull_registry_status hull_registry_set_default(hull_registry *registry,
                                               const char *class_name,
                                               const char *engine_name)
{
	unsigned           index = class_name == NULL ? CLASS_COUNT : class_index(class_name);
	const hull_engine *engine;

	if (index == CLASS_COUNT || engine_name == NULL)
		return HULL_REGISTRY_ERR_UNKNOWN;
	engine = find(registry, engine_name, strlen(engine_name));
	if (engine == NULL)
		return HULL_REGISTRY_ERR_UNKNOWN;
	if (!implements(engine, class_name))
		return HULL_REGISTRY_ERR_NO_ENGINE;
	registry->defaults[index] = engine;
	return HULL_REGISTRY_OK;
}

/* The value of `(key value)` in `machine`, or 0 when it is not written. */
static int value_of(const hull_spec *spec, const hull_sexpr_form *machine, const char *key,
                    hull_sexpr_form *value)
{
	hull_sexpr_form entry;
	hull_sexpr_form head;

	if (!hull_spec_find(spec, machine, key, &entry))
		return 0;
	hull_sexpr_child(&spec->doc, &entry, &head);
	return hull_sexpr_next(&spec->doc, &head, value);
}

/* An arch other than the host's own (J.1: foreign means emu). */
static int foreign(const hull_registry *registry, const hull_spec *spec,
                   const hull_sexpr_form *machine)
{
	hull_sexpr_form arch;

	if (!value_of(spec, machine, "arch", &arch) || hull_sexpr_is(&spec->doc, &arch, "host"))
		return 0;
	return registry->host_arch == NULL || !hull_sexpr_is(&spec->doc, &arch, registry->host_arch);
}

hull_registry_status hull_registry_choose(const hull_registry *registry,
                                          const hull_spec *spec,
                                          const hull_sexpr_form *machine,
                                          hull_choice *choice, const char **reason)
{
	hull_sexpr_form    backend;
	const char        *name   = "ns";
	size_t             length = 2u;
	const hull_engine *engine;

	if (value_of(spec, machine, "backend", &backend)) {
		name   = (const char *)spec->doc.data + backend.content;
		length = backend.content_length;
	}
	for (unsigned i = 0u; i < CLASS_COUNT; i++) {
		if (strlen(classes[i]) != length || memcmp(classes[i], name, length) != 0)
			continue;
		engine = registry->defaults[i];
		for (uint32_t e = 0u; engine == NULL && e < registry->count; e++)
			if (implements(registry->engines[e], classes[i]))
				engine = registry->engines[e];
		if (engine == NULL) {
			set_reason(reason, "no engine on this host implements that class");
			return HULL_REGISTRY_ERR_NO_ENGINE;
		}
		choice->engine     = engine;
		choice->class_name = classes[i];
		return HULL_REGISTRY_OK;
	}
	engine = find(registry, name, length);
	if (engine == NULL) {
		set_reason(reason, "no engine or class of that name is loaded");
		return HULL_REGISTRY_ERR_UNKNOWN;
	}
	/* An engine runs a machine as the first class it lists, unless the
	 * machine's arch is foreign and the engine can emulate. */
	choice->engine     = engine;
	choice->class_name = classes[class_index(engine->caps->classes[0])];
	if (foreign(registry, spec, machine) && implements(engine, "emu"))
		choice->class_name = "emu";
	return HULL_REGISTRY_OK;
}
