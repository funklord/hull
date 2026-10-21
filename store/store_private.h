/*
 * store_private.h -- what store.c and unpack.c share, and nothing outside
 * the store calls.
 */

#ifndef HULL_STORE_PRIVATE_H
#define HULL_STORE_PRIVATE_H

/* `name` in `parent`, and all beneath it: each directory made the user's
 * to empty first, since a tree's modes may not let it be, and no symlink
 * followed, since one is removed as itself. 0, or -1 when something
 * stayed. */
int hull_store_remove_tree(int parent, const char *name);

#endif /* HULL_STORE_PRIVATE_H */
