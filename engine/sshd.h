/*
 * sshd.h -- the sshd hull fetches for a machine whose image has none
 * (section 8.1 item 3): Debian's dropbear, the libraries it links, the C
 * library and its loader, and OpenSSH's sftp-server, each package pinned by
 * version and sha256, unpacked into the user's cache by `hull sshd fetch`.
 * Nothing of it is shipped in the tree (section 9). In a machine it is at
 * HULL_SSHD_GUEST, read-only, and run through its own loader, so it needs
 * nothing of the image's but a passwd entry and a shell for the login.
 *
 * Dropbear runs `sftp-server` for the sftp subsystem from a path compiled
 * in, /usr/lib/sftp-server, which is the image's to have or not; the
 * fetched binary has that path rewritten, once and to the same length, to
 * HULL_SSHD_GUEST "/sftp", and is pinned again by the sha256 that gives.
 */
#ifndef HULL_ENGINE_SSHD_H
#define HULL_ENGINE_SSHD_H

#include <fcntl.h>
#include <signal.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#define HULL_SSHD_VERSION "2025.89-1~deb13u1"
#define HULL_SSHD_POOL    "https://deb.debian.org/debian/pool/main/"

typedef struct hull_sshd_package {
	const char *path;   /* under HULL_SSHD_POOL */
	const char *sha256; /* the .deb's */
} hull_sshd_package;

static const hull_sshd_package hull_sshd_packages[] = {
	{ "d/dropbear/dropbear-bin_2025.89-1~deb13u1_amd64.deb",
	  "1ddbd76749c7076097fb899eaf55f0a398d6f62799a6000501b32ed7b5d9257b" },
	{ "libt/libtomcrypt/libtomcrypt1_1.18.2+dfsg-7+b2_amd64.deb",
	  "704174c153914d76b8924f35113042f437abddd7f281da9fca8861defb707a01" },
	{ "libt/libtommath/libtommath1_1.3.0-1_amd64.deb",
	  "0e6025c5c3c07c7055993eb23fd1eb8e55ec037ef46eca2e7622825e85f56a60" },
	{ "g/gmp/libgmp10_6.3.0+dfsg-3_amd64.deb",
	  "d0d0265eb01770f17afd0f7c8c0622f80479dcfbbe13653a0debeec61464e622" },
	{ "z/zlib/zlib1g_1.3.dfsg+really1.3.1-1+b1_amd64.deb",
	  "015be740d6236ad114582dea500c1d907f29e16d6db00566ca32fb68d71ac90d" },
	{ "libx/libxcrypt/libcrypt1_4.4.38-1_amd64.deb",
	  "0ebc144d662e3197982d1bf3a7b8b35ca845e54c68811de0328b1f0d7c67585c" },
	{ "g/glibc/libc6_2.41-12+deb13u4_amd64.deb",
	  "967aa62605721081c3eb2a17650611a792aa802d76a6511d1840242623d204c9" },
	{ "o/openssh/openssh-sftp-server_10.0p1-7+deb13u4_amd64.deb",
	  "28a82485438fafc8d6a7552ab61cac827338c16f39e3790d6ebcc146d452e542" },
};

/* dropbear as Debian builds it, and as hull runs it: its two paths
 * rewritten (above). */
#define HULL_SSHD_DROPBEAR_SHA256 "7896833aa1d4eb0d4edf7cbefe91dce6dd0a9795a1610032a9ef7d01dac13c75"
#define HULL_SSHD_PATCHED_SHA256  "1454240f4e25f28cf4050fc006d38c9be4d37d10fc896d599a3c4d1c75261a3f"
#define HULL_SSHD_SFTP_FROM       "/usr/lib/sftp-server"
#define HULL_SSHD_INTERP_FROM     "/lib64/ld-linux-x86-64.so.2"

/* Where a machine has it, and the files in it a machine runs. */
#define HULL_SSHD_GUEST       "/run/hull/sshd"
#define HULL_SSHD_LIBS        "usr/lib/x86_64-linux-gnu"
#define HULL_SSHD_LOADER      HULL_SSHD_LIBS "/ld-linux-x86-64.so.2"
#define HULL_SSHD_DROPBEAR    "usr/sbin/dropbear"
#define HULL_SSHD_CONVERT     "usr/bin/dropbearconvert"
#define HULL_SSHD_SFTP_SERVER "usr/lib/openssh/sftp-server"
#define HULL_SSHD_SFTP        "sftp"
#define HULL_SSHD_INTERP      "ld.so"

/* The fetched sshd's directory, $XDG_CACHE_HOME/hull/dropbear-VERSION or
 * under ~/.cache. 0, or -1 where neither variable names a place. */
