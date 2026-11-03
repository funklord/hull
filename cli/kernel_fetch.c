/*
 * kernel_fetch.c -- `hull kernel fetch` (Appendix F): the kernel a vm boots
 * where the host's own cannot be read, as Ubuntu keeps /boot/vmlinuz
 * root's alone. Debian's generic amd64 package, pinned in engine/kernel.h
 * by version and sha256, is downloaded, checked, and unpacked into the
 * user's cache: its data.tar.xz read out of the .deb's ar archive and
 * decompressed by the system's liblzma, its tar by the store's layer
 * reader, its modules moved to lib/modules and given their modules.dep by
 * the host's depmod. Nothing of it is shipped in the tree (section 9).
 *
 * The generic kernel rather than Debian's cloud one: the cloud kernel has
 * no 9p, which a vm's shares are, measured 2026-11-03 in its config.
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

#include <lzma.h>

#include "hostnet.h"
#include "https.h"
#include "kernel.h"
#include "kernel_fetch.h"
#include "sha256.h"
#include "store.h"

typedef struct download {
	int         fd;
	hull_sha256 hash;
} download;

static int downloaded(void *context, const uint8_t *data, size_t n, char *why, size_t cap)
{
	download *d = context;

	hull_sha256_update(&d->hash, data, n);
	for (size_t done = 0u; done < n;) {
		ssize_t wrote = write(d->fd, data + done, n - done);

		if (wrote < 0 && errno == EINTR)
			continue;
		if (wrote <= 0) {
			snprintf(why, cap, "the download could not be written: %s", strerror(errno));
			return -1;
		}
		done += (size_t)wrote;
	}
	return 0;
}

/* The member `name` of the ar archive `fd`: its offset and size. 0, or -1
 * where the archive has none. */
static int ar_member(int fd, const char *name, off_t *offset, uint64_t *size)
{
	char  magic[8];
	char  header[60];
	off_t at = 8;

	if (pread(fd, magic, sizeof magic, 0) != (ssize_t)sizeof magic ||
	    memcmp(magic, "!<arch>\n", 8u) != 0)
		return -1;
	while (pread(fd, header, sizeof header, at) == (ssize_t)sizeof header) {
		char               field[17];
		unsigned long long length;
		size_t             n = strlen(name);

		memcpy(field, header + 48, 10u);
		field[10] = '\0';
		length = strtoull(field, NULL, 10);
		if (memcmp(header, name, n) == 0 && (header[n] == ' ' || header[n] == '/')) {
			*offset = at + 60;
			*size = length;
			return 0;
		}
		at += 60 + (off_t)length + (off_t)(length & 1u);
	}
	return -1;
}

/* `size` bytes of `in` from `offset`, xz, into `out` decompressed. */
static int unxz(int in, off_t offset, uint64_t size, int out, char *why, size_t cap)
{
	static uint8_t from[65536];
	static uint8_t to[262144];
	lzma_stream    s = LZMA_STREAM_INIT;
	lzma_ret       ret;
	uint64_t       left = size;

	if (lzma_stream_decoder(&s, UINT64_MAX, 0u) != LZMA_OK) {
		snprintf(why, cap, "liblzma would not start");
		return -1;
	}
	do {
		lzma_action action = LZMA_RUN;

		if (s.avail_in == 0u && left > 0u) {
			ssize_t got = pread(in, from, left < sizeof from ? (size_t)left : sizeof from,
			                    offset);

			if (got <= 0) {
				lzma_end(&s);
				snprintf(why, cap, "the package's data could not be read");
				return -1;
			}
			offset += got;
			left -= (uint64_t)got;
			s.next_in = from;
			s.avail_in = (size_t)got;
		}
		if (left == 0u)
			action = LZMA_FINISH;
		s.next_out = to;
		s.avail_out = sizeof to;
		ret = lzma_code(&s, action);
		if (s.avail_out < sizeof to &&
		    write(out, to, sizeof to - s.avail_out) != (ssize_t)(sizeof to - s.avail_out)) {
			lzma_end(&s);
			snprintf(why, cap, "the package's data could not be written: %s", strerror(errno));
			return -1;
		}
	} while (ret == LZMA_OK);
	lzma_end(&s);
	if (ret != LZMA_STREAM_END) {
		snprintf(why, cap, "the package's data.tar.xz is not whole xz (liblzma said %d)",
		         (int)ret);
		return -1;
	}
	return 0;
}

