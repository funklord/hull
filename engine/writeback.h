/*
 * writeback.h -- (writeback sync-on-stop), section 3.9's rule 2: a block
 * projection's tree, dumped out of its image by the qemu engine, made the
 * directory's again once the machine has stopped clean.
 *
 * hull_writeback_mirror makes `dir` hold what `stage` holds: a file,
 * directory or symlink of the stage's written where it differs, with its
 * mode and times, and one the stage lacks removed. What the dump cannot
 * carry is left as it was in `dir` -- a device node, a FIFO, a socket --
 * and a top-level lost+found the image has and the directory had not is
 * not made, mke2fs having put it there. Everything is the user's: the
 * guest's owners are not carried, a user being able to give no other.
 * Hard links come back as copies. Nothing follows a symlink.
 */
#ifndef HULL_WRITEBACK_H
#define HULL_WRITEBACK_H

#include <stddef.h>

/* 0, or -1 with why in `why`, `dir` then part written: the crash window
 * the rule documents. */
int hull_writeback_mirror(const char *stage, const char *dir, char *why, size_t cap);

/* `name` under the directory `parent`, removed with all it holds, not
 * following a symlink. 0, or -1 with errno. */
int hull_writeback_remove(int parent, const char *name);

#endif
