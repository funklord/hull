/*
 * dosemu.c -- a dos class engine: a machine run by dosemu2, for software
 * written for DOS (project.md section 5.0). What the dos engines share is
 * dos.c; this is dosemu2's own.
 *
 * Where DOSBox emulates a whole PC, dosemu2 runs DOS code on the host's
 * CPU, under the FreeDOS kernel fdpp and the comcom64 shell, and reaches
 * host directories by its own redirector, LREDIR -- the second engine
 * the dos class was made for. dosemu2 mounts the machine's directory as
 * C: (--Fdrive_c) and keeps D: and E: for its own commands and shell,
 * and F: for the directory -K names, the engine's own, so a volume may
 * not take those. It runs H:\HULL.BAT from the engine's own
 * directory (-K, -E): LREDIR H: and each volume at its letter, read-only
 * where the volume is, SET for each env entry, C: and the workdir, the
 * command by CALL with its DOS output redirected to H:\OUT.TXT, then
 * S%ERRORLEVEL% into H:\STATUS.TXT and exitemu. The status is read from
 * the batch's file rather than dosemu2's exit, which has codes of its
 * own for its own failures.
 *
 * dosemu2 refuses LREDIR to a path not in its lredir_paths, given here
 * on its command line; that list is split on spaces, so no path the
 * engine hands it may hold one. It runs in its dumb terminal (-td), its
 * keyboard on stdin (-ks), which is /dev/null; what it writes there, its
 * banner and the kernel's, goes to the machine's log, not its output.
 * Sound is off, a test machine wanting none, and with it the PulseAudio
 * client that would leave its directory in the HOME the engine gives.
 */

#define _GNU_SOURCE

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "access.h"
#include "builtin.h"
#include "dos.h"

static int write_file(const char *path, const char *text)
{
	int    fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
	size_t length = strlen(text);
	int    ok = fd >= 0 && write(fd, text, length) == (ssize_t)length;

	if (fd >= 0 && close(fd) != 0)
		ok = 0;
	return ok ? 0 : -1;
}

/* HULL.BAT in the machine's directory: 0, or -1. */
static int write_files(const hull_machine *m)
{
	char   path[4200];
	char  *text = malloc(64u * 1024u);
	size_t at = 0u;
	int    ok;

#define ADD(...) at += (size_t)snprintf(text + at, 64u * 1024u - at, __VA_ARGS__)
	if (text == NULL)
		return -1;
	ADD("@echo off\r\nLREDIR H: %s\r\n", m->dir);
	for (unsigned i = 0u; i < m->volume_count; i++)
		ADD("LREDIR%s %c: %s\r\nif errorlevel 1 goto m%c\r\n",
		    m->volume_readonly[i] ? " -R" : "", m->volume_drive[i], m->volume_host[i],
		    m->volume_drive[i]);
	for (unsigned i = 0u; i < m->media_count; i++)
		ADD("LREDIR -C %c: %s\r\nif errorlevel 1 goto m%c\r\n", m->media_drive[i],
		    m->media_host[i], m->media_drive[i]);
	for (unsigned i = 0u; i < HULL_DOS_MAX_ENV && m->envp[i] != NULL; i++)
		ADD("set %s\r\n", m->envp[i]);
	ADD("C:\r\ncd \\");
	for (const char *c = m->workdir + (m->workdir[0] == '/'); *c != '\0'; c++)
		ADD("%c", *c == '/' ? '\\' : *c);
	ADD("\r\ncall");
	for (unsigned i = 0u; i < HULL_DOS_MAX_ARGS && m->argv[i] != NULL; i++)
		ADD(" %s", m->argv[i]);
	ADD(" > H:\\OUT.TXT\r\necho S%%ERRORLEVEL%%>H:\\STATUS.TXT\r\nexitemu %%ERRORLEVEL%%\r\n");
	/* A drive LREDIR refused ends the batch before the command runs. */
	for (unsigned i = 0u; i < m->volume_count; i++)
		ADD(":m%c\r\necho M%c>H:\\STATUS.TXT\r\nexitemu 125\r\n", m->volume_drive[i],
		    m->volume_drive[i]);
	for (unsigned i = 0u; i < m->media_count; i++)
		ADD(":m%c\r\necho M%c>H:\\STATUS.TXT\r\nexitemu 125\r\n", m->media_drive[i],
		    m->media_drive[i]);
#undef ADD
	snprintf(path, sizeof path, "%s/HULL.BAT", m->dir);
	ok = write_file(path, text) == 0;
	free(text);
	return ok ? 0 : -1;
}

static void exec_child(const hull_machine *m)
{
	static char local[4200];
	static char paths[16384];
	static char setting[16384 + 32];
	static char home[4200 + 8];
	static char path_entry[8200];
	const char *path = getenv("PATH");
	size_t      at;

	snprintf(local, sizeof local, "%s/local", m->dir);
	at = (size_t)snprintf(paths, sizeof paths, "%s", m->dir);
	for (unsigned i = 0u; i < m->volume_count && at < sizeof paths; i++)
		at += (size_t)snprintf(paths + at, sizeof paths - at, " %s", m->volume_host[i]);
	for (unsigned i = 0u; i < m->media_count && at < sizeof paths; i++)
		at += (size_t)snprintf(paths + at, sizeof paths - at, " %s", m->media_host[i]);
	snprintf(setting, sizeof setting, "lredir_paths \"%s\"", paths);
	snprintf(home, sizeof home, "HOME=%s", m->dir);
	snprintf(path_entry, sizeof path_entry, "PATH=%s",
	         path != NULL ? path : "/usr/local/bin:/usr/bin:/bin");
	if (mkdir(local, 0700) != 0)
		return;
	{
		char *const argv[] = { "dosemu", "-n", "--Flocal_dir", local, "--Fdrive_c", m->root,
		                       "-I", setting, "-I", "sound off", "-K", (char *)m->dir,
		                       "-td", "-ks",
		                       "-E", "HULL.BAT", NULL };
		char *const envp[] = { home, path_entry, "TERM=dumb", NULL };

		hull_access_execute(argv, envp);
	}
}

