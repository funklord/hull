/*
 * iso.h -- a directory as an ISO9660 image, the same bytes for the same
 * tree: the projector of a disc collection's data track (project.md
 * section 3.10), its files/ made the disc a DOS program reads.
 */

#ifndef HULL_ISO_H
#define HULL_ISO_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Write the tree under `root` to `fd` as an ISO9660 image of level 1,
 * labelled `volume`: every name DOS's 8.3 in capitals, as a disc of the
 * DOS era is, directories at most eight deep, each file contiguous, and
 * every date and order fixed, so a tree gives the same image each time.
 * Returns 0, or -1 with why in `detail`, naming the entry: a name that is
 * not 8.3, a link or a device, a tree too deep or too large, or a write
 * that failed.
 */
int hull_iso_write(const char *root, const char *volume, int fd, char *detail, size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* HULL_ISO_H */
