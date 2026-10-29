/*
 * dosbox.c -- a dos class engine: a machine run by DOSBox, for software
 * written for DOS (project.md section 5.0). What the dos engines share is
 * dos.c; this is DOSBox's own.
 *
 * DOSBox's autoexec mounts the machine's directory as C:, the engine's
 * own as H: and each volume at its letter, and runs H:\HULL.BAT: SET for
 * each env entry, C: and the workdir, the command by CALL with what it
 * writes through DOS redirected to H:\OUT.TXT, then its ERRORLEVEL
 * written to H:\STATUS.TXT by a cascade of IF ERRORLEVEL, since DOSBox's
 * own exit says nothing of it, and EXIT. CALL is what lets a batch for a
 * command return to HULL.BAT; DOS does not redirect a batch's own lines,
 * so only what a program writes through DOS is the machine's output,
 * never what it draws on the screen.
 *
 * DOSBox runs with SDL's dummy video and audio, no window and no sound.
 */

#define _GNU_SOURCE

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "access.h"
#include "builtin.h"
#include "dos.h"

/* `text` written whole to `path`, made for the user alone. */
static int write_file(const char *path, const char *text)
{
	int    fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
	size_t length = strlen(text);
	int    ok = fd >= 0 && write(fd, text, length) == (ssize_t)length;

	if (fd >= 0 && close(fd) != 0)
		ok = 0;
	return ok ? 0 : -1;
}

/* HULL.BAT and dosbox.conf in the machine's directory: 0, or -1. */
static int write_files(const hull_machine *m)
{
	char   path[4200];
	char  *text = malloc(64u * 1024u);
	size_t at = 0u;
	int    ok;

#define ADD(...) at += (size_t)snprintf(text + at, 64u * 1024u - at, __VA_ARGS__)
	if (text == NULL)
		return -1;
	ADD("@echo off\r\n");
	for (unsigned i = 0u; i < HULL_DOS_MAX_ENV && m->envp[i] != NULL; i++)
		ADD("set %s\r\n", m->envp[i]);
	ADD("C:\r\ncd \\");
	for (const char *c = m->workdir + (m->workdir[0] == '/'); *c != '\0'; c++)
		ADD("%c", *c == '/' ? '\\' : *c);
	ADD("\r\ncall");
	for (unsigned i = 0u; i < HULL_DOS_MAX_ARGS && m->argv[i] != NULL; i++)
		ADD(" %s", m->argv[i]);
	ADD(" > H:\\OUT.TXT\r\n");
	/* ERRORLEVEL n is true from n up, so asked from the top down. */
	for (unsigned n = 255u; n > 0u; n--)
		ADD("if errorlevel %u goto s%u\r\n", n, n);
	ADD("echo S0>H:\\STATUS.TXT\r\ngoto end\r\n");
	for (unsigned n = 255u; n > 0u; n--)
		ADD(":s%u\r\necho S%u>H:\\STATUS.TXT\r\ngoto end\r\n", n, n);
	ADD(":end\r\nexit\r\n");
	snprintf(path, sizeof path, "%s/HULL.BAT", m->dir);
	ok = write_file(path, text) == 0;

	at = 0u;
	ADD("[autoexec]\nmount c \"%s\"\nmount h \"%s\"\n", m->root, m->dir);
	for (unsigned i = 0u; i < m->volume_count; i++)
		ADD("mount %c \"%s\"\n", m->volume_drive[i], m->volume_host[i]);
	for (unsigned i = 0u; i < m->media_count; i++)
		ADD(m->media_image[i] ? "imgmount %c \"%s\" -t iso\n" : "mount %c \"%s\" -t cdrom\n",
		    m->media_drive[i], m->media_host[i]);
	ADD("h:\\hull.bat\n");
	snprintf(path, sizeof path, "%s/dosbox.conf", m->dir);
	ok = ok && write_file(path, text) == 0;
#undef ADD
	free(text);
	return ok ? 0 : -1;
}

static void exec_child(const hull_machine *m)
{
	char        conf[4200];
	char        home[4200 + 8];
	char        path_entry[8200];
	const char *path = getenv("PATH");

	snprintf(conf, sizeof conf, "%s/dosbox.conf", m->dir);
	snprintf(home, sizeof home, "HOME=%s", m->dir);
	snprintf(path_entry, sizeof path_entry, "PATH=%s",
	         path != NULL ? path : "/usr/local/bin:/usr/bin:/bin");
	{
		char *const argv[] = { "dosbox", "-conf", conf, NULL };
		char *const envp[] = { home, path_entry, "SDL_VIDEODRIVER=dummy",
		                       "SDL_AUDIODRIVER=dummy", NULL };

		hull_access_execute(argv, envp);
	}
}