/* The batch's word in STATUS.TXT, not dosemu2's exit, which has codes of
 * its own for its own failures. */
static int end_code(const hull_machine *m, int wstatus)
{
	(void)wstatus;
	return hull_dos_said(m);
}

static const hull_dos_engine flavour = {
	.name         = "dosemu",
	.reserved     = "DEF",
	.reserved_why = "a volume may not be /d, /e or /f under dosemu2, whose own commands and "
	                "shell are D: and E:, and which mounts hull's batch directory as F:",
	.not_found    = "Bad command or file name - ",
	.space_free   = 1,
	.images       = 0,
	.makes        = "tmp",
	.write_files  = write_files,
	.exec_child   = exec_child,
	.end_code     = end_code,
};

static hull_status dosemu_create(hull_core *core, hull_buf plan, hull_machine **out,
                                 hull_error *error)
{
	return hull_dos_create(&flavour, core, plan, out, error);
}

static const hull_cap_entry entries[] = {
	{ "source.dir.direct",  HULL_SUP_NATIVE,   "the directory is drive C:", NULL },
	{ "source.dir.overlay", HULL_SUP_EMULATED,
	  "a copy of the tree in the machine's directory, made as it starts, its writes thrown "
	  "away with it", NULL },
	{ "source.ref.overlay", HULL_SUP_REJECTED,
	  "not yet: a stored tree would be checked out to a directory first", NULL },
	{ "projection.bind",    HULL_SUP_NATIVE,   "the directory mounted as a drive", NULL },
	{ "writeback.none",     HULL_SUP_NATIVE,   NULL, NULL },

	{ "command",            HULL_SUP_EMULATED,
	  "a DOS command line, run by CALL from a batch of hull's under FreeDOS: what a program "
	  "writes through DOS is its output, redirected, not what it draws on the screen; a "
	  "command not found ends 127", NULL },
	{ "workdir",            HULL_SUP_NATIVE,   "a directory of C:", NULL },
	{ "env",                HULL_SUP_NATIVE,   "SET in the batch", NULL },
	{ "volumes",            HULL_SUP_NATIVE,
	  "a drive each by LREDIR, its guest path the letter: /g is G:; /d, /e and /f are "
	  "dosemu2's, /h hull's, and no path may hold a space", NULL },
	{ "media",              HULL_SUP_NATIVE,
	  "a CD-ROM drive each, by LREDIR -C, at the first drive from G: no volume holds; with "
	  "none, dosemu2's MSCDEX answers with a stub drive at Z:", NULL },
	{ "media.dir",          HULL_SUP_EMULATED,
	  "the collection's files/ as the disc's tree, redirected as a CD-ROM, its label not "
	  "kept; one with audio tracks or quirks/ refused, dosemu2 playing no CD audio and "
	  "reading no disc raw", NULL },
	{ "media.image",        HULL_SUP_REJECTED,
	  "dosemu2 mounts no disc image; a dir: collection of the disc's files instead", NULL },
	{ "media.tar",          HULL_SUP_REJECTED, "not yet: a tar: collection", NULL },
	{ "media.pack",         HULL_SUP_REJECTED, "not yet: a pack: collection", NULL },
	{ "network.none",       HULL_SUP_NATIVE,   "a DOS machine here has no network", NULL },
	{ "security.no_new_privs", HULL_SUP_NATIVE, "set on the dosemu2 process", NULL },
	{ "on_disconnect.stop", HULL_SUP_NATIVE,   NULL, NULL },
	{ "on_disconnect.keep", HULL_SUP_REJECTED,
	  "not yet: dosemu2 is a child of the core, and dies with it", NULL },
	{ "hostname",           HULL_SUP_REJECTED, "DOS has no hostname", NULL },

	{ NULL,                 HULL_SUP_NATIVE,   NULL, NULL },
};

static const char *const classes[] = { "dos", NULL };
static const char *const media[]   = { "cdrom", NULL };

static const hull_capabilities caps = {
	.struct_size    = sizeof (hull_capabilities),
	.engine_name    = "dosemu",
	.engine_version = "0.1",
	.classes        = classes,
	.entries        = entries,
	.media_types    = media,
	.migration      = HULL_MIG_PINNED,
	.arches         = NULL,
	.requires_root  = 0,
};

static const hull_engine_ops ops = {
	.struct_size = sizeof (hull_engine_ops),
	.create      = dosemu_create,
	.start       = hull_dos_start,
	.stop        = hull_dos_stop,
	.destroy     = hull_dos_destroy,
	.signal      = hull_dos_signal,
	.stats       = hull_dos_stats,
	.pause       = hull_dos_pause,
};

static const hull_engine engine = {
	.struct_size = sizeof (hull_engine),
	.abi_major   = HULL_ABI_MAJOR,
	.abi_minor   = HULL_ABI_MINOR,
	.caps        = &caps,
	.ops         = &ops,
};

const hull_engine *hull_engine_dosemu(const hull_host_ops *host_ops)
{
	if (host_ops != NULL)
		hull_dos_host = host_ops;
	return &engine;
}
