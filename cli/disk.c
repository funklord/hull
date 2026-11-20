/*
 * disk.c -- `hull disk write` (section 3.13): a (disk ...) written to an
 * image file or straight to a drive, then read back.
 *
 * The table is written here, as `hull export --bootable` writes its own,
 * so that one description gives one layout; each filesystem is made in
 * place by its own tool at the partition's offset -- mke2fs -E offset=,
 * mkfs.vfat --offset -- and filled by it from the tree, mke2fs reading the
 * tree as the tar a push would send, mtools copying a materialized one. So
 * a drive is never the copy of an image: it is built where it is, at its
 * own size, and a filesystem made on the device zeroes what it needs the
 * way it does on any device. All as the user, no mount: a drive has to be
 * the user's to write, which is the administrator's (a group, a udev rule)
 * and nothing hull does.
 *
 * Read back: the page cache dropped first, so what is read is the drive's;
 * then the table compared with what was written, each ext filesystem
 * checked by e2fsck, and each partition's tree dumped by debugfs or mtools
 * and compared with the tree it was filled from, by diff. What the
 * comparison cannot see as the user -- owners, a device node -- e2fsck
 * holds to the filesystem's own consistency only.
 */
#define _GNU_SOURCE

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>

#include <linux/fs.h>
#include <zlib.h>

#include "disk.h"
#include "hostnet.h"
#include "sha256.h"

#define SECTOR  ((uint64_t)512u)
#define MIB     ((uint64_t)1024u * 1024u)
#define ENTRIES 128u        /* GPT's partition entries, 128 bytes each */
#define FAT_TIME "946684800" /* 2000-01-01, what mtools gives what it copies */

typedef struct work {
	hull_store *store;
	char        dir[4200];
	char        why_tool[300];
} work;

/* --- little helpers -------------------------------------------------------- */

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

