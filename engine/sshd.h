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

typedef struct hull_sshd_package {
	const char *path;   /* under kernel.h's HULL_DEBIAN_POOL and _SNAPSHOT */
	const char *sha256; /* the .deb's */
} hull_sshd_package;

/* amd64's, the host's, and arm64's for an arm64 machine under emu: the
 * same versions, each .deb's sha256 from Debian's index for trixie as its
 * signed Release names it, arm64's read 2026-11-09. */
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

static const hull_sshd_package hull_sshd_packages_arm64[] = {
	{ "d/dropbear/dropbear-bin_2025.89-1~deb13u1_arm64.deb",
	  "1cfab9f1d5dbf89a8ad790cafe5585a8e08a7c0e0ce098c7a89f957a6428976e" },
	{ "libt/libtomcrypt/libtomcrypt1_1.18.2+dfsg-7+b2_arm64.deb",
	  "30c14959f4ae9ba1b41dab47f6d7ca212b7cf45ce340d1a3a52e1fd25a5b643b" },
	{ "libt/libtommath/libtommath1_1.3.0-1_arm64.deb",
	  "7e53ae5d16d06801b1c0c88e2fcb3b38e08d76aa3855bcbd6abc8e9639cb6945" },
	{ "g/gmp/libgmp10_6.3.0+dfsg-3_arm64.deb",
	  "a27bbc27f119161ea9702c8dd66f54131cdf0d2ca73000f50ea91ef2fdfef0fb" },
	{ "z/zlib/zlib1g_1.3.dfsg+really1.3.1-1+b1_arm64.deb",
	  "209aa5cf671e97b9eb0410844fa6df4cae2e75b0c72e7802ab6c8ece13e6ddef" },
	{ "libx/libxcrypt/libcrypt1_4.4.38-1_arm64.deb",
	  "b9aaa808aa3812b3de3e54eb5dfb16762c1779db032604abef2354c02fdc29b8" },
	{ "g/glibc/libc6_2.41-12+deb13u4_arm64.deb",
	  "8784eda966b189c777a384dac5ce009e8fc9b52d006926c5a013e7fa8aa688cc" },
	{ "o/openssh/openssh-sftp-server_10.0p1-7+deb13u4_arm64.deb",
	  "c949831340f9af9e70a6dab740fc9cdff6dd0b249bf9d7d640f6486036476e05" },
};

/* What differs between the two: dropbear as Debian builds it and as hull
 * runs it, its two paths rewritten (above); the loader it names; where
 * the libraries are. */
typedef struct hull_sshd_arch {
	const char              *name;
	const hull_sshd_package *packages;
	size_t                   count;
	const char              *dropbear_sha256;
	const char              *patched_sha256;
	const char              *interp_from;
	const char              *libs;
	const char              *loader;
} hull_sshd_arch;

#define HULL_SSHD_LIBS_AMD64   "usr/lib/x86_64-linux-gnu"
#define HULL_SSHD_LOADER_AMD64 HULL_SSHD_LIBS_AMD64 "/ld-linux-x86-64.so.2"
#define HULL_SSHD_LIBS_ARM64   "usr/lib/aarch64-linux-gnu"
#define HULL_SSHD_LOADER_ARM64 HULL_SSHD_LIBS_ARM64 "/ld-linux-aarch64.so.1"

static const hull_sshd_arch hull_sshd_arches[] = {
	{ "amd64", hull_sshd_packages, sizeof hull_sshd_packages / sizeof hull_sshd_packages[0],
	  "7896833aa1d4eb0d4edf7cbefe91dce6dd0a9795a1610032a9ef7d01dac13c75",
	  "1454240f4e25f28cf4050fc006d38c9be4d37d10fc896d599a3c4d1c75261a3f",
	  "/lib64/ld-linux-x86-64.so.2", HULL_SSHD_LIBS_AMD64, HULL_SSHD_LOADER_AMD64 },
	{ "arm64", hull_sshd_packages_arm64,
	  sizeof hull_sshd_packages_arm64 / sizeof hull_sshd_packages_arm64[0],
	  "800a3be86c192a94008853f033f66bf505ca457fdc20d4cdb703d021afcb7750",
	  "c7403dbc89a3f536682f19b5f64ba0a6b9f5575dda8d4713d2f90a7f91556a93",
	  "/lib/ld-linux-aarch64.so.1", HULL_SSHD_LIBS_ARM64, HULL_SSHD_LOADER_ARM64 },
};

