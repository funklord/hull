/*
 * export.c -- `hull export --bootable` (section 3.13): a stored tree as a
 * standalone disk image, the border artifact that leaves hull behind.
 *
 * The image is GPT, written here rather than by sfdisk, so that one tree
 * gives one layout: an EFI system partition holding Limine, the host's
 * kernel, an initramfs and limine.conf, and the tree's own partition,
 * ext4. The tree goes in as hull_store_layer() gives it, a tar with its
 * manifest's owners, read by mke2fs -d -- as the user, needing no mount
 * and no root, root's files root's in the image. The initramfs is a vm's,
 * the agent and the modules that mount a block root, with /hull/boot
 * naming the tree's partition and its init: the agent mounts it and
 * hands it to that init, there being no host to wait for.
 *
 * Limine is not shipped in the tree (section 9's dependency rule): its
 * UEFI loader is downloaded when first needed, into the user's cache,
 * pinned by version and checked against its sha256 on every use.
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

#include <zlib.h>

#include "builtin.h"
#include "export.h"
#include "hostnet.h"
#include "https.h"
#include "sha256.h"

/* The newest Limine with prebuilt binaries: from 12.0 its releases are
 * source alone. Moved by hand, with the digest of the file it names. */
#define LIMINE_VERSION "11.4.1"
#define LIMINE_URL     "https://raw.githubusercontent.com/limine-bootloader/limine/" \
                       "v" LIMINE_VERSION "-binary/BOOTX64.EFI"
#define LIMINE_SHA256  "333f7a69379b1f47e019be215885cdc078b9edd6e9255cbe2f706d6c5ec0ef06"

#define SECTOR     512ull
#define MIB        (1024ull * 1024ull)
#define ENTRIES    128u      /* GPT's partition entries, 128 bytes each */
#define FAT_TIME   946684800 /* 2000-01-01, the ESP's files' mtime */

/* A file the export made, `work`/`name`. */
static void path_of(char *out, size_t cap, const char *work, const char *name)
{
	snprintf(out, cap, "%s/%s", work, name);
}

/* The first of the usual places that holds `name` as a program. */
static const char *tool(const char *name, char *out, size_t cap)
{
	static const char *const dirs[] = { "/usr/sbin", "/sbin", "/usr/bin", "/bin" };

	for (unsigned i = 0u; i < sizeof dirs / sizeof dirs[0]; i++) {
		snprintf(out, cap, "%s/%s", dirs[i], name);
		if (access(out, X_OK) == 0)
			return out;
	}
	return NULL;
}

static int run(const char *const *argv, const char *what, char *why, size_t cap)
{
	char said[600];

	if (hull_hostnet_run(argv, said, sizeof said) == 0)
		return 0;
	said[strcspn(said, "\n")] = '\0';
	snprintf(why, cap, "%s: %s", what, said);
	return -1;
}

/* --- Limine -------------------------------------------------------------- */

typedef struct fetch {
	int         fd;
	hull_sha256 hash;
} fetch;

