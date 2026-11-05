/*
 * media.h -- a disc as a collection of plain files and back, exactly
 * (project.md section 3.10): `hull media import`'s decomposition and the
 * reassembly a machine's disc and `hull media verify` both are.
 *
 *     DIR/files/...             the disc's tree, plain files
 *     DIR/media.conf            (disc (volume V) (bytes N) (sha256 H)), and
 *                               (sparse true) where H is over the non-zero
 *                               blocks of an image mostly holes, and no
 *                               plain sha256
 *     DIR/layout.conf           a line each, `SECTOR SIZE PATH`: where on
 *                               the disc each file's bytes sit
 *     DIR/quirks/sectors/0xS.raw  every run of bytes from sector S that
 *                               placing the files does not give: the
 *                               descriptors, the directories, a boot area,
 *                               a file no name could be given
 *
 * A disk of FAT is the same, its unit the disk's sector, (unit N) in
 * media.conf, and a file whose clusters are apart a line for each run,
 * `SECTOR SIZE @OFFSET/TOTAL PATH`. So is an ext2, ext3 or ext4 filesystem,
 * its regular files; a link's, a device's and every owner and mode being
 * the patch's. A partitioned disk, by GPT or MBR, is each such partition's
 * tree under files/pN, N its place in the table; a partition of anything
 * else is the patch's.
 *
 * A raw dump, `IMAGE.cue` and the .bin files it names, carries more: the
 * disc above is its data track's 2048 bytes a sector, and
 *
 *     DIR/disc.cue              the cue sheet, as it came
 *     DIR/tracks/NN.wav         an audio track, its samples the dump's
 *     DIR/quirks/raw/0xS.raw    every run of bytes from raw sector S that
 *                               regenerating the sector does not give:
 *                               a sync, a header, an EDC or ECC that are
 *                               not what the data says, which is how a
 *                               disc's copy protection is written, and
 *                               gaps between tracks that are not silence
 *
 * media.conf then says (raw (bytes N) (sha256 H) (sectors D)), D the data
 * track's sectors, each track (at BYTE) (bytes N) in the dump, and for a
 * sheet of several files (parts (part NAME BYTES) ...), the dump being
 * them end to end.
 *
 * The projection is the files placed at their sectors over nothing; the
 * quirks are what differs from the original, found by diffing, so nobody
 * writes them by hand; and the two together are the original, whose hash
 * is checked. An image that is no ISO9660 becomes one run from sector 0,
 * the honest blob, no worse than the file it was.
 */

#ifndef HULL_MEDIA_H
#define HULL_MEDIA_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct hull_media_report {
	unsigned long long files;       /* given a place in files/ */
	unsigned long long bytes;       /* the image's */
	unsigned long long patch_bytes; /* in quirks/: how much is not understood */
} hull_media_report;

/* `image`, a disc image or a .cue, decomposed into the new directory
 * `dir`, the result reassembled
 * and its hash held to the image's before it is called done. 0, or -1
 * with why in `detail`, and nothing left at `dir`. */
int hull_media_import(const char *image, const char *dir, hull_media_report *report, char *detail,
                      size_t cap);

/* `dir`'s raw dump, reassembled to the empty file `fd` and its hash held
 * to media.conf's. 0, or -1 with why in `detail`; 1 where the collection
 * was imported from no raw dump. */
int hull_media_write_raw(const char *dir, int fd, char *detail, size_t cap);

/* What `hull media export` writes: the image, or the raw dump, at `out`, a
 * file; or where the dump was several, `out` a new directory of them and
 * the sheet. 0, or -1 with why in `detail`. */
int hull_media_export(const char *dir, const char *out, char *detail, size_t cap);

/* Whether `dir` is a collection this file made: has it a layout.conf. */
int hull_media_exact(const char *dir);

/* The collection reassembled to the empty file `fd`, its hash held to
 * media.conf's. 0, or -1 with why in `detail`. */
int hull_media_write(const char *dir, int fd, char *detail, size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* HULL_MEDIA_H */
