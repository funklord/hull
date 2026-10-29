/*
 * registry.h -- the engines hull has loaded, and which one a machine's
 * `(backend ...)` chooses (project.md sections 5.0 and 4.5).
 *
 * `(backend ns)` names a class and gets the host's default engine of it;
 * `(backend qemu)` names an engine and gets the class it runs the machine
 * as. Either way the result is the pair hull_matrix_resolve() takes.
 *
 * The registry holds pointers to what hull_engine_register() returned,
 * which are static for the engine's life, and allocates nothing.
 */

#ifndef HULL_REGISTRY_H
#define HULL_REGISTRY_H

#include <stdint.h>

#include "hull_engine.h"
#include "spec.h"

#ifdef __cplusplus
extern "C" {
#endif

#define HULL_REGISTRY_MAX 32u
#define HULL_CLASS_COUNT  4u   /* ns, vm, emu, dos: registry.c's list, held to it */

typedef enum hull_registry_status {
	HULL_REGISTRY_OK = 0,
	HULL_REGISTRY_ERR_ABI,        /* another major version, or a struct too short */
	HULL_REGISTRY_ERR_ENGINE,     /* a declaration missing a part, or with a bad name */
	HULL_REGISTRY_ERR_DUPLICATE,  /* an engine of that name is registered */
	HULL_REGISTRY_ERR_FULL,
	HULL_REGISTRY_ERR_UNKNOWN,    /* no engine or class of that name */
	HULL_REGISTRY_ERR_NO_ENGINE,  /* a class no registered engine implements */
} hull_registry_status;

typedef struct hull_registry {
	const char        *host_arch;  /* "amd64": what `(arch host)` means */
	const hull_engine *engines[HULL_REGISTRY_MAX];
	uint32_t           count;
	const hull_engine *defaults[HULL_CLASS_COUNT];  /* by class: ns, vm, emu, dos; NULL =
	                                                  first registered */
} hull_registry;

/* What `(backend ...)` chose. `class_name` is static. */
typedef struct hull_choice {
	const hull_engine *engine;
	const char        *class_name;
} hull_choice;

void hull_registry_init(hull_registry *registry, const char *host_arch);

/* Add an engine, in order of preference: a class's default is the first
 * engine registered that implements it, until one is set. `reason`, which
 * may be NULL, says why an engine was refused. */
hull_registry_status hull_registry_add(hull_registry *registry, const hull_engine *engine,
                                       const char **reason);

/* Make `engine_name` the host's default engine of `class_name`, as host
 * policy says. The engine must implement the class. */
hull_registry_status hull_registry_set_default(hull_registry *registry,
                                               const char *class_name,
                                               const char *engine_name);

/* The engine and class a backend chooses for `machine`, a machine in a
 * spec hull_spec_open() accepted. `backend` is the class or engine to
 * choose, as `hull check --backend` names one, or NULL for the machine's
 * own `(backend ...)`: J.1's `ns` where it names none. */
hull_registry_status hull_registry_choose(const hull_registry *registry,
                                          const hull_spec *spec,
                                          const hull_sexpr_form *machine,
                                          const char *backend, hull_choice *choice,
                                          const char **reason);

#ifdef __cplusplus
}
#endif

#endif /* HULL_REGISTRY_H */
