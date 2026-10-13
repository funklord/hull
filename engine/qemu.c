/*
 * qemu.c -- the qemu engine's declaration: the vm class with KVM and the
 * emu class with TCG, one driver (project.md sections 5.2 and 5.3).
 */

#include "builtin.h"

static const hull_cap_entry entries[] = {
	/* The dir over virtiofs, with upper/ on a second share for overlay
	 * (5.2); an image ref through the store. */
	{ "source.dir.direct",  HULL_SUP_NATIVE,   NULL, NULL },
	{ "source.dir.overlay", HULL_SUP_NATIVE,   NULL, NULL },
	{ "source.ref.overlay", HULL_SUP_NATIVE,   NULL, NULL },

	/* Applied by the guest agent (5.2). */
	{ "command",            HULL_SUP_NATIVE,   NULL, NULL },
	{ "workdir",            HULL_SUP_NATIVE,   NULL, NULL },
	{ "env",                HULL_SUP_NATIVE,   NULL, NULL },
	{ "hostname",           HULL_SUP_NATIVE,   NULL, NULL },
	{ "ports",              HULL_SUP_EMULATED, "a host-side forwarder", NULL },
	{ "volumes",            HULL_SUP_NATIVE,   NULL, NULL },
	{ "cpus",               HULL_SUP_NATIVE,   NULL, NULL },
	{ "memory",             HULL_SUP_NATIVE,   "a hard allocation", NULL },
	{ "ssh",                HULL_SUP_NATIVE,   NULL, NULL },
	{ "secrets",            HULL_SUP_NATIVE,   "a tmpfs the agent mounts", NULL },

	{ "security.readonly_root", HULL_SUP_NATIVE, NULL, NULL },
	{ "security.no_new_privs",  HULL_SUP_NATIVE, "set by the agent on the workload", NULL },
	{ "security.allow",         HULL_SUP_EMULATED, "virtio, VFIO or USB devices where permitted",
	  NULL },

	/* virtiofs misses host-side events; polling is offered (6). */
	{ "fidelity.inotify",   HULL_SUP_REJECTED, "virtiofs does not see changes made on the host",
	  "vm:inotify-poll" },

	{ "projection.auto",        HULL_SUP_NATIVE, NULL, NULL },
	{ "projection.passthrough", HULL_SUP_NATIVE, NULL, NULL },
	{ "writeback.none",         HULL_SUP_NATIVE, NULL, NULL },
	{ "media",                  HULL_SUP_NATIVE, NULL, NULL },

	/* The agent's disconnect policy (B.4). */
	{ "on_disconnect.stop", HULL_SUP_NATIVE,   NULL, NULL },
	{ "on_disconnect.keep", HULL_SUP_NATIVE,   NULL, NULL },

	/* J.5, all of it this engine's or its classes'. */
	{ "vm.profile.micro",       HULL_SUP_NATIVE, NULL, NULL },
	{ "vm.profile.full",        HULL_SUP_NATIVE, "boots in seconds", NULL },
	{ "vm.kernel.bundled",      HULL_SUP_NATIVE, NULL, NULL },
	{ "vm.kernel.image",        HULL_SUP_NATIVE, NULL, NULL },
	{ "vm.kernel.path",         HULL_SUP_NATIVE, NULL, NULL },
	{ "vm.cpu_profile.host",    HULL_SUP_NATIVE, NULL, NULL },
	{ "vm.cpu_profile.migratable", HULL_SUP_NATIVE, NULL, NULL },
	{ "qemu.accel.auto",        HULL_SUP_NATIVE, NULL, NULL },
	{ "qemu.accel.kvm",         HULL_SUP_NATIVE, NULL, NULL },
	{ "qemu.accel.tcg",         HULL_SUP_NATIVE, NULL, NULL },
	{ "emu.cores_scale",        HULL_SUP_NATIVE, NULL, NULL },

	{ NULL,                 HULL_SUP_NATIVE,   NULL, NULL },
};

static const char *const classes[] = { "vm", "emu", NULL };

/* What emu boots on any host (5.3); vm takes only the host's own. */
static const char *const arches[] = { "amd64", "arm64", "riscv64", NULL };

/* J.4's examples. */
static const char *const media_types[] = { "cdrom", "floppy", "disk", NULL };

static const hull_capabilities caps = {
	.struct_size    = sizeof (hull_capabilities),
	.engine_name    = "qemu",
	.engine_version = "0",
	.classes        = classes,
	.entries        = entries,
	.media_types    = media_types,
	.migration      = HULL_MIG_LIVE,  /* H.3 */
	.arches         = arches,
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

const hull_engine *hull_engine_qemu(void)
{
	return &engine;
}