static uint64_t round_mib(uint64_t bytes)
{
	return (bytes + MIB - 1u) / MIB * MIB;
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

static int need(const char *name, const char *package, char *out, size_t size, char *why,
                size_t cap)
{
	if (tool(name, out, size) != NULL)
		return 0;
	snprintf(why, cap, "%s is not installed (%s)", name, package);
	return -1;
}

/* A digest's first 16 bytes as a version 4 GUID's shape. */
static void guid_of(uint8_t out[16], const uint8_t digest[32])
{
	memcpy(out, digest, 16u);
	out[7] = (uint8_t)((out[7] & 0x0fu) | 0x40u);
	out[8] = (uint8_t)((out[8] & 0x3fu) | 0x80u);
}

/* The same as text, the way mke2fs -U takes a UUID. */
static void uuid_text(char out[37], const uint8_t digest[32])
{
	uint8_t g[16];

	guid_of(g, digest);
	snprintf(out, 37, "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
	         g[0], g[1], g[2], g[3], g[4], g[5], g[6], g[7], g[8], g[9], g[10], g[11], g[12],
	         g[13], g[14], g[15]);
}

/* A GUID from its text, as GPT holds it: the first three fields little
 * endian, the rest as written. */
static void guid_from_text(uint8_t out[16], const char *text)
{
	static const unsigned order[16] = { 3, 2, 1, 0, 5, 4, 7, 6, 8, 9, 10, 11, 12, 13, 14, 15 };
	uint8_t               bytes[16] = { 0 };
	unsigned              n = 0u;

	for (const char *p = text; p[0] != '\0' && p[1] != '\0' && n < 16u; p++) {
		unsigned value;

		if (*p == '-')
			continue;
		sscanf(p, "%2x", &value);
		bytes[n++] = (uint8_t)value;
		p++;
	}
	for (unsigned i = 0u; i < 16u; i++)
		out[i] = bytes[order[i]];
}

/* --- the layout ------------------------------------------------------------ */

typedef struct layout {
	uint64_t bytes;
	uint64_t first[HULL_DISK_PARTITIONS];
	uint64_t length[HULL_DISK_PARTITIONS];
	uint8_t  digest[32];                    /* the disk's identity: its GUIDs, its signature */
	uint8_t  part_digest[HULL_DISK_PARTITIONS][32];
	uint64_t tree[HULL_DISK_PARTITIONS];    /* each tree's tar, in bytes; 0 for none */
} layout;

/* What a partition of `tree` bytes needs: the tree and a quarter again,
 * and 64 MiB beside it; 64 MiB with none. */
static uint64_t needs(uint64_t tree)
{
	return round_mib(tree + tree / 4u + 64u * MIB);
}

/* Where each partition goes on a disk of `total` bytes, or 0 for the
 * size it needs, which is then set. */
static int lay_out(const hull_disk *disk, layout *l, uint64_t total, char *why, size_t cap)
{
	uint64_t end_room = disk->gpt ? MIB : 0u;   /* GPT's copy at the end */
	uint64_t fixed = MIB + end_room;
	int      rest = -1;

	for (unsigned i = 0u; i < disk->count; i++) {
		if (disk->partition[i].bytes == 0u) {
			if (rest >= 0 || i + 1u != disk->count) {
				snprintf(why, cap, "partition %u: only the last partition may be \"rest\"", i + 1u);
				return -1;
			}
			rest = (int)i;
		} else {
			fixed += round_mib(disk->partition[i].bytes);
		}
	}
	if (total == 0u)
		total = fixed + (rest >= 0 ? needs(l->tree[rest]) : 0u);
	if (total < fixed + (rest >= 0 ? MIB : 0u)) {
		snprintf(why, cap, "the partitions need %" PRIu64 " MiB and the disk has %" PRIu64,
		         (fixed + (rest >= 0 ? MIB : 0u)) / MIB, total / MIB);
		return -1;
	}
	if (!disk->gpt && total / SECTOR > 0xffffffffull) {
		snprintf(why, cap, "a dos label reaches 2 TiB, and the disk is larger: (label gpt)");
		return -1;
	}
	l->bytes = total;
	{
		uint64_t at = MIB;

		for (unsigned i = 0u; i < disk->count; i++) {
			l->first[i]  = at;
			l->length[i] = disk->partition[i].bytes != 0u ? round_mib(disk->partition[i].bytes) :
			               (total - end_room - at) / MIB * MIB;
			at += l->length[i];
		}
	}
	if (rest >= 0 && l->tree[rest] != 0u && l->length[rest] < needs(l->tree[rest]) - 64u * MIB) {
		snprintf(why, cap, "partition %d's tree needs about %" PRIu64 " MiB and the disk leaves "
		         "it %" PRIu64, rest + 1, (needs(l->tree[rest]) - 64u * MIB) / MIB,
		         l->length[rest] / MIB);
		return -1;
	}
	return 0;
}

/* The disk's identity: what it is described as, its trees and its size,
 * so that one description written to one size gives one disk. */
static void identify(const hull_disk *disk, layout *l)
{
	hull_sha256 hash;
	char        text[1400];

	hull_sha256_init(&hash);
	snprintf(text, sizeof text, "hull-disk 1 %s %" PRIu64 "\n", disk->gpt ? "gpt" : "dos",
	         l->bytes);
	hull_sha256_update(&hash, (const uint8_t *)text, strlen(text));
	for (unsigned i = 0u; i < disk->count; i++) {
		const hull_disk_partition *p = &disk->partition[i];

		snprintf(text, sizeof text, "%u %s %" PRIu64 " %d %d %d %s %s %s\n", i, p->name,
		         l->length[i], (int)p->type, p->active, (int)p->fs, p->features, p->volume,
		         p->ref);
		hull_sha256_update(&hash, (const uint8_t *)text, strlen(text));
	}
	hull_sha256_final(&hash, l->digest);
	for (unsigned i = 0u; i < disk->count; i++) {
		uint8_t index = (uint8_t)i;

		hull_sha256_init(&hash);
		hull_sha256_update(&hash, l->digest, 32u);
		hull_sha256_update(&hash, &index, 1u);
		hull_sha256_final(&hash, l->part_digest[i]);
	}
}

/* --- the table ------------------------------------------------------------- */

/* A CHS address of `lba`, as a 255-head, 63-sector BIOS reads it; the
 * largest where it is past what CHS can say. */
static void chs(uint8_t out[3], uint64_t lba)
{
	uint64_t cylinder = lba / (255u * 63u);
	unsigned head = (unsigned)(lba / 63u % 255u);
	unsigned sector = (unsigned)(lba % 63u + 1u);

	if (cylinder > 1023u) {
		out[0] = 0xfeu, out[1] = 0xffu, out[2] = 0xffu;
		return;
	}
	out[0] = (uint8_t)head;
	out[1] = (uint8_t)(sector | (cylinder >> 2 & 0xc0u));
	out[2] = (uint8_t)cylinder;
}

/* FAT32 from 512 MiB, FAT16 below, as make_vfat makes it. */
static int fat32(uint64_t length)
{
	return length >= 512u * MIB;
}

static uint8_t dos_type(hull_disk_type type, uint64_t length)
{
	switch (type) {
	case HULL_DISK_EFI:  return 0xefu;
	case HULL_DISK_FAT:  return fat32(length) ? 0x0cu : 0x0eu;  /* FAT32 or FAT16, by LBA */
	case HULL_DISK_SWAP: return 0x82u;
	default:             return 0x83u;
	}
}

static const char *gpt_type(hull_disk_type type)
{
	switch (type) {
	case HULL_DISK_EFI:  return "C12A7328-F81F-11D2-BA4B-00A0C93EC93B";
	case HULL_DISK_FAT:  return "EBD0A0A2-B9E5-4433-87C0-68B6B72699C7";
	case HULL_DISK_BIOS: return "21686148-6449-6E6F-744E-656564454649";
	case HULL_DISK_SWAP: return "0657FD6D-A4AB-43C4-84E5-0933C84B4F4F";
	default:             return "0FC63DAF-8483-4772-8E79-3D69D8477DE4";
	}
}

/* The boot code, the first 440 bytes of `path`, into `mbr`. */
static int boot_code(const char *path, uint8_t *mbr, char *why, size_t cap)
{
	struct stat is;
	int         fd = open(path, O_RDONLY | O_CLOEXEC);
	ssize_t     got;

	if (fd < 0 || fstat(fd, &is) != 0) {
		snprintf(why, cap, "the boot code, %s: %s", path, strerror(errno));
		if (fd >= 0)
			close(fd);
		return -1;
	}
	/* 440 bytes of code; 446 or 512 is a whole first sector's worth,
	 * mbr.bin's or a dump's, its code first. */
	if (is.st_size != 440 && is.st_size != 446 && is.st_size != 512) {
		close(fd);
		snprintf(why, cap, "the boot code, %s, is %lld bytes, and a first sector's code is 440 "
		         "(or 446 or 512, of which the first 440)", path, (long long)is.st_size);
		return -1;
	}
	got = pread(fd, mbr, 440u, 0);
	close(fd);
	if (got != 440) {
		snprintf(why, cap, "the boot code, %s, could not be read", path);
		return -1;
	}
	return 0;
}

/* The table's sectors: the first, and for GPT its header and entries at
 * both ends. Each region into `regions`, to be written and read back. */
typedef struct region {
	uint64_t at;
	size_t   length;
	uint8_t *data;
} region;

typedef struct table {
	uint8_t mbr[SECTOR];
	uint8_t primary[SECTOR];
	uint8_t backup[SECTOR];
	uint8_t entries[ENTRIES * 128u];
	region  region[5];
	unsigned count;
} table;

static void gpt_header(uint8_t *h, uint64_t self, uint64_t other, uint64_t entries,
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

static int make_table(const hull_disk *disk, const layout *l, table *t, char *why, size_t cap)
{
	uint64_t sectors = l->bytes / SECTOR;

	memset(t, 0, sizeof *t);
	if (disk->boot[0] != '\0' && boot_code(disk->boot, t->mbr, why, cap) != 0)
		return -1;
	t->mbr[510] = 0x55u, t->mbr[511] = 0xaau;
	if (!disk->gpt) {
		/* The disk's signature, from its identity. */
		memcpy(t->mbr + 440, l->digest, 4u);
		for (unsigned i = 0u; i < disk->count; i++) {
			uint8_t *e = t->mbr + 446 + 16u * i;
			uint64_t first = l->first[i] / SECTOR;
			uint64_t count = l->length[i] / SECTOR;

			e[0] = disk->partition[i].active ? 0x80u : 0x00u;
			chs(e + 1, first);
			e[4] = dos_type(disk->partition[i].type, l->length[i]);
			chs(e + 5, first + count - 1u);
			put32(e + 8, (uint32_t)first);
			put32(e + 12, (uint32_t)count);
		}
		t->region[t->count++] = (region){ 0u, SECTOR, t->mbr };
		return 0;
	}
	{
		uint8_t  disk_guid[16];
		uint32_t crc;
		uint64_t last = sectors - 1u;

		guid_of(disk_guid, l->digest);
		for (unsigned i = 0u; i < disk->count; i++) {
			uint8_t *e = t->entries + 128u * i;
			uint8_t  own[16];

			guid_from_text(e, gpt_type(disk->partition[i].type));
			guid_of(own, l->part_digest[i]);
			memcpy(e + 16, own, 16u);
			put64(e + 32, l->first[i] / SECTOR);
			put64(e + 40, (l->first[i] + l->length[i]) / SECTOR - 1u);
			/* Bit 2, legacy BIOS bootable: what gptmbr.bin looks for. */
			put64(e + 48, disk->partition[i].active ? 4u : 0u);
			for (unsigned k = 0u; disk->partition[i].name[k] != '\0' && k < 36u; k++)
				put16(e + 56 + 2u * k, (unsigned char)disk->partition[i].name[k]);
		}
		crc = (uint32_t)crc32(0L, t->entries, sizeof t->entries);
		/* The protective MBR: one partition of type 0xEE over the disk,
		 * the boot code before it. */
		t->mbr[446 + 1] = 0x00u, t->mbr[446 + 2] = 0x02u, t->mbr[446 + 3] = 0x00u;
		t->mbr[446 + 4] = 0xeeu;
		t->mbr[446 + 5] = 0xffu, t->mbr[446 + 6] = 0xffu, t->mbr[446 + 7] = 0xffu;
		put32(t->mbr + 446 + 8, 1u);
		put32(t->mbr + 446 + 12, last > 0xffffffffull ? 0xffffffffu : (uint32_t)last);
		gpt_header(t->primary, 1u, last, 2u, sectors - 34u, disk_guid, crc);
		gpt_header(t->backup, last, 1u, sectors - 33u, sectors - 34u, disk_guid, crc);
		t->region[t->count++] = (region){ 0u, SECTOR, t->mbr };
		t->region[t->count++] = (region){ SECTOR, SECTOR, t->primary };
		t->region[t->count++] = (region){ 2u * SECTOR, sizeof t->entries, t->entries };
		t->region[t->count++] = (region){ (sectors - 33u) * SECTOR, sizeof t->entries,
		                                  t->entries };
		t->region[t->count++] = (region){ last * SECTOR, SECTOR, t->backup };
	}
	return 0;
}

static int write_at(int fd, const void *data, size_t n, uint64_t at)
{
	for (size_t done = 0u; done < n;) {
		ssize_t wrote = pwrite(fd, (const uint8_t *)data + done, n - done, (off_t)(at + done));

		if (wrote < 0 && errno == EINTR)
			continue;
		if (wrote <= 0)
			return -1;
		done += (size_t)wrote;
	}
	return 0;
}

/* Zeros over `n` bytes at `at`: what an earlier table or filesystem left
 * there, which a tool reading this disk might otherwise believe. */
static int zero(int fd, uint64_t at, uint64_t n)
{
	static const uint8_t zeros[65536];

	while (n > 0u) {
		size_t piece = n < sizeof zeros ? (size_t)n : sizeof zeros;

		if (write_at(fd, zeros, piece, at) != 0)
			return -1;
		at += piece;
		n -= piece;
	}
	return 0;
}

/* --- the target ------------------------------------------------------------ */

/* Whether the block device `dev`, or a partition of it, is mounted or a
 * swap: by its numbers, in mountinfo, and by name in /proc/swaps. */
static int in_use(dev_t dev, const char *target, char *why, size_t cap)
{
	char   line[8192];
	char   base[256];
	char   path[600];
	dev_t  devs[64];
	char   names[64][256];
	unsigned count = 0u;
	FILE  *file;
	DIR   *dir;
	struct dirent *each;

	devs[count] = dev;
	snprintf(names[count++], sizeof names[0], "%s", target);
	snprintf(path, sizeof path, "/sys/dev/block/%u:%u", major(dev), minor(dev));
	/* A partition of the disk is a directory of its own under it, with
	 * a `partition` file and its numbers in `dev`. */
	if ((dir = opendir(path)) != NULL) {
		while ((each = readdir(dir)) != NULL && count < 64u) {
			char     file_path[900];
			unsigned maj, min;
			FILE    *f;

			snprintf(file_path, sizeof file_path, "%s/%s/partition", path, each->d_name);
			if (each->d_name[0] == '.' || access(file_path, F_OK) != 0)
				continue;
			snprintf(file_path, sizeof file_path, "%s/%s/dev", path, each->d_name);
			if ((f = fopen(file_path, "re")) == NULL)
				continue;
			if (fscanf(f, "%u:%u", &maj, &min) == 2) {
				devs[count] = makedev(maj, min);
				snprintf(names[count++], sizeof names[0], "/dev/%.240s", each->d_name);
			}
			fclose(f);
		}
		closedir(dir);
	}
	if ((file = fopen("/proc/self/mountinfo", "re")) != NULL) {
		while (fgets(line, sizeof line, file) != NULL) {
			unsigned maj, min;

			if (sscanf(line, "%*s %*s %u:%u", &maj, &min) != 2)
				continue;
			for (unsigned i = 0u; i < count; i++)
				if (devs[i] == makedev(maj, min)) {
					fclose(file);
					snprintf(why, cap, "%s is mounted: unmount it first", names[i]);
					return 1;
				}
		}
		fclose(file);
	}
	if ((file = fopen("/proc/swaps", "re")) != NULL) {
		while (fgets(line, sizeof line, file) != NULL) {
			if (sscanf(line, "%255s", base) != 1)
				continue;
			for (unsigned i = 0u; i < count; i++)
				if (strcmp(base, names[i]) == 0) {
					fclose(file);
					snprintf(why, cap, "%s is a swap in use", names[i]);
					return 1;
				}
		}
		fclose(file);
	}
	return 0;
}

/* The target open to write: a block device, held to be free and sized;
 * or a file, made anew. Its size into `*bytes`, 0 for a file. */
static int open_target(const char *target, int *device, uint64_t *bytes, char *why, size_t cap)
{
	struct stat is;
	int         fd;

	*device = 0;
	*bytes = 0u;
	if (stat(target, &is) == 0 && S_ISBLK(is.st_mode)) {
		*device = 1;
		if (in_use(is.st_rdev, target, why, cap) != 0)
			return -1;
		/* O_EXCL on a block device: refused while anything holds it --
		 * a mount, a device mapper, another writer -- and checked here
		 * then let go, each tool opening it after. */
		if ((fd = open(target, O_RDWR | O_EXCL | O_CLOEXEC)) < 0) {
			snprintf(why, cap, errno == EACCES || errno == EPERM ?
			         "%s is not the user's to write: a group such as disk, or a udev rule, is "
			         "the administrator's to give" : "%s: %s", target, strerror(errno));
			return -1;
		}
		if (ioctl(fd, BLKGETSIZE64, bytes) != 0 || *bytes < 4u * MIB) {
			snprintf(why, cap, "%s: its size cannot be read, or it is under 4 MiB", target);
			close(fd);
			return -1;
		}
		*bytes = *bytes / MIB * MIB;
		return fd;
	}
	if (stat(target, &is) == 0 && !S_ISREG(is.st_mode)) {
		snprintf(why, cap, "%s is neither a block device nor a file", target);
		return -1;
	}
	if ((fd = open(target, O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC, 0644)) < 0) {
		snprintf(why, cap, "%s: %s", target, strerror(errno));
		return -1;
	}
	return fd;
}

/* --- the trees ------------------------------------------------------------- */

static int tar_sink(void *context, const uint8_t *data, size_t n)
{
	return write_at(*(int *)context, data, n, (uint64_t)lseek(*(int *)context, 0, SEEK_CUR)) ==
	       0 && lseek(*(int *)context, (off_t)n, SEEK_CUR) >= 0 ? 0 : -1;
}

/* Partition `i`'s tree as a tar, `pN.tar` in the work, its size into
 * `*bytes`. */
static int tree_tar(work *w, const hull_disk_partition *p, unsigned i, uint64_t *bytes, char *why,
                    size_t cap)
{
	char        path[4300];
	char        arch[64];
	struct stat is;
	int         fd;

	snprintf(path, sizeof path, "%s/p%u.tar", w->dir, i + 1u);
	if ((fd = open(path, O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0600)) < 0) {
		snprintf(why, cap, "%s: %s", path, strerror(errno));
		return -1;
	}
	if (hull_store_layer(w->store, p->ref, tar_sink, &fd, arch, why, cap) != 0) {
		close(fd);
		return -1;
	}
	if (fstat(fd, &is) != 0 || close(fd) != 0) {
		snprintf(why, cap, "%s: %s", path, strerror(errno));
		return -1;
	}
	*bytes = (uint64_t)is.st_size;
	return 0;
}

/* --- the filesystems -------------------------------------------------------- */

/* EXTLINUX for partition `i`: its install wants the filesystem mounted
 * (it finds ldlinux.sys's blocks by FIBMAP), which as the user only a
 * guest's root can do. So the partition is made as an image of its own,
 * at what its tree needs, given to a hull vm as a volume whose guest runs
 * `extlinux --install`, copied whole to its place -- zeros too, so
 * nothing the drive held before is read as its inode tables -- and grown
 * to the partition by resize2fs, which moves no file and so keeps the
 * sectors EXTLINUX recorded. */
static int install_extlinux(work *w, const char *target, unsigned i, const layout *l,
                            const char *image, uint64_t bytes, char *why, size_t cap)
{
	char        self[4096];
	char        conf[4300];
	char        what[80];
	char        e2fsck[300], resize2fs[300], extlinux[300];
	FILE       *file;
	ssize_t     n = readlink("/proc/self/exe", self, sizeof self - 1u);
	int         written;

	if (n <= 0) {
		snprintf(why, cap, "hull cannot find itself to start the vm EXTLINUX installs in");
		return -1;
	}
	self[n] = '\0';
	if (need("extlinux", "extlinux", extlinux, sizeof extlinux, why, cap) != 0 ||
	    need("e2fsck", "e2fsprogs", e2fsck, sizeof e2fsck, why, cap) != 0 ||
	    need("resize2fs", "e2fsprogs", resize2fs, sizeof resize2fs, why, cap) != 0)
		return -1;
	snprintf(conf, sizeof conf, "%s/extlinux.conf", w->dir);
	if ((file = fopen(conf, "we")) == NULL) {
		snprintf(why, cap, "%s: %s", conf, strerror(errno));
		return -1;
	}
	/* The host's tree is the guest's, so its extlinux is the host's. */
	written = fprintf(file, "(machine (source \"dir:/\") (backend vm) (mutable overlay) "
	                  "(network none) (memory \"512M\") (volumes (volume (block \"%s\") "
	                  "(guest \"/disk\"))) (command \"/bin/sh\" \"-c\" \"mkdir -p "
	                  "/disk/boot/extlinux && %s --install /disk/boot/extlinux\"))\n", image,
	                  extlinux) > 0;
	if ((fclose(file) != 0) | !written) {
		snprintf(why, cap, "%s could not be written", conf);
		return -1;
	}
	{
		const char *run_vm[] = { self, "run", "-f", conf, NULL };

		snprintf(what, sizeof what, "EXTLINUX could not be installed in partition %u's vm",
		         i + 1u);
		if (run(run_vm, what, why, cap) != 0)
			return -1;
	}
	/* Whole, every byte, at its place. */
	{
		static uint8_t buffer[1u << 20];
		int            from = open(image, O_RDONLY | O_CLOEXEC);
		int            to = open(target, O_WRONLY | O_CLOEXEC);
		uint64_t       done = 0u;
		int            failed = from < 0 || to < 0;

		while (!failed && done < bytes) {
			ssize_t got = pread(from, buffer, sizeof buffer, (off_t)done);

			if (got <= 0 || write_at(to, buffer, (size_t)got, l->first[i] + done) != 0)
				failed = 1;
			else
				done += (uint64_t)got;
		}
		if (to >= 0 && fsync(to) != 0)
			failed = 1;
		if (from >= 0)
			close(from);
		if (to >= 0)
			close(to);
		if (failed) {
			snprintf(why, cap, "partition %u could not be written: %s", i + 1u, strerror(errno));
			return -1;
		}
	}
	if (l->length[i] > bytes) {
		char        at[4400];
		char        size[32];
		const char *check[] = { e2fsck, "-f", "-y", at, NULL };
		const char *grow[] = { resize2fs, at, size, NULL };
		char        said[600];

		snprintf(at, sizeof at, "%s?offset=%" PRIu64, target, l->first[i]);
		snprintf(size, sizeof size, "%" PRIu64 "k", l->length[i] / 1024u);
		hull_hostnet_run(check, said, sizeof said);  /* resize2fs asks for a check first */
		snprintf(what, sizeof what, "partition %u could not be grown to its size", i + 1u);
		if (run(grow, what, why, cap) != 0)
			return -1;
	}
	return 0;
}

static int make_ext(work *w, const char *target, const hull_disk_partition *p, unsigned i,
                    const layout *l, char *why, size_t cap)
{
	char        mke2fs[300];
	char        uuid[37];
	char        options[200];
	char        size[32];
	char        tar[4300];
	char        image[4300] = "";
	char        what[80];
	const char *argv[24];
	unsigned    n = 0u;
	uint64_t    part_bytes = 0u;

	if (need("mke2fs", "e2fsprogs", mke2fs, sizeof mke2fs, why, cap) != 0)
		return -1;
	uuid_text(uuid, l->part_digest[i]);
	/* For EXTLINUX, an image of its own at what the tree needs; else at
	 * the partition's place. */
	if (p->extlinux) {
		uint64_t bytes = needs(l->tree[i]);

		if (bytes > l->length[i])
			bytes = l->length[i];
		snprintf(image, sizeof image, "%s/p%u.img", w->dir, i + 1u);
		snprintf(options, sizeof options, "hash_seed=%s,root_owner=0:0", uuid);
		snprintf(size, sizeof size, "%" PRIu64 "k", bytes / 1024u);
		part_bytes = bytes;
	} else {
		snprintf(options, sizeof options, "hash_seed=%s,root_owner=0:0,offset=%" PRIu64, uuid,
		         l->first[i]);
		snprintf(size, sizeof size, "%" PRIu64 "k", l->length[i] / 1024u);
	}
	snprintf(tar, sizeof tar, "%s/p%u.tar", w->dir, i + 1u);
	argv[n++] = "env";
	argv[n++] = "E2FSPROGS_FAKE_TIME=1";
	argv[n++] = mke2fs;
	argv[n++] = "-q";
	argv[n++] = "-F";
	argv[n++] = "-t";
	argv[n++] = p->fs == HULL_DISK_EXT2 ? "ext2" : p->fs == HULL_DISK_EXT3 ? "ext3" : "ext4";
	argv[n++] = "-U";
	argv[n++] = uuid;
	argv[n++] = "-E";
	argv[n++] = options;
	if (p->features[0] != '\0') {
		argv[n++] = "-O";
		argv[n++] = p->features;
	}
	if (p->volume[0] != '\0') {
		argv[n++] = "-L";
		argv[n++] = p->volume;
	}
	if (p->ref[0] != '\0') {
		argv[n++] = "-d";
		argv[n++] = tar;
	}
	argv[n++] = p->extlinux ? image : target;
	argv[n++] = size;
	argv[n] = NULL;
	snprintf(what, sizeof what, "partition %u's filesystem could not be made", i + 1u);
	if (run(argv, what, why, cap) != 0)
		return -1;
	return p->extlinux ? install_extlinux(w, target, i, l, image, part_bytes, why, cap) : 0;
}

/* An mtools configuration of the work's own: no ~/.mtoolsrc read, and no
 * check of a FAT just made. */
static int mtools_rc(work *w, char *setting, size_t size, char *why, size_t cap)
{
	char  path[4300];
	FILE *file;
	int   written;

	snprintf(path, sizeof path, "%s/mtoolsrc", w->dir);
	if ((file = fopen(path, "we")) == NULL) {
		snprintf(why, cap, "%s: %s", path, strerror(errno));
		return -1;
	}
	written = fputs("mtools_skip_check=1\n", file) >= 0;
	if ((fclose(file) != 0) | !written) {
		snprintf(why, cap, "%s could not be written", path);
		return -1;
	}
	snprintf(setting, size, "MTOOLSRC=%s", path);
	return 0;
}

/* Partition `i`'s tree materialized at `sN` in the work. */
static int materialize(work *w, const hull_disk_partition *p, unsigned i, char *dir, size_t size,
                       char *why, size_t cap)
{
	uint64_t skipped = 0u;

	snprintf(dir, size, "%s/s%u", w->dir, i + 1u);
	return hull_store_materialize(w->store, p->ref, dir, &skipped, why, cap);
}

static int make_vfat(work *w, const char *target, const hull_disk_partition *p, unsigned i,
                     const layout *l, char *why, size_t cap)
{
	char        mkfs[300], mcopy[300];
	char        offset[32], blocks[32], serial[16];
	char        image[4400];
	char        setting[4400];
	char        source[4300];
	char        what[80];
	const char *argv[24];
	unsigned    n = 0u;

	if (need("mkfs.vfat", "dosfstools", mkfs, sizeof mkfs, why, cap) != 0)
		return -1;
	snprintf(offset, sizeof offset, "%" PRIu64, l->first[i] / SECTOR);
	snprintf(blocks, sizeof blocks, "%" PRIu64, l->length[i] / 1024u);
	snprintf(serial, sizeof serial, "%02x%02x%02x%02x", l->part_digest[i][0],
	         l->part_digest[i][1], l->part_digest[i][2], l->part_digest[i][3]);
	argv[n++] = mkfs;
	argv[n++] = "--invariant";
	argv[n++] = "-I";             /* a whole disk is the target: the offset is the partition */
	argv[n++] = "--offset";
	argv[n++] = offset;
	argv[n++] = "-i";
	argv[n++] = serial;
	if (p->type == HULL_DISK_FAT || p->type == HULL_DISK_EFI) {
		argv[n++] = "-F";
		argv[n++] = fat32(l->length[i]) ? "32" : "16";
	}
	if (p->volume[0] != '\0') {
		argv[n++] = "-n";
		argv[n++] = p->volume;
	}
	argv[n++] = target;
	argv[n++] = blocks;
	argv[n] = NULL;
	snprintf(what, sizeof what, "partition %u's filesystem could not be made", i + 1u);
	if (run(argv, what, why, cap) != 0)
		return -1;
	/* SYSLINUX: its boot sector in the filesystem's and ldlinux.sys
	 * beside the tree, written through mtools at the offset, as the
	 * user; the MBR's code that chains to it is (boot ...)'s. */
	if (p->syslinux) {
		char        syslinux[300];
		char        bytes[32];
		const char *install[] = { syslinux, "--offset", bytes, "--install", target, NULL };

		if (need("syslinux", "syslinux", syslinux, sizeof syslinux, why, cap) != 0)
			return -1;
		snprintf(bytes, sizeof bytes, "%" PRIu64, l->first[i]);
		snprintf(what, sizeof what, "SYSLINUX could not be installed in partition %u", i + 1u);
		if (run(install, what, why, cap) != 0)
			return -1;
	}
	if (p->ref[0] == '\0')
		return 0;
	if (need("mcopy", "mtools", mcopy, sizeof mcopy, why, cap) != 0 ||
	    mtools_rc(w, setting, sizeof setting, why, cap) != 0 ||
	    materialize(w, p, i, source, sizeof source, why, cap) != 0)
		return -1;
	snprintf(image, sizeof image, "%s@@%" PRIu64, target, l->first[i]);
	{
		DIR           *dir = opendir(source);
		struct dirent *each;
		int            status = 0;

		if (dir == NULL) {
			snprintf(why, cap, "%s: %s", source, strerror(errno));
			return -1;
		}
		/* Each top-level entry, sorted by the directory's own order:
		 * mtools copies a directory with -s, and a FAT keeps entries in
		 * the order they came. */
		while (status == 0 && (each = readdir(dir)) != NULL) {
			char        from[4600];
			const char *copy[] = { "env", setting, "SOURCE_DATE_EPOCH=" FAT_TIME, mcopy, "-s",
			                       "-Q", "-i", image, from, "::/", NULL };

			if (strcmp(each->d_name, ".") == 0 || strcmp(each->d_name, "..") == 0)
				continue;
			/* SYSLINUX's own two files are its install's, not the tree's:
			 * a disk imported with them carries them in its files. */
			if (p->syslinux && (strcasecmp(each->d_name, "ldlinux.sys") == 0 ||
			                    strcasecmp(each->d_name, "ldlinux.c32") == 0))
				continue;
			snprintf(from, sizeof from, "%s/%s", source, each->d_name);
			snprintf(what, sizeof what, "partition %u's tree could not be copied in", i + 1u);
			status = run(copy, what, why, cap);
		}
		closedir(dir);
		return status;
	}
}

/* --- reading back ------------------------------------------------------------ */

/* `made`, read back, against `source`: each file's bytes, each link's
 * target. What the filesystem's tools add -- ext's lost+found, SYSLINUX's
 * own two files where `loader` -- is no part of the tree. */
static int compare_trees(const char *made, const char *source, unsigned i, int loader, char *why,
                         size_t cap)
{
	char        diff[300];
	char        said[600];
	const char *argv[] = { diff, "-r", "-q", "--no-dereference", "-x", "lost+found",
	                       "-x", loader ? "ldlinux.sys" : "lost+found",
	                       "-x", loader ? "ldlinux.c32" : "lost+found",
	                       "-x", loader ? "LDLINUX.SYS" : "lost+found",
	                       "-x", loader ? "LDLINUX.C32" : "lost+found", source, made, NULL };

	if (need("diff", "diffutils", diff, sizeof diff, why, cap) != 0)
		return -1;
	if (hull_hostnet_run(argv, said, sizeof said) == 0)
		return 0;
	said[strcspn(said, "\n")] = '\0';
	snprintf(why, cap, "partition %u read back differs from its tree: %s", i + 1u, said);
	return -1;
}

static int verify_ext(work *w, const char *target, const hull_disk_partition *p, unsigned i,
                      const layout *l, char *why, size_t cap)
{
	char        e2fsck[300], debugfs[300];
	char        at[4400];
	char        out[4300], command[4400], source[4300];
	char        what[80];
	const char *check[] = { e2fsck, "-f", "-n", at, NULL };

	if (need("e2fsck", "e2fsprogs", e2fsck, sizeof e2fsck, why, cap) != 0 ||
	    need("debugfs", "e2fsprogs", debugfs, sizeof debugfs, why, cap) != 0)
		return -1;
	snprintf(at, sizeof at, "%s?offset=%" PRIu64, target, l->first[i]);
	snprintf(what, sizeof what, "partition %u is not a clean filesystem read back", i + 1u);
	if (run(check, what, why, cap) != 0)
		return -1;
	if (p->ref[0] == '\0')
		return 0;
	snprintf(out, sizeof out, "%s/r%u", w->dir, i + 1u);
	if (mkdir(out, 0700) != 0) {
		snprintf(why, cap, "%s: %s", out, strerror(errno));
		return -1;
	}
	/* rdump of / writes the root's entries into `out`, as the user: what
	 * a user cannot make -- a device -- it says, and the diff does not
	 * see it either, materialize leaving it out the same way. */
	snprintf(command, sizeof command, "rdump / %s", out);
	{
		const char *dump[] = { debugfs, "-R", command, at, NULL };
		char        said[600];

		hull_hostnet_run(dump, said, sizeof said);
	}
	if (materialize(w, p, i, source, sizeof source, why, cap) != 0)
		return -1;
	return compare_trees(out, source, i, p->extlinux, why, cap);
}

static int verify_vfat(work *w, const char *target, const hull_disk_partition *p, unsigned i,
                       const layout *l, char *why, size_t cap)
{
	char        mcopy[300];
	char        setting[4400];
	char        image[4400];
	char        out[4300], source[4300];
	char        what[80];
	struct stat is;

	if (p->ref[0] == '\0')
		return 0;
	if (need("mcopy", "mtools", mcopy, sizeof mcopy, why, cap) != 0 ||
	    mtools_rc(w, setting, sizeof setting, why, cap) != 0)
		return -1;
	snprintf(image, sizeof image, "%s@@%" PRIu64, target, l->first[i]);
	snprintf(out, sizeof out, "%s/r%u", w->dir, i + 1u);
	if (mkdir(out, 0700) != 0) {
		snprintf(why, cap, "%s: %s", out, strerror(errno));
		return -1;
	}
	{
		const char *copy[] = { "env", setting, mcopy, "-s", "-n", "-Q", "-i", image, "::*", out,
		                       NULL };

		snprintf(what, sizeof what, "partition %u could not be read back", i + 1u);
		if (run(copy, what, why, cap) != 0)
			return -1;
	}
	snprintf(source, sizeof source, "%s/s%u", w->dir, i + 1u);
	if (stat(source, &is) != 0 && materialize(w, p, i, source, sizeof source, why, cap) != 0)
		return -1;
	return compare_trees(out, source, i, p->syslinux, why, cap);
}

/* --- the whole --------------------------------------------------------------- */

static void clean(work *w)
{
	char *slash = strrchr(w->dir, '/');
	int   parent;

	if (w->dir[0] == '\0' || slash == NULL)
		return;
	*slash = '\0';
	parent = open(w->dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	if (parent >= 0) {
		hull_store_remove_tree(parent, slash + 1);
		close(parent);
	}
	*slash = '/';
}

int hull_disk_write(hull_store *store, const hull_disk *disk, const char *target,
                    hull_disk_report *report, char *why, size_t cap)
{
	static layout l;
	static table  t;
	work          w = { .store = store };
	uint64_t      bytes;
	int           fd = -1;
	int           device = 0;
	int           made = 0;      /* a file the write made, to take away should it fail */
	int           status = -1;

	memset(report, 0, sizeof *report);
	memset(&l, 0, sizeof l);
	if (disk->count == 0u || disk->count > (disk->gpt ? HULL_DISK_PARTITIONS : 4u)) {
		snprintf(why, cap, disk->gpt ? "a disk has 1 to 16 partitions here" :
		         "a dos label has 1 to 4 partitions, all primary");
		return -1;
	}
	for (unsigned i = 0u; i < disk->count; i++) {
		const hull_disk_partition *p = &disk->partition[i];

		if (p->ref[0] != '\0' && p->fs == HULL_DISK_NONE) {
			snprintf(why, cap, "partition %u has a source and no filesystem to hold it", i + 1u);
			return -1;
		}
		if (p->features[0] != '\0' && p->fs != HULL_DISK_EXT2 && p->fs != HULL_DISK_EXT3 &&
		    p->fs != HULL_DISK_EXT4) {
			snprintf(why, cap, "partition %u: (features ...) are mke2fs's, for an ext "
			         "filesystem", i + 1u);
			return -1;
		}
		if (p->extlinux && p->fs != HULL_DISK_EXT2 && p->fs != HULL_DISK_EXT3 &&
		    p->fs != HULL_DISK_EXT4) {
			snprintf(why, cap, "partition %u: EXTLINUX boots from ext; on FAT it is SYSLINUX",
			         i + 1u);
			return -1;
		}
		if (p->syslinux && p->fs != HULL_DISK_VFAT) {
			snprintf(why, cap, "partition %u: SYSLINUX boots from FAT; on ext it is EXTLINUX, "
			         "(loader extlinux)", i + 1u);
			return -1;
		}
		if (p->type == HULL_DISK_BIOS && !disk->gpt) {
			snprintf(why, cap, "partition %u: a BIOS boot partition is GPT's", i + 1u);
			return -1;
		}
	}
	snprintf(w.dir, sizeof w.dir, "%s/tmp/disk-XXXXXX", store->root);
	if (mkdtemp(w.dir) == NULL) {
		snprintf(why, cap, "%s: %s", w.dir, strerror(errno));
		w.dir[0] = '\0';
		return -1;
	}
	/* The trees first, as tars: their sizes size the disk, and a tree
	 * that cannot be read costs nothing written. */
	for (unsigned i = 0u; i < disk->count; i++)
		if (disk->partition[i].ref[0] != '\0' &&
		    tree_tar(&w, &disk->partition[i], i, &l.tree[i], why, cap) != 0)
			goto done;
	if ((fd = open_target(target, &device, &bytes, why, cap)) < 0)
		goto done;
	made = !device;
	if (device && disk->bytes != 0u && disk->bytes > bytes) {
		snprintf(why, cap, "the disk is described as %" PRIu64 " MiB and %s has %" PRIu64,
		         disk->bytes / MIB, target, bytes / MIB);
		goto done;
	}
	/* A drive's own size; a file's as described, or what it needs. */
	if (lay_out(disk, &l, device ? bytes : disk->bytes != 0u ? round_mib(disk->bytes) : 0u, why,
	            cap) != 0)
		goto done;
	identify(disk, &l);
	if (make_table(disk, &l, &t, why, cap) != 0)
		goto done;
	if (!device && ftruncate(fd, (off_t)l.bytes) != 0) {
		snprintf(why, cap, "%s: %s", target, strerror(errno));
		goto done;
	}
	/* On a drive, what was there before at either end -- a table, its
	 * GPT copy -- goes, and each partition's first MiB, where an old
	 * filesystem's signature would be read before the new one's. */
	if (device && (zero(fd, 0u, MIB) != 0 || zero(fd, l.bytes - MIB, MIB) != 0)) {
		snprintf(why, cap, "%s could not be written: %s", target, strerror(errno));
		goto done;
	}
	for (unsigned i = 0u; device && i < disk->count; i++)
		if (zero(fd, l.first[i], MIB) != 0) {
			snprintf(why, cap, "%s could not be written: %s", target, strerror(errno));
			goto done;
		}
	for (unsigned i = 0u; i < t.count; i++)
		if (write_at(fd, t.region[i].data, t.region[i].length, t.region[i].at) != 0) {
			snprintf(why, cap, "%s could not be written: %s", target, strerror(errno));
			goto done;
		}
	if (fsync(fd) != 0) {
		snprintf(why, cap, "%s: %s", target, strerror(errno));
		goto done;
	}
	/* Let go, so each tool may open the drive for itself. */
	close(fd);
	fd = -1;
	for (unsigned i = 0u; i < disk->count; i++) {
		const hull_disk_partition *p = &disk->partition[i];

		if ((p->fs == HULL_DISK_EXT2 || p->fs == HULL_DISK_EXT3 || p->fs == HULL_DISK_EXT4) &&
		    make_ext(&w, target, p, i, &l, why, cap) != 0)
			goto done;
		if (p->fs == HULL_DISK_VFAT && make_vfat(&w, target, p, i, &l, why, cap) != 0)
			goto done;
	}

	/* Read back: the cache dropped, so the drive is what answers. */
	if ((fd = open(target, O_RDWR | O_CLOEXEC)) < 0 || fsync(fd) != 0) {
		snprintf(why, cap, "%s: %s", target, strerror(errno));
		goto done;
	}
	posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED);
	for (unsigned i = 0u; i < t.count; i++) {
		static uint8_t back[ENTRIES * 128u];

		if (pread(fd, back, t.region[i].length, (off_t)t.region[i].at) !=
		    (ssize_t)t.region[i].length ||
		    memcmp(back, t.region[i].data, t.region[i].length) != 0) {
			snprintf(why, cap, "%s read back does not hold the table written at byte %" PRIu64,
			         target, t.region[i].at);
			goto done;
		}
	}
	for (unsigned i = 0u; i < disk->count; i++) {
		const hull_disk_partition *p = &disk->partition[i];

		if ((p->fs == HULL_DISK_EXT2 || p->fs == HULL_DISK_EXT3 || p->fs == HULL_DISK_EXT4) &&
		    verify_ext(&w, target, p, i, &l, why, cap) != 0)
			goto done;
		if (p->fs == HULL_DISK_VFAT && verify_vfat(&w, target, p, i, &l, why, cap) != 0)
			goto done;
	}
	/* The kernel told the table changed, where it may be: a drive whose
	 * partitions it already knew. Refused while one is open, which is
	 * nothing the write needs. */
	if (device && ioctl(fd, BLKRRPART) != 0)
		snprintf(report->note, sizeof report->note, "the kernel was not told of the new table "
		         "(%s; re-reading one is root's): it reads it when the drive is next attached, "
		         "or at `blockdev --rereadpt`", strerror(errno));
	report->bytes = l.bytes;
	report->device = device;
	report->verified = 1;
	for (unsigned i = 0u; i < disk->count; i++) {
		report->first[i] = l.first[i];
		report->length[i] = l.length[i];
	}
	status = 0;
done:
	if (fd >= 0)
		close(fd);
	if (status != 0 && made)
		unlink(target);
	clean(&w);
	return status;
}