/* STATUS.TXT's `Sn`, the batch's word; -1 where it left none. */
static int end_code(const hull_machine *m, int wstatus)
{
	char  said[16];
	FILE *file = NULL;
	int   fd = hull_dos_open(m, "STATUS.TXT");
	int   code = -1;

	(void)wstatus;
	if (fd >= 0 && (file = fdopen(fd, "r")) == NULL)
		close(fd);
	if (file != NULL) {
		if (fgets(said, sizeof said, file) != NULL && said[0] == 'S')
			code = atoi(said + 1);
		fclose(file);
	}
	return code;
}

static const hull_dos_engine flavour = {
	.name         = "dosbox",
	.reserved     = "",
	.reserved_why = "",
	.not_found    = "Illegal command: ",
	.space_free   = 0,
	.images       = 1,
	.write_files  = write_files,
	.exec_child   = exec_child,
	.end_code     = end_code,
};

static hull_status dosbox_create(hull_core *core, hull_buf plan, hull_machine **out,
                                 hull_error *error)
{
	return hull_dos_create(&flavour, core, plan, out, error);
}

static const hull_cap_entry entries[] = {
	/* The directory is drive C:, written in place, as a DOS PC's disk is. */
	{ "source.dir.direct",  HULL_SUP_NATIVE,   "the directory is drive C:", NULL },
	{ "source.dir.overlay", HULL_SUP_EMULATED,
	  "a copy of the tree in the machine's directory, made as it starts, its writes thrown "
	  "away with it", NULL },
	{ "source.ref.overlay", HULL_SUP_REJECTED,
	  "not yet: a stored tree would be checked out to a directory first", NULL },
	{ "projection.bind",    HULL_SUP_NATIVE,   "the directory mounted as a drive", NULL },
	{ "writeback.none",     HULL_SUP_NATIVE,   NULL, NULL },

	{ "command",            HULL_SUP_EMULATED,
	  "a DOS command line, run by CALL from a batch of hull's: what a program writes through "
	  "DOS is its output, redirected, not what it draws on the screen, and a batch's own lines "
	  "are not, DOS redirecting none; a command not found ends 127", NULL },
	{ "workdir",            HULL_SUP_NATIVE,   "a directory of C:", NULL },
	{ "env",                HULL_SUP_NATIVE,   "SET in the batch; DOS makes the names upper case",
	  NULL },
	{ "volumes",            HULL_SUP_NATIVE,
	  "a drive each, its guest path the letter: /d is D:, and /h is hull's", NULL },
	{ "media",              HULL_SUP_NATIVE,
	  "a CD-ROM drive each, by DOSBox's own MSCDEX, at the first drive from D: no volume "
	  "holds", NULL },
	{ "media.dir",          HULL_SUP_EMULATED,
	  "the collection's files/ as the disc's tree, mounted as a CD-ROM; its TOC, audio tracks "
	  "and quirks are not yet replayed, so a copy-protection check will fail", NULL },
	{ "media.image",        HULL_SUP_NATIVE,   "an ISO image, by imgmount", NULL },
	{ "media.tar",          HULL_SUP_REJECTED, "not yet: a tar: collection", NULL },
	{ "media.pack",         HULL_SUP_REJECTED, "not yet: a pack: collection", NULL },
	{ "network.none",       HULL_SUP_NATIVE,   "a DOS machine here has no network", NULL },
	{ "security.no_new_privs", HULL_SUP_NATIVE, "set on the DOSBox process", NULL },
	{ "on_disconnect.stop", HULL_SUP_NATIVE,   NULL, NULL },
	{ "on_disconnect.keep", HULL_SUP_REJECTED,
	  "not yet: DOSBox is a child of the core, and dies with it", NULL },
	{ "hostname",           HULL_SUP_REJECTED, "DOS has no hostname", NULL },

	{ NULL,                 HULL_SUP_NATIVE,   NULL, NULL },
};

static const char *const classes[] = { "dos", NULL };
static const char *const media[]   = { "cdrom", NULL };

static const hull_capabilities caps = {
	.struct_size    = sizeof (hull_capabilities),
	.engine_name    = "dosbox",
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
	.create      = dosbox_create,
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

const hull_engine *hull_engine_dosbox(const hull_host_ops *host_ops)
{
	if (host_ops != NULL)
		hull_dos_host = host_ops;
	return &engine;
}
