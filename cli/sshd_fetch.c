/*
 * sshd_fetch.c -- `hull sshd fetch` (section 8.1 item 3): the sshd a
 * machine is given where its image has none. engine/sshd.h's packages are
 * downloaded and checked as `hull kernel fetch`'s is, applied in order into
 * one tree by the store's layer reader, their documentation and the C
 * library's character-set converters left out, dropbear's sftp-server and
 * loader paths rewritten to hull's, and the script and the link those
 * paths name made beside them. amd64's for the host's machines, and
 * arm64's, `--arch arm64`, for an arm64 machine under emu.
 * Nothing in it is run on the host.
 */
#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "kernel.h"
#include "kernel_fetch.h"
#include "sha256.h"
#include "sshd.h"
#include "sshd_fetch.h"
#include "store.h"

/* `n` bytes' sha256, as hex. */
static void file_sha256(const uint8_t *data, size_t n, char hex[65])
{
	hull_sha256 hash;
	uint8_t     digest[32];

	hull_sha256_init(&hash);
	hull_sha256_update(&hash, data, n);
	hull_sha256_final(&hash, digest);
	hull_sha256_hex(digest, hex);
}

/*
 * dropbear's two paths that name the image's files rewritten to hull's,
 * each found once with its NUL and overwritten NUL-padded to the same
 * length: the sftp-server it runs, and its ELF interpreter, the loader,
 * since dropbear runs itself again for each connection and that run is the
 * kernel's, not the loader's it was started by. The binary is Debian's as
 * the package's sha256 said, and is held to hull's pin after.
 */
static int patch_dropbear(const hull_sshd_arch *arch, const char *path, char *why, size_t cap)
{
	const struct {
		const char *from;
		const char *to;
	} rewrites[] = {
		{ HULL_SSHD_SFTP_FROM, HULL_SSHD_GUEST "/" HULL_SSHD_SFTP },
		{ arch->interp_from, HULL_SSHD_GUEST "/" HULL_SSHD_INTERP },
	};
	uint8_t    *data = NULL;
	char        hex[65];
	struct stat is;
	size_t      n;
	int         fd = open(path, O_RDWR | O_CLOEXEC);
	int         status = -1;

	if (fd < 0 || fstat(fd, &is) != 0 || is.st_size <= 0) {
		snprintf(why, cap, "the fetched dropbear cannot be read: %s", strerror(errno));
		goto done;
	}
	n = (size_t)is.st_size;
	if ((data = malloc(n)) == NULL || pread(fd, data, n, 0) != (ssize_t)n) {
		snprintf(why, cap, "the fetched dropbear cannot be read");
		goto done;
	}
	file_sha256(data, n, hex);
	if (strcmp(hex, arch->dropbear_sha256) != 0) {
		snprintf(why, cap, "the fetched dropbear is not the one pinned: sha256 %s", hex);
		goto done;
	}
	for (size_t r = 0u; r < sizeof rewrites / sizeof rewrites[0]; r++) {
		size_t   from = strlen(rewrites[r].from) + 1u; /* with its NUL, so a longer
		                                                * string beginning with it
		                                                * is not taken for it */
		size_t   to = strlen(rewrites[r].to) + 1u;
		uint8_t *found = NULL;
		unsigned seen = 0u;

		if (to > from) {
			snprintf(why, cap, "%s is longer than the %s it replaces", rewrites[r].to,
			         rewrites[r].from);
			goto done;
		}
		for (size_t at = 0u; at + from <= n; at++)
			if (memcmp(data + at, rewrites[r].from, from) == 0) {
				found = data + at;
				seen++;
			}
		if (seen != 1u) {
			snprintf(why, cap, "the fetched dropbear names %s %u times, not once",
			         rewrites[r].from, seen);
			goto done;
		}
		memset(found, 0, from);
		memcpy(found, rewrites[r].to, to);
	}
	file_sha256(data, n, hex);
	if (strcmp(hex, arch->patched_sha256) != 0) {
		snprintf(why, cap, "dropbear rewritten is not the one pinned: sha256 %s", hex);
		goto done;
	}
	if (pwrite(fd, data, n, 0) != (ssize_t)n) {
		snprintf(why, cap, "the fetched dropbear could not be rewritten: %s", strerror(errno));
		goto done;
	}
	status = 0;
done:
	free(data);
	if (fd >= 0)
		close(fd);
	return status;
}

/* The loader the rewritten interpreter names, at the tree's top. */
static int link_loader(const hull_sshd_arch *arch, const char *tree, char *why, size_t cap)
{
	char path[4400];

	snprintf(path, sizeof path, "%s/%s", tree, HULL_SSHD_INTERP);
	if (symlink(arch->loader, path) != 0) {
		snprintf(why, cap, "%s: %s", path, strerror(errno));
		return -1;
	}
	return 0;
}

/*
 * The path dropbear runs for sftp. dropbear runs a command through the
 * user's shell, `SHELL -c PATH`, so the script has no #! line: a shell
 * given a file the kernel will not execute reads it as a script itself,
 * which every POSIX shell does, and no /bin/sh is assumed of the image.
 */
