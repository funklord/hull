/* setup.h -- `hull setup`; see setup.c. */

#ifndef HULL_CLI_SETUP_H
#define HULL_CLI_SETUP_H

#include <stddef.h>

/* A machine directory DIR that installs Windows from the user's own ISO
 * unattended and then runs it with an OpenSSH server the user's keys open:
 * its answer files on a USB stick, the install's spec and the machine's.
 * `index` is the edition's image in the ISO, `user` the account made.
 * 0, or -1 with `why`; DIR must not exist. */
int hull_setup_windows(const char *iso, const char *dir, unsigned index, const char *user,
                       char *why, size_t cap);

/* A machine directory DIR that installs FreeDOS onto a blank disk of
 * `megabytes` from the user's own FreeDOS boot floppy BOOT, with nobody at
 * it, and then boots that disk: the floppy's FDAUTO.BAT partitions,
 * reboots, formats with the system and powers off. With `packages`, a
 * FreeDOS USB image holding packages/base's zips, the base packages too,
 * copied onto C: as installed. 0, or -1 with `why`; DIR must not exist. */
int hull_setup_freedos(const char *boot, const char *dir, unsigned megabytes, const char *packages,
                       char *why, size_t cap);

#endif
