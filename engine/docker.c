/*
 * docker.c -- the docker adapter's declaration: the ns class through the
 * Docker daemon (project.md section 5.0).
 */

#include "builtin.h"

static const hull_cap_entry entries[] = {
	/* Docker runs an image, not a directory (5.0). */
	{ "source.dir.direct",  HULL_SUP_REJECTED,
	  "docker cannot run a directory in place; (mutable overlay) runs a copy", NULL },
	{ "source.dir.overlay", HULL_SUP_EMULATED, "a cached docker import of the tree", NULL },
	{ "source.ref.overlay", HULL_SUP_NATIVE,   NULL, NULL },

	/* "ports/volumes/env map directly" (5.0). */
	{ "command",            HULL_SUP_NATIVE,   NULL, NULL },
	{ "workdir",            HULL_SUP_NATIVE,   NULL, NULL },
	{ "env",                HULL_SUP_NATIVE,   NULL, NULL },
	{ "ports",              HULL_SUP_NATIVE,   NULL, NULL },
	{ "volumes",            HULL_SUP_NATIVE,   NULL, NULL },
	{ "hostname",           HULL_SUP_NATIVE,   NULL, NULL },

	/* ns: cgroups, memory a ceiling (6). */
	{ "cpus",               HULL_SUP_NATIVE,   NULL, NULL },
	{ "memory",             HULL_SUP_NATIVE,   "a ceiling, not an allocation", NULL },

	/* Every backend (8.1); a tmpfs bound in on ns (Appendix C). */
	{ "ssh",                HULL_SUP_NATIVE,   NULL, NULL },
	{ "secrets",            HULL_SUP_NATIVE,   NULL, NULL },

	/* Section 7: capabilities are additive grants on ns. */
	{ "security.readonly_root", HULL_SUP_NATIVE, NULL, NULL },
	{ "security.no_new_privs",  HULL_SUP_NATIVE, NULL, NULL },
	{ "security.allow",         HULL_SUP_NATIVE, "capability grants", NULL },

	/* A shared kernel sees every event and lock (6). */
	{ "fidelity.inotify",   HULL_SUP_NATIVE,   NULL, NULL },
	{ "fidelity.flock",     HULL_SUP_NATIVE,   NULL, NULL },

	{ "projection.auto",    HULL_SUP_NATIVE,   NULL, NULL },
	{ "projection.bind",    HULL_SUP_NATIVE,   NULL, NULL },
	{ "writeback.none",     HULL_SUP_NATIVE,   NULL, NULL },
	{ "on_disconnect.stop", HULL_SUP_NATIVE,   NULL, NULL },

	{ NULL,                 HULL_SUP_NATIVE,   NULL, NULL },
};

static const char *const classes[] = { "ns", NULL };

static const hull_capabilities caps = {
	.struct_size    = sizeof (hull_capabilities),
	.engine_name    = "docker",
	.engine_version = "0",
	.classes        = classes,
	.entries        = entries,
	.migration      = HULL_MIG_WARM,  /* ns: warm, best-effort (H.3) */
	.requires_root  = 1,              /* dockerd's root daemon (5.0) */
};

static const hull_engine_ops ops = {
	.struct_size = sizeof (hull_engine_ops),
};

static const hull_engine engine = {
	.struct_size = sizeof (hull_engine),
	.abi_major   = HULL_ABI_MAJOR,
	.abi_minor   = HULL_ABI_MINOR,
	.caps        = &caps,
	.ops         = &ops,
};

const hull_engine *hull_engine_docker(void)
{
	return &engine;
}
