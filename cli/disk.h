/*
 * disk.h -- `hull disk write` (section 3.13): a disk described without a
 * machine -- its label, its partitions, each one's filesystem and the tree
 * that fills it, the boot code its first sector carries -- written to an
 * image file or to a drive, sized to what it is written to, and read back.
 */
#ifndef HULL_DISK_H
#define HULL_DISK_H

#include <stddef.h>
#include <stdint.h>

#include "store.h"

#ifdef __cplusplus
extern "C" {
#endif

#define HULL_DISK_PARTITIONS 16u

typedef enum hull_disk_type {
	HULL_DISK_LINUX,
	HULL_DISK_EFI,
	HULL_DISK_FAT,
	HULL_DISK_BIOS,
	HULL_DISK_SWAP,
} hull_disk_type;

typedef enum hull_disk_fs {
	HULL_DISK_NONE,
	HULL_DISK_EXT2,
	HULL_DISK_EXT3,
	HULL_DISK_EXT4,
	HULL_DISK_VFAT,
} hull_disk_fs;

typedef struct hull_disk_partition {
	char           name[37];      /* GPT's name for it; "" for none */
	uint64_t       bytes;         /* 0: the rest of the disk */
	hull_disk_type type;
	int            active;        /* dos: the boot flag; gpt: legacy BIOS bootable */
	hull_disk_fs   fs;
	char           features[512]; /* mke2fs -O's list, comma-separated; "" for none */
	char           volume[64];    /* the filesystem's label; "" for none */
	char           ref[200];      /* the stored tree that fills it; "" for none */
	int            syslinux;      /* (loader syslinux): its boot sector and ldlinux.sys */
	int            extlinux;      /* (loader extlinux): the same in /boot/extlinux of an ext */
} hull_disk_partition;

typedef struct hull_disk {
	int                 gpt;
	uint64_t            bytes;     /* 0: the target's, or what the partitions need */
	char                boot[4400]; /* boot code's file, its first 440 bytes; "" for none */
	unsigned            count;
	hull_disk_partition partition[HULL_DISK_PARTITIONS];
} hull_disk;

/* What a write did, for the caller to say. */
typedef struct hull_disk_report {
	uint64_t bytes;                          /* the disk's */
	int      device;                         /* the target was a block device */
	int      verified;                       /* read back and found as written */
	uint64_t first[HULL_DISK_PARTITIONS];    /* each partition's first byte */
	uint64_t length[HULL_DISK_PARTITIONS];   /* and its length */
	char     note[600];                      /* what the reader should know, or "" */
} hull_disk_report;

/* `disk` written to `target`: a block device, which must be the user's to
 * write and nothing's mounted, its whole size used; or a file, made anew
 * at `disk->bytes` or at what the partitions need. Then read back: the
 * table, each ext filesystem checked by e2fsck, and each partition's tree
 * compared with the tree it was filled from. 0, or -1 with `why`. */
int hull_disk_write(hull_store *store, const hull_disk *disk, const char *target,
                    hull_disk_report *report, char *why, size_t cap);

#ifdef __cplusplus
}
#endif

#endif
