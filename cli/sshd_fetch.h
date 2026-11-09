#ifndef HULL_SSHD_FETCH_H
#define HULL_SSHD_FETCH_H

#include <stddef.h>

/* `hull sshd fetch` (section 8.1 item 3): the pinned dropbear of
 * engine/sshd.h, with what it links and an sftp-server, downloaded,
 * checked and unpacked into the user's cache, its directory printed; at
 * once where it is there already. 0, or -1 with `why`. */
int hull_sshd_fetch(const char *arch, char *why, size_t cap);

#endif