static int write_sftp(const hull_sshd_arch *arch, const char *tree, char *why, size_t cap)
{
	char   script[600];
	char   path[4400];
	size_t n;
	int    fd;

	snprintf(script, sizeof script, "exec %s/%s --library-path %s/%s %s/%s \"$@\"\n",
	         HULL_SSHD_GUEST, arch->loader, HULL_SSHD_GUEST, arch->libs, HULL_SSHD_GUEST,
	         HULL_SSHD_SFTP_SERVER);
	n = strlen(script);
	snprintf(path, sizeof path, "%s/%s", tree, HULL_SSHD_SFTP);
	fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0755);
	if (fd < 0 || write(fd, script, n) != (ssize_t)n) {
		snprintf(why, cap, "%s: %s", path, strerror(errno));
		if (fd >= 0)
			close(fd);
		return -1;
	}
	return close(fd);
}

int hull_sshd_fetch(const char *arch_name, char *why, size_t cap)
{
	const hull_sshd_arch *arch = hull_sshd_arch_of(arch_name);
	char         dir[4096];
	char         work[4200];
	char         deb[4300], tar[4300], tree[4300], made[4400];
	uint64_t     skipped = 0u;
	hull_unpack *u = NULL;
	int          status = -1;

	if (arch == NULL) {
		snprintf(why, cap, "hull fetches no sshd for %s: amd64 and arm64", arch_name);
		return -1;
	}
	if (hull_sshd_dir_of(arch->name, dir, sizeof dir) != 0) {
		snprintf(why, cap, "neither XDG_CACHE_HOME nor HOME names a place for the sshd");
		return -1;
	}
	snprintf(made, sizeof made, "%s/%s", dir, HULL_SSHD_DROPBEAR);
	if (access(made, R_OK) == 0) {
		printf("%s\n", dir);
		return 0;
	}
	/* Beside where it goes, so the last step is a rename. */
	for (char *slash = strchr(dir + 1, '/'); slash != NULL; slash = strchr(slash + 1, '/')) {
		*slash = '\0';
		if (mkdir(dir, 0755) != 0 && errno != EEXIST) {
			snprintf(why, cap, "%s: %s", dir, strerror(errno));
			return -1;
		}
		*slash = '/';
	}
	snprintf(work, sizeof work, "%s.part-XXXXXX", dir);
	if (mkdtemp(work) == NULL) {
		snprintf(why, cap, "%s: %s", work, strerror(errno));
		return -1;
	}
	snprintf(deb, sizeof deb, "%s/package.deb", work);
	snprintf(tar, sizeof tar, "%s/data.tar", work);
	snprintf(tree, sizeof tree, "%s/tree", work);
	if (mkdir(tree, 0755) != 0) {
		snprintf(why, cap, "%s: %s", tree, strerror(errno));
		goto done;
	}
	if ((u = hull_unpack_begin(tree, why, cap)) == NULL)
		goto done;
	fprintf(stderr, "hull: fetching Debian's %s dropbear %s and what it links, about 4 MB, "
	        "from %s\n", arch->name, HULL_SSHD_VERSION, HULL_DEBIAN_POOL);
	for (size_t i = 0u; i < arch->count; i++) {
		int fd;
		int applied;

		if (hull_fetch_deb(arch->packages[i].path, arch->packages[i].sha256, deb, tar,
		                   why, cap) != 0)
			goto done;
		if ((fd = open(tar, O_RDONLY | O_CLOEXEC)) < 0) {
			snprintf(why, cap, "%s: %s", tar, strerror(errno));
			goto done;
		}
		applied = hull_unpack_layer(u, fd, &skipped, why, cap);
		close(fd);
		unlink(tar);
		if (applied != 0)
			goto done;
	}
	{
		hull_unpack *ending = u;

		u = NULL;
		if (hull_unpack_end(ending, why, cap) != 0)
			goto done;
	}
	/* What a machine never reads: documentation, and the converters iconv
	 * loads, half the C library's package. */
	{
		char at[4400];
		int  parent;

		snprintf(at, sizeof at, "%s/usr", tree);
		if ((parent = open(at, O_RDONLY | O_DIRECTORY | O_CLOEXEC)) >= 0) {
			hull_store_remove_tree(parent, "share");
			close(parent);
		}
		snprintf(at, sizeof at, "%s/%s", tree, arch->libs);
		if ((parent = open(at, O_RDONLY | O_DIRECTORY | O_CLOEXEC)) >= 0) {
			hull_store_remove_tree(parent, "gconv");
			close(parent);
		}
	}
	snprintf(made, sizeof made, "%s/%s", tree, HULL_SSHD_DROPBEAR);
	if (patch_dropbear(arch, made, why, cap) != 0 || write_sftp(arch, tree, why, cap) != 0 ||
	    link_loader(arch, tree, why, cap) != 0)
		goto done;
	if (rename(tree, dir) != 0) {
		snprintf(why, cap, "%s: %s", dir, strerror(errno));
		goto done;
	}
	printf("%s\n", dir);
	status = 0;
done:
	if (u != NULL) {
		char ignored[200];

		hull_unpack_end(u, ignored, sizeof ignored);
	}
	unlink(deb);
	unlink(tar);
	/* What did not become the sshd's directory: the tree, made by this
	 * process beneath its own work directory. */
	{
		int parent = open(work, O_RDONLY | O_DIRECTORY | O_CLOEXEC);

		if (parent >= 0) {
			hull_store_remove_tree(parent, "tree");
			close(parent);
		}
	}
	rmdir(work);
	return status;
}