/* `arch`'s, or NULL where hull fetches none for it. */
static inline const hull_sshd_arch *hull_sshd_arch_of(const char *arch)
{
	for (size_t i = 0u; i < sizeof hull_sshd_arches / sizeof hull_sshd_arches[0]; i++)
		if (strcmp(hull_sshd_arches[i].name, arch) == 0)
			return &hull_sshd_arches[i];
	return NULL;
}

#define HULL_SSHD_SFTP_FROM "/usr/lib/sftp-server"

/* Where a machine has it, and the files in it a machine runs; the
 * libraries and the loader are those of the arch this is built for, the
 * agent's in a machine, the host's in chroot's init and docker's. */
#define HULL_SSHD_GUEST       "/run/hull/sshd"
#if defined(__aarch64__)
#define HULL_SSHD_LIBS        HULL_SSHD_LIBS_ARM64
#define HULL_SSHD_LOADER      HULL_SSHD_LOADER_ARM64
#define HULL_SSHD_FETCH_ARCH  " --arch arm64"
#else
#define HULL_SSHD_LIBS        HULL_SSHD_LIBS_AMD64
#define HULL_SSHD_LOADER      HULL_SSHD_LOADER_AMD64
#define HULL_SSHD_FETCH_ARCH  ""
#endif
#define HULL_SSHD_DROPBEAR    "usr/sbin/dropbear"
#define HULL_SSHD_CONVERT     "usr/bin/dropbearconvert"
#define HULL_SSHD_SFTP_SERVER "usr/lib/openssh/sftp-server"
#define HULL_SSHD_SFTP        "sftp"
#define HULL_SSHD_INTERP      "ld.so"

/* `arch`'s fetched sshd's directory, $XDG_CACHE_HOME/hull/dropbear-VERSION
 * for amd64 and dropbear-ARCH-VERSION for another, or under ~/.cache. 0,
 * or -1 where neither variable names a place. */
static inline int hull_sshd_dir_of(const char *arch, char *out, size_t cap)
{
	const char *cache = getenv("XDG_CACHE_HOME");
	const char *home = getenv("HOME");
	char        name[96];

	if (strcmp(arch, "amd64") == 0)
		snprintf(name, sizeof name, "dropbear-%s", HULL_SSHD_VERSION);
	else
		snprintf(name, sizeof name, "dropbear-%s-%s", arch, HULL_SSHD_VERSION);
	if (cache != NULL && cache[0] == '/')
		snprintf(out, cap, "%s/hull/%s", cache, name);
	else if (home != NULL && home[0] == '/')
		snprintf(out, cap, "%s/.cache/hull/%s", home, name);
	else
		return -1;
	return 0;
}

/* The host's, amd64's. */
static inline int hull_sshd_dir(char *out, size_t cap)
{
	return hull_sshd_dir_of("amd64", out, cap);
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
		       "none has been fetched: `hull sshd fetch" HULL_SSHD_FETCH_ARCH "` downloads "
		       "dropbear, once";
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
		/* Through the fetched loader, which dropbear's own interpreter
		 * names and dropbearconvert's does not: it names the image's,
		 * which an image of another C library, or of none, lacks. */
		char *const run[] = { "ld.so", (char *)HULL_SSHD_AT(HULL_SSHD_CONVERT), "openssh",
		                      "dropbear", "/run/hull/ssh/host_key", (char *)key, NULL };

		execve(HULL_SSHD_AT(HULL_SSHD_LOADER), run, envp);
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