/* `url` into `d`, from the start, following a redirect within the same
 * scheme up to four times, as the snapshot answers with one to the file by
 * its sha1. The final status into `status`. 0, or -1 with `why`. */
static int get_following(const char *url, download *d, int *status, char *why, size_t cap)
{
	char at[4400];

	snprintf(at, sizeof at, "%s", url);
	for (int hops = 0; hops < 5; hops++) {
		hull_https_response got = { 0 };

		if (ftruncate(d->fd, 0) != 0 || lseek(d->fd, 0, SEEK_SET) != 0) {
			snprintf(why, cap, "the download could not be begun again: %s", strerror(errno));
			return -1;
		}
		hull_sha256_init(&d->hash);
		why[0] = '\0';
		if (hull_https_get(at, NULL, &got, downloaded, d, why, cap) != 0)
			return -1;
		*status = got.status;
		if (got.status < 300 || got.status > 399 || got.location[0] == '\0')
			return 0;
		if (got.location[0] == '/') {
			char        host[256];
			const char *rest = got.location;

			if (hull_https_host(at, host, sizeof host) != 0) {
				snprintf(why, cap, "%s redirected, and its host cannot be read", at);
				return -1;
			}
			snprintf(at, sizeof at, "https://%s%s", host, rest);
		} else if (strncmp(got.location, "https://", 8u) == 0) {
			snprintf(at, sizeof at, "%s", got.location);
		} else {
			snprintf(why, cap, "%s redirected to %s, which is not https", at, got.location);
			return -1;
		}
	}
	snprintf(why, cap, "%s redirected more than four times", url);
	return -1;
}

int hull_fetch_deb(const char *path, const char *sha256, const char *deb, const char *tar,
                   char *why, size_t cap)
{
	const char              *pool = getenv("HULL_DEBIAN_POOL");
	const char              *bases[2] = { HULL_DEBIAN_POOL, HULL_DEBIAN_SNAPSHOT };
	char                     hex[65];
	char                     url[1024] = "";
	uint8_t                  digest[32];
	download                 d;
	off_t                    offset;
	uint64_t                 size;
	int                      out;
	int                      got = 0;
	int                      status = -1;

	if ((d.fd = open(deb, O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0600)) < 0) {
		snprintf(why, cap, "%s: %s", deb, strerror(errno));
		return -1;
	}
	/* HULL_DEBIAN_POOL names another pool, as HULL_BOOT_DIR another
	 * /boot: a test's, one that has nothing, to hold the snapshot to it. */
	if (pool != NULL && strncmp(pool, "https://", 8u) == 0)
		bases[0] = pool;
	/* The pool first, the snapshot where the pool no longer has it. */
	for (unsigned i = 0u; i < sizeof bases / sizeof bases[0] && got != 200; i++) {
		snprintf(url, sizeof url, "%s%s", bases[i], path);
		if (get_following(url, &d, &got, why, cap) != 0)
			goto done;
		if (got != 200 && i == 0u)
			fprintf(stderr, "hull: %s answered %d: taking it from Debian's snapshot\n", url, got);
	}
	if (got != 200) {
		snprintf(why, cap, "%s answered %d, and the pool no longer has it either: the pin "
		         "wants moving", url, got);
		goto done;
	}
	hull_sha256_final(&d.hash, digest);
	hull_sha256_hex(digest, hex);
	if (strcmp(hex, sha256) != 0) {
		snprintf(why, cap, "the package from %s is not the one pinned: sha256 %s, wanted %s",
		         url, hex, sha256);
		goto done;
	}
	/* Its files: data.tar.xz out of the .deb, decompressed. */
	if (ar_member(d.fd, "data.tar.xz", &offset, &size) != 0) {
		snprintf(why, cap, "the package from %s holds no data.tar.xz", url);
		goto done;
	}
	if ((out = open(tar, O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0600)) < 0) {
		snprintf(why, cap, "%s: %s", tar, strerror(errno));
		goto done;
	}
	if (unxz(d.fd, offset, size, out, why, cap) != 0) {
		close(out);
		unlink(tar);
		goto done;
	}
	close(out);
	status = 0;
done:
	close(d.fd);
	unlink(deb);
	return status;
}