static int fetched(void *context, const uint8_t *data, size_t n, char *why, size_t cap)
{
	fetch *f = context;

	hull_sha256_update(&f->hash, data, n);
	for (size_t done = 0u; done < n;) {
		ssize_t wrote = write(f->fd, data + done, n - done);

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

/* Whether the file at `path` has the digest Limine's is pinned to. */
static int pinned(const char *path)
{
	uint8_t     buffer[65536];
	uint8_t     digest[32];
	char        hex[65];
	hull_sha256 hash;
	ssize_t     got;
	int         fd = open(path, O_RDONLY | O_CLOEXEC);

	if (fd < 0)
		return 0;
	hull_sha256_init(&hash);
	while ((got = read(fd, buffer, sizeof buffer)) > 0)
		hull_sha256_update(&hash, buffer, (size_t)got);
	close(fd);
	if (got < 0)
		return 0;
	hull_sha256_final(&hash, digest);
	hull_sha256_hex(digest, hex);
	return strcmp(hex, LIMINE_SHA256) == 0;
}

/* Limine's UEFI loader, in the user's cache, fetched where it is not
 * there or not what it is pinned to: its path into `out`. A download is
 * said on stderr, since a build that reaches the network says so. */
static int limine_fetched(char *out, size_t cap, char *why, size_t why_cap);

/* The directory beside the running hull where `make limine` puts Limine
 * built from source, 12.x: BOOTX64.EFI, limine-bios.sys and the limine
 * tool. 0, or -1 where there is no such place. */
static int limine_built(char *out, size_t cap)
{
	char    self[4096];
	ssize_t n = readlink("/proc/self/exe", self, sizeof self - 1u);
	char   *slash;

	if (n <= 0)
		return -1;
	self[n] = '\0';
	if ((slash = strrchr(self, '/')) == NULL)
		return -1;
	*slash = '\0';
	return snprintf(out, cap, "%s/limine", self) < (int)cap ? 0 : -1;
}

static int limine(char *out, size_t cap, char *why, size_t why_cap)
{
	char built[4200];

	/* Built from source by `make limine`, the newest, where it is. */
	if (limine_built(built, sizeof built) == 0 &&
	    snprintf(out, cap, "%s/BOOTX64.EFI", built) < (int)cap && access(out, R_OK) == 0)
		return 0;
	return limine_fetched(out, cap, why, why_cap);
}

/* Limine's UEFI loader as released prebuilt, 11.4.1's, fetched into the
 * cache, where none is built. */
static int limine_fetched(char *out, size_t cap, char *why, size_t why_cap)
{
	const char         *cache = getenv("XDG_CACHE_HOME");
	const char         *home = getenv("HOME");
	char                dir[4096];
	char                part[4400];
	hull_https_response got = { 0 };
	fetch               f;
	uint8_t             digest[32];
	char                hex[65];

	if (cache != NULL && cache[0] == '/')
		snprintf(dir, sizeof dir, "%s/hull", cache);
	else if (home != NULL && home[0] == '/')
		snprintf(dir, sizeof dir, "%s/.cache/hull", home);
	else {
		snprintf(why, why_cap, "neither XDG_CACHE_HOME nor HOME names a place for Limine");
		return -1;
	}
	snprintf(out, cap, "%s/limine-%s/BOOTX64.EFI", dir, LIMINE_VERSION);
	if (pinned(out))
		return 0;
	for (char *slash = strchr(out + 1, '/'); slash != NULL; slash = strchr(slash + 1, '/')) {
		*slash = '\0';
		if (mkdir(out, 0755) != 0 && errno != EEXIST) {
			snprintf(why, why_cap, "%s: %s", out, strerror(errno));
			*slash = '/';
			return -1;
		}
		*slash = '/';
	}
	fprintf(stderr, "hull: fetching Limine %s's UEFI loader: %s\n", LIMINE_VERSION, LIMINE_URL);
	snprintf(part, sizeof part, "%s.part", out);
	if ((f.fd = open(part, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644)) < 0) {
		snprintf(why, why_cap, "%s: %s", part, strerror(errno));
		return -1;
	}
	hull_sha256_init(&f.hash);
	why[0] = '\0';
	if (hull_https_get(LIMINE_URL, NULL, &got, fetched, &f, why, why_cap) != 0 ||
	    got.status != 200) {
		if (why[0] == '\0')
			snprintf(why, why_cap, "Limine's loader: %s answered %d", LIMINE_URL, got.status);
		close(f.fd);
		unlink(part);
		return -1;
	}
	hull_sha256_final(&f.hash, digest);
	hull_sha256_hex(digest, hex);
	if (close(f.fd) != 0 || strcmp(hex, LIMINE_SHA256) != 0) {
		unlink(part);
		snprintf(why, why_cap, "Limine's loader from %s is not the one pinned: sha256 %s, "
		         "wanted %s", LIMINE_URL, hex, LIMINE_SHA256);
		return -1;
	}
	if (rename(part, out) != 0) {
		snprintf(why, why_cap, "%s: %s", out, strerror(errno));
		unlink(part);
		return -1;
	}
	return 0;
}

/* --- the image ----------------------------------------------------------- */

static int tar_sink(void *context, const uint8_t *data, size_t n)
{
	int fd = *(int *)context;

	for (size_t done = 0u; done < n;) {
		ssize_t wrote = write(fd, data + done, n - done);

		if (wrote < 0 && errno == EINTR)
			continue;
		if (wrote <= 0)
			return -1;
		done += (size_t)wrote;
	}
	return 0;
}

static unsigned long long file_size(const char *path)
{
	struct stat is;

	return stat(path, &is) == 0 ? (unsigned long long)is.st_size : 0ull;
}

static unsigned long long round_mib(unsigned long long bytes)
{
	return (bytes + MIB - 1u) / MIB * MIB;
}

/* `from` into `to` at byte `at`, its runs of zeros left as holes. */
static int place(int to, const char *from, unsigned long long at)
{
	static uint8_t     buffer[65536];
	static const uint8_t zero[sizeof buffer];
	int                fd = open(from, O_RDONLY | O_CLOEXEC);
	ssize_t            got;

	if (fd < 0)
		return -1;
	while ((got = read(fd, buffer, sizeof buffer)) > 0) {
		if (memcmp(buffer, zero, (size_t)got) != 0 &&
		    pwrite(to, buffer, (size_t)got, (off_t)at) != got) {
			close(fd);
			return -1;
		}
		at += (unsigned long long)got;
	}
	close(fd);
	return got < 0 ? -1 : 0;
}

static void put16(uint8_t *p, unsigned v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
}

static void put32(uint8_t *p, uint32_t v)
{
	for (unsigned i = 0u; i < 4u; i++)
		p[i] = (uint8_t)(v >> (8u * i));
}

static void put64(uint8_t *p, uint64_t v)
{
	for (unsigned i = 0u; i < 8u; i++)
		p[i] = (uint8_t)(v >> (8u * i));
}

/* A GUID from its text, as GPT holds it: the first three fields little
 * endian, the rest as written. */
static void guid(uint8_t out[16], const char *text)
{
	static const unsigned order[16] = { 3, 2, 1, 0, 5, 4, 7, 6, 8, 9, 10, 11, 12, 13, 14, 15 };
	uint8_t                bytes[16];
	unsigned               n = 0u;

	for (const char *p = text; *p != '\0' && n < 16u; p++) {
		unsigned hi;
		unsigned lo;

		if (*p == '-')
			continue;
		sscanf(p, "%1x", &hi);
		sscanf(p + 1, "%1x", &lo);
		bytes[n++] = (uint8_t)(hi << 4 | lo);
		p++;
	}
	for (unsigned i = 0u; i < 16u; i++)
		out[i] = bytes[order[i]];
}

/* One partition's entry: its type, its own GUID, its sectors, its name. */
static void entry(uint8_t *e, const char *type, const uint8_t own[16], uint64_t first,
                  uint64_t last, const char *name)
{
	guid(e, type);
	memcpy(e + 16, own, 16u);
	put64(e + 32, first);
	put64(e + 40, last);
	for (unsigned i = 0u; name[i] != '\0' && i < 36u; i++)
		put16(e + 56 + 2u * i, (unsigned char)name[i]);
}

/* GPT's header at `self`, its copy at `other`, its entries at `entries`. */
static void header(uint8_t *h, uint64_t self, uint64_t other, uint64_t entries,
                   uint64_t last_usable, const uint8_t disk[16], uint32_t entries_crc)
{
	memset(h, 0, SECTOR);
	memcpy(h, "EFI PART", 8u);
	put32(h + 8, 0x00010000u);
	put32(h + 12, 92u);
	put64(h + 24, self);
	put64(h + 32, other);
	put64(h + 40, 34u);
	put64(h + 48, last_usable);
	memcpy(h + 56, disk, 16u);
	put64(h + 72, entries);
	put32(h + 80, ENTRIES);
	put32(h + 84, 128u);
	put32(h + 88, entries_crc);
	put32(h + 16, (uint32_t)crc32(0L, h, 92u));
}

/* The partition table of a disk of `sectors`, the ESP and the root at
 * the sectors given, each partition's GUID drawn from `digest`. */
static int write_gpt(int fd, uint64_t sectors, uint64_t esp_first, uint64_t esp_last,
                     uint64_t root_first, uint64_t root_last, const uint8_t digest[32],
                     const char *root_name)
{
	static uint8_t entries[ENTRIES * 128u];
	uint8_t        mbr[SECTOR];
	uint8_t        primary[SECTOR];
	uint8_t        backup[SECTOR];
	uint8_t        disk[16];
	uint8_t        esp[16];
	uint8_t        root[16];
	uint8_t        bios[16];
	uint32_t       crc;
	uint64_t       last = sectors - 1u;

	/* Each a version 4 GUID's shape, its bits the tree's digest. */
	memcpy(disk, digest, 16u);
	memcpy(esp, digest + 16, 16u);
	memcpy(root, digest + 8, 16u);
	memcpy(bios, digest + 4, 16u);
	disk[7] = (uint8_t)((disk[7] & 0x0fu) | 0x40u), disk[8] = (uint8_t)((disk[8] & 0x3fu) | 0x80u);
	esp[7]  = (uint8_t)((esp[7] & 0x0fu) | 0x40u), esp[8] = (uint8_t)((esp[8] & 0x3fu) | 0x80u);
	root[7] = (uint8_t)((root[7] & 0x0fu) | 0x40u), root[8] = (uint8_t)((root[8] & 0x3fu) | 0x80u);
	bios[7] = (uint8_t)((bios[7] & 0x0fu) | 0x40u), bios[8] = (uint8_t)((bios[8] & 0x3fu) | 0x80u);
	memset(entries, 0, sizeof entries);
	entry(entries, "C12A7328-F81F-11D2-BA4B-00A0C93EC93B", esp, esp_first, esp_last, "hull-esp");
	entry(entries + 128, "0FC63DAF-8483-4772-8E79-3D69D8477DE4", root, root_first, root_last,
	      root_name);
	/* A BIOS boot partition in the gap before the ESP, third so the ESP
	 * and the root keep their numbers: where Limine's second stage goes
	 * for a BIOS to boot the disk, GPT having no room of its own for it.
	 * Empty where no BIOS loader is installed, and nothing to UEFI. */
	entry(entries + 256, "21686148-6449-6E6F-744E-656564454649", bios, 34u, esp_first - 1u,
	      "hull-bios");
	crc = (uint32_t)crc32(0L, entries, sizeof entries);

	/* The protective MBR: one partition of type 0xEE over the disk. */
	memset(mbr, 0, sizeof mbr);
	mbr[446 + 1] = 0x00u, mbr[446 + 2] = 0x02u, mbr[446 + 3] = 0x00u;
	mbr[446 + 4] = 0xeeu;
	mbr[446 + 5] = 0xffu, mbr[446 + 6] = 0xffu, mbr[446 + 7] = 0xffu;
	put32(mbr + 446 + 8, 1u);
	put32(mbr + 446 + 12, last > 0xffffffffull ? 0xffffffffu : (uint32_t)last);
	mbr[510] = 0x55u, mbr[511] = 0xaau;

	header(primary, 1u, last, 2u, sectors - 34u, disk, crc);
	header(backup, last, 1u, sectors - 33u, sectors - 34u, disk, crc);
	return pwrite(fd, mbr, SECTOR, 0) == (ssize_t)SECTOR &&
	       pwrite(fd, primary, SECTOR, (off_t)SECTOR) == (ssize_t)SECTOR &&
	       pwrite(fd, entries, sizeof entries, (off_t)(2u * SECTOR)) == (ssize_t)sizeof entries &&
	       pwrite(fd, entries, sizeof entries, (off_t)((sectors - 33u) * SECTOR)) ==
	       (ssize_t)sizeof entries &&
	       pwrite(fd, backup, SECTOR, (off_t)(last * SECTOR)) == (ssize_t)SECTOR ? 0 : -1;
}

/* The work files, named, and the directory, once the export is done. */
static void clean(const char *work)
{
	static const char *const names[] = { "tree.tar", "root.img", "esp.img", "initrd",
	                                     "limine.conf", "mtoolsrc" };
	char                     path[4300];

	for (unsigned i = 0u; i < sizeof names / sizeof names[0]; i++) {
		path_of(path, sizeof path, work, names[i]);
		unlink(path);
	}
	rmdir(work);
}

/* The ESP, `esp.img` in `work`: FAT32 with Limine as the removable-media
 * loader, BOOTX64.EFI, beside limine.conf, the kernel and the initramfs.
 * mkfs.vfat's --invariant and fixed mtimes make one input one image:
 * every file and directory 2000-01-01 by SOURCE_DATE_EPOCH, mtools'
 * time for what it makes, not the source's -- Limine built by `make
 * limine` carries the hour it was built, which made two builds two
 * images. */
static int build_esp(const char *work, const char *loader, const char *kernel,
                     const char *cmdline, char *why, size_t cap)
{
	char                 mkfs[300], mmd[300], mcopy[300];
	char                 esp[4300], initrd[4300], conf[4300], rc[4300];
	char                 size[32];
	char                 setting[4400];
	unsigned long long   bytes;
	FILE                *file;
	int                  written;
	struct timespec      times[2] = { { FAT_TIME, 0 }, { FAT_TIME, 0 } };

	if (tool("mkfs.vfat", mkfs, sizeof mkfs) == NULL) {
		snprintf(why, cap, "a bootable image's ESP is made by mkfs.vfat, which is not installed "
		         "(dosfstools)");
		return -1;
	}
	if (tool("mmd", mmd, sizeof mmd) == NULL || tool("mcopy", mcopy, sizeof mcopy) == NULL) {
		snprintf(why, cap, "a bootable image's ESP is filled by mtools, which is not installed");
		return -1;
	}
	path_of(esp, sizeof esp, work, "esp.img");
	path_of(initrd, sizeof initrd, work, "initrd");
	path_of(conf, sizeof conf, work, "limine.conf");
	path_of(rc, sizeof rc, work, "mtoolsrc");
	/* The kernel's console is the screen and the first serial port, the
	 * latter last, so it is /dev/console: what a headless qemu shows. */
	if ((file = fopen(conf, "w")) == NULL) {
		snprintf(why, cap, "limine.conf could not be written");
		return -1;
	}
	/* Closed whether the write went or not, so a failed one leaks nothing. */
	written = fprintf(file, "timeout: 0\n\n/hull\n    protocol: linux\n    path: boot():/vmlinuz\n"
	                  "    module_path: boot():/initrd\n    cmdline: %s\n", cmdline) >= 0;
	if ((fclose(file) != 0) | !written) {
		snprintf(why, cap, "limine.conf could not be written");
		return -1;
	}
	/* mtools reads ~/.mtoolsrc where there is one: an empty one of its
	 * own instead, and no check of a FAT it just made. */
	if ((file = fopen(rc, "w")) == NULL) {
		snprintf(why, cap, "the ESP's mtools configuration could not be written");
		return -1;
	}
	written = fputs("mtools_skip_check=1\n", file) >= 0;
	if ((fclose(file) != 0) | !written) {
		snprintf(why, cap, "the ESP's mtools configuration could not be written");
		return -1;
	}
	utimensat(AT_FDCWD, initrd, times, 0);
	utimensat(AT_FDCWD, conf, times, 0);
	bytes = file_size(loader) + file_size(kernel) + file_size(initrd) + file_size(conf);
	bytes = round_mib(bytes + bytes / 4u + 8u * MIB);
	if (bytes < 64u * MIB)
		bytes = 64u * MIB;
	snprintf(size, sizeof size, "%llu", bytes / 1024u);
	snprintf(setting, sizeof setting, "MTOOLSRC=%s", rc);
	{
		const char *const format[] = { mkfs, "--invariant", "-F", "32", "-i", "68756c6c",
		                               "-n", "HULL-ESP", "-C", esp, size, NULL };
		const char *const dirs[] = { "env", setting, "SOURCE_DATE_EPOCH=946684800", mmd, "-i",
		                             esp, "::/EFI", "::/EFI/BOOT", NULL };
		const char *const efi[] = { "env", setting, "SOURCE_DATE_EPOCH=946684800", mcopy, "-i",
		                            esp, loader, "::/EFI/BOOT/BOOTX64.EFI", NULL };
		const char *const files[] = { "env", setting, "SOURCE_DATE_EPOCH=946684800", mcopy, "-i",
		                              esp, kernel, "::/vmlinuz", NULL };
		const char *const rest[] = { "env", setting, "SOURCE_DATE_EPOCH=946684800", mcopy, "-i",
		                             esp, initrd, conf, "::/", NULL };
		char              built[4200];
		char              stage[4300];
		int               bios = limine_built(built, sizeof built) == 0 &&
		                         snprintf(stage, sizeof stage, "%s/limine-bios.sys", built) > 0 &&
		                         access(stage, R_OK) == 0;
		const char *const bios_files[] = { "env", setting, "SOURCE_DATE_EPOCH=946684800",
		                                   mcopy, "-i", esp, stage, "::/", NULL };

		if (run(format, "the ESP could not be made", why, cap) != 0 ||
		    run(dirs, "the ESP's directories could not be made", why, cap) != 0 ||
		    run(efi, "Limine could not be put on the ESP", why, cap) != 0 ||
		    run(files, "the kernel could not be put on the ESP", why, cap) != 0 ||
		    run(rest, "the initramfs could not be put on the ESP", why, cap) != 0 ||
		    (bios && run(bios_files, "Limine's BIOS stage could not be put on the ESP", why,
		                 cap) != 0))
			return -1;
	}
	return 0;
}

/* The image, standalone where `host` is NULL, its init `init`; hosted
 * where it names the agent's UART. */
static int export_image(hull_store *store, const char *name, const char *image,
                        const char *init, const char *host, char *why, size_t cap)
{
	char                     work[4200];
	char                     tar[4300], root[4300], esp[4300];
	char                     mke2fs[300];
	char                     kernel[4400];
	char                     loader[4300];
	char                     hex[65];
	char                     arch[64] = "";
	char                     part_name[40];
	char                     boot[400];
	char                     cmdline[200];
	char                     uuid[40];
	uint8_t                  digest[32];
	unsigned long long       bytes, esp_bytes, root_bytes, total;
	int                      fd;
	int                      status = -1;

	if (host != NULL && (strncmp(host, "tty", 3u) != 0 || strpbrk(host, "/ \n") != NULL ||
	                     strlen(host) > 16u)) {
		snprintf(why, cap, "the agent's port is not a tty's name");
		return -1;
	}
	if (host == NULL &&
	    (init[0] != '/' || strchr(init, '\n') != NULL || strlen(init) >= 256u)) {
		snprintf(why, cap, "the image's init is not an absolute path");
		return -1;
	}
	if (tool("mke2fs", mke2fs, sizeof mke2fs) == NULL) {
		snprintf(why, cap, "a bootable image's root is made by mke2fs, which is not installed "
		         "(e2fsprogs)");
		return -1;
	}
	if (hull_store_resolve(store, name, hex, why, cap) != 0)
		return -1;
	for (unsigned i = 0u; i < 32u; i++)
		sscanf(hex + 2u * i, "%2hhx", &digest[i]);
	/* The fetch first: a download that fails costs nothing built. */
	if (limine(loader, sizeof loader, why, cap) != 0)
		return -1;
	snprintf(work, sizeof work, "%s/tmp/export-XXXXXX", store->root);
	if (mkdtemp(work) == NULL) {
		snprintf(why, cap, "%s: %s", work, strerror(errno));
		return -1;
	}
	path_of(tar, sizeof tar, work, "tree.tar");
	path_of(root, sizeof root, work, "root.img");
	path_of(esp, sizeof esp, work, "esp.img");

	/* The tree, as a tar with its manifest's owners. */
	if ((fd = open(tar, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600)) < 0) {
		snprintf(why, cap, "%s: %s", tar, strerror(errno));
		goto done;
	}
	if (hull_store_layer(store, name, tar_sink, &fd, arch, why, cap) != 0) {
		close(fd);
		goto done;
	}
	if (close(fd) != 0) {
		snprintf(why, cap, "%s: %s", tar, strerror(errno));
		goto done;
	}
	if (strcmp(arch, "amd64") != 0) {
		snprintf(why, cap, "the tree is %s's, and a bootable image boots the host's x86_64 "
		         "kernel", arch[0] != '\0' ? arch : "an unnamed arch");
		goto done;
	}

	/* Its partition: ext4 from the tar, sized for it and a quarter
	 * again, and 256 MiB for what the machine writes. Its UUID and hash
	 * seed are the tree's digest, its time fixed. */
	bytes = file_size(tar);
	root_bytes = round_mib(bytes + bytes / 4u + 256u * MIB);
	snprintf(uuid, sizeof uuid, "%.8s-%.4s-%.4s-%.4s-%.12s", hex, hex + 8, hex + 12, hex + 16,
	         hex + 20);
	{
		char size[32];
		char options[120];
		const char *const argv[] = { "env", "E2FSPROGS_FAKE_TIME=1", mke2fs, "-q", "-F", "-t",
		                             "ext4", "-U", uuid, "-E", options, "-d", tar, root, size,
		                             NULL };

		snprintf(size, sizeof size, "%lluk", root_bytes / 1024u);
		snprintf(options, sizeof options, "hash_seed=%s,root_owner=0:0", uuid);
		if (run(argv, "the tree's partition could not be made (mke2fs reads the tree as a "
		        "tar, through libarchive)", why, cap) != 0)
			goto done;
	}
	unlink(tar);

	/* The kernel and initramfs, the agent told which partition is the
	 * tree's by its name, and its init. */
	snprintf(part_name, sizeof part_name, "hull-root-%.8s", hex);
	/* Standalone, the agent boots the tree's init by /hull/boot; hosted,
	 * it waits for its host on the UART and mounts the partition named
	 * when the host's spec says to, the console the first port's or, when
	 * the first is the agent's, the second's. */
	if (host == NULL) {
		snprintf(boot, sizeof boot, "root %s\ninit %s\n", part_name, init);
		snprintf(cmdline, sizeof cmdline, "console=tty0 console=ttyS0");
	} else {
		snprintf(cmdline, sizeof cmdline, "console=tty0 console=ttyS1 hull.host=%s hull.root=%s",
		         host, part_name);
	}
	if (hull_qemu_boot_files(work, host == NULL ? boot : NULL, kernel, sizeof kernel, why,
	                         cap) != NULL)
		goto done;
	if (build_esp(work, loader, kernel, cmdline, why, cap) != 0)
		goto done;

	/* The disk: 1 MiB for the table, the ESP, the root, and 1 MiB for
	 * the table's copy at the end. */
	esp_bytes = file_size(esp);
	total = MIB + esp_bytes + root_bytes + MIB;
	if ((fd = open(image, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644)) < 0) {
		snprintf(why, cap, "%s: %s", image, strerror(errno));
		goto done;
	}
	if (ftruncate(fd, (off_t)total) != 0 ||
	    write_gpt(fd, total / SECTOR, MIB / SECTOR, (MIB + esp_bytes) / SECTOR - 1u,
	              (MIB + esp_bytes) / SECTOR, (MIB + esp_bytes + root_bytes) / SECTOR - 1u,
	              digest, part_name) != 0 ||
	    place(fd, esp, MIB) != 0 || place(fd, root, MIB + esp_bytes) != 0) {
		snprintf(why, cap, "%s could not be written: %s", image, strerror(errno));
		close(fd);
		unlink(image);
		goto done;
	}
	if (close(fd) != 0) {
		snprintf(why, cap, "%s: %s", image, strerror(errno));
		unlink(image);
		goto done;
	}
	/* BIOS boot too, where Limine is built from source: its tool writes
	 * the MBR's boot code and its second stage to the BIOS boot
	 * partition, the third. The fetched 11.x has no tool, so its images
	 * boot under UEFI alone. */
	{
		char built[4200];
		char tool[4300];

		if (limine_built(built, sizeof built) == 0 &&
		    snprintf(tool, sizeof tool, "%s/limine", built) > 0 && access(tool, X_OK) == 0) {
			const char *const argv[] = { tool, "bios-install", image, "3", NULL };

			if (run(argv, "Limine's BIOS boot could not be installed", why, cap) != 0) {
				unlink(image);
				goto done;
			}
		}
	}
	status = 0;
done:
	clean(work);
	return status;
}

int hull_export_bootable(hull_store *store, const char *name, const char *image,
                         const char *init, char *why, size_t cap)
{
	return export_image(store, name, image, init, NULL, why, cap);
}

int hull_export_hosted(hull_store *store, const char *name, const char *image,
                       const char *host, char *why, size_t cap)
{
	return export_image(store, name, image, "", host, why, cap);
}