static inline int hull_sshd_dir(char *out, size_t cap)
{
	const char *cache = getenv("XDG_CACHE_HOME");
	const char *home = getenv("HOME");

	if (cache != NULL && cache[0] == '/')
		snprintf(out, cap, "%s/hull/dropbear-%s", cache, HULL_SSHD_VERSION);
	else if (home != NULL && home[0] == '/')
		snprintf(out, cap, "%s/.cache/hull/dropbear-%s", home, HULL_SSHD_VERSION);
	else
		return -1;
	return 0;
}

/*
 * In the machine, after its root is in place: the image's own sshd, the
 * first of the usual two paths that is executable, into `found` (NULL
 * where it has none and the fetched dropbear will serve); or a reason the
 * machine cannot have one. dropbear logs in as the image's root, so a
 * passwd that does not name root, or a / that others may write, which
 * dropbear holds against every key, is refused rather than given an sshd
 * no key can log in to. System calls alone, as below.
 */
static inline const char *hull_sshd_choose(const char **found)
{
	static const char *const where[] = { "/usr/sbin/sshd", "/usr/bin/sshd", NULL };
	static char              text[65536];
	struct stat              root;
	ssize_t                  got = 0;
	int                      named = 0;
	int                      fd;

	*found = NULL;
	for (unsigned i = 0u; where[i] != NULL; i++)
		if (access(where[i], X_OK) == 0) {
			*found = where[i];
			return NULL;
		}
	if (access(HULL_SSHD_GUEST "/" HULL_SSHD_DROPBEAR, R_OK) != 0)
		return "(ssh true): the image has no sshd at /usr/sbin/sshd or /usr/bin/sshd, and "
		       "none has been fetched: `hull sshd fetch` downloads dropbear, once";
	/* Read by the system calls alone, as hull_sshd_exec_dropbear is run:
	 * chroot calls this in its init too. Its first 64 KiB, which hold
	 * root's line wherever a passwd has one. */
	if ((fd = open("/etc/passwd", O_RDONLY | O_CLOEXEC)) >= 0) {
		got = read(fd, text, sizeof text - 1u);
		close(fd);
	}
	for (ssize_t at = 0; got > 0 && at + 5 <= got && !named; at++)
		named = (at == 0 || text[at - 1] == '\n') && memcmp(text + at, "root:", 5u) == 0;
	if (!named)
		return "(ssh true): the image has no sshd, and the fetched dropbear logs in as root, "
		       "whom its /etc/passwd does not name";
	if (stat("/", &root) != 0 || (root.st_mode & 022) != 0)
		return "(ssh true): the image has no sshd, and the fetched dropbear takes no key where "
		       "the machine's / may be written by group or others, as OpenSSH's StrictModes "
		       "takes none: `chmod go-w` the directory the machine is of";
	return NULL;
}

/*
 * The fetched dropbear, in the child that is to become it, listening on
 * `listen` (dropbear's -p: [ADDRESS:]PORT). Its loader is its own by its
 * interpreter and its libraries by the path, which its run for each
 * connection inherits and a login does not, dropbear giving a login an
 * environment of its own. The machine's host key is the core's, in
 * OpenSSH's form, which dropbear does not read: converted first, by the
 * fetched dropbearconvert, into a file of root's beside the material.
 * Returns only where neither could be run, having said why on stderr.
 * Only system calls, no fork() or stdio of glibc's: chroot's init, a raw
 * clone of the core, may hold a lock a core thread had (chroot.c's
 * bare_fork).
 */
#define HULL_SSHD_AT(file) HULL_SSHD_GUEST "/" file
static inline void hull_sshd_exec_dropbear(const char *listen)
{
	static const char key[] = "/run/hull/dropbear_host_key";
	static char      *envp[] = { "LD_LIBRARY_PATH=" HULL_SSHD_AT(HULL_SSHD_LIBS), NULL };
	char             *argv[] = { "dropbear", "-F", "-E", "-s", "-r", (char *)key, "-p",
	                             (char *)listen, "-D", "/run/hull/ssh",
	                             "-P", "/run/hull/dropbear.pid", NULL };
	pid_t             convert;
	int               status;

	umask(077);
	unlink(key);
	convert = (pid_t)syscall(SYS_clone, SIGCHLD, NULL, NULL, NULL, 0L);
	if (convert == 0) {
		char *const run[] = { "dropbearconvert", "openssh", "dropbear",
		                      "/run/hull/ssh/host_key", (char *)key, NULL };

		execve(HULL_SSHD_AT(HULL_SSHD_CONVERT), run, envp);
		_exit(127);
	}
	if (convert < 0 || waitpid(convert, &status, 0) != convert || !WIFEXITED(status) ||
	    WEXITSTATUS(status) != 0) {
		static const char said[] = "hull: dropbear: the machine's host key could not be "
		                           "converted for it\n";

		if (write(2, said, sizeof said - 1u) < 0)
			return;
		return;
	}
	execve(HULL_SSHD_AT(HULL_SSHD_DROPBEAR), argv, envp);
	{
		static const char said[] = "hull: dropbear could not be run\n";

		if (write(2, said, sizeof said - 1u) < 0)
			return;
	}
}

#endif