int hull_kernel_fetch(char *why, size_t cap)
{
	char                dir[4096];
	char                work[4200];
	char                deb[4300], tar[4300], tree[4300], from[4400], to[4400], made[4400];
	char                depmod[64] = "/usr/sbin/depmod";
	uint64_t            skipped = 0u;
	hull_unpack        *u;
	int                 fd;
	int                 out;
	int                 status = -1;
	struct stat         is;

	if (hull_kernel_dir(dir, sizeof dir) != 0) {
		snprintf(why, cap, "neither XDG_CACHE_HOME nor HOME names a place for the kernel");
		return -1;
	}
	snprintf(made, sizeof made, "%s/boot/vmlinuz-%s", dir, HULL_KERNEL_RELEASE);
	if (access(made, R_OK) == 0) {
		printf("%s\n", dir);
		return 0;
	}
	if (access(depmod, X_OK) != 0)
		snprintf(depmod, sizeof depmod, "/sbin/depmod");
	if (access(depmod, X_OK) != 0) {
		snprintf(why, cap, "the fetched kernel's modules are indexed by depmod, which is not "
		         "installed (kmod)");
		return -1;
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
	snprintf(deb, sizeof deb, "%s/kernel.deb", work);
	snprintf(tar, sizeof tar, "%s/data.tar", work);
	snprintf(tree, sizeof tree, "%s/tree", work);

	fprintf(stderr, "hull: fetching Debian's kernel %s, about 108 MB: %s%s\n", HULL_KERNEL_VERSION,
	        HULL_DEBIAN_POOL, HULL_KERNEL_PATH);
	if (hull_fetch_deb(HULL_KERNEL_PATH, HULL_KERNEL_SHA256, deb, tar, why, cap) != 0)
		goto done;
	if ((out = open(tar, O_RDONLY | O_CLOEXEC)) < 0) {
		snprintf(why, cap, "%s: %s", tar, strerror(errno));
		goto done;
	}
	if (mkdir(tree, 0755) != 0) {
		snprintf(why, cap, "%s: %s", tree, strerror(errno));
		close(out);
		goto done;
	}
	if ((u = hull_unpack_begin(tree, why, cap)) == NULL) {
		close(out);
		goto done;
	}
	fd = hull_unpack_layer(u, out, &skipped, why, cap);
	close(out);
	if (fd != 0) {
		char ignored[200];

		hull_unpack_end(u, ignored, sizeof ignored);
		goto done;
	}
	if (hull_unpack_end(u, why, cap) != 0)
		goto done;
	unlink(tar);
	/* Merged /usr puts modules in usr/lib/modules; depmod and the engine
	 * look in lib/modules. */
	snprintf(from, sizeof from, "%s/usr/lib/modules", tree);
	snprintf(to, sizeof to, "%s/lib", tree);
	if (stat(from, &is) == 0) {
		mkdir(to, 0755);
		snprintf(to, sizeof to, "%s/lib/modules", tree);
		if (rename(from, to) != 0) {
			snprintf(why, cap, "the kernel's modules could not be moved: %s", strerror(errno));
			goto done;
		}
	}
	{
		const char *const argv[] = { depmod, "-b", tree, HULL_KERNEL_RELEASE, NULL };
		char              said[400];

		if (hull_hostnet_run(argv, said, sizeof said) != 0) {
			said[strcspn(said, "\n")] = '\0';
			snprintf(why, cap, "depmod could not index the kernel's modules: %s", said);
			goto done;
		}
	}
	if (rename(tree, dir) != 0) {
		snprintf(why, cap, "%s: %s", dir, strerror(errno));
		goto done;
	}
	printf("%s\n", dir);
	status = 0;
done:
	unlink(deb);
	unlink(tar);
	/* What did not become the kernel's directory: the tree, made by this
	 * process beneath its own work directory, removed as gc removes one. */
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
