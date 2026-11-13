/*
 * display.h -- a screen's local display (project.md section 5.2): qemu's
 * D-Bus display, reached by a QMP monitor in the machine's directory and
 * linked as $XDG_RUNTIME_DIR/hull/display/ADDRESS, where hull-gui finds it
 * by the address it shows, as VNC is at ADDRESS:5900. The qemu and pc
 * engines both give one.
 */

#ifndef HULL_ENGINE_DISPLAY_H
#define HULL_ENGINE_DISPLAY_H

#include <stddef.h>

/* Whether `monitor`, the monitor's path, fits a unix socket's address. */
int hull_display_fits(const char *monitor);

/* Link `monitor` as hull/display/`address`, the link's path into `link`,
 * or "" where none could be made: a screen with no local display, still
 * VNC's. */
void hull_display_link(const char *monitor, const char *address, char *link, size_t cap);

/* Remove `link` where it still names `monitor`, a later machine at the
 * same address having replaced it otherwise; `link` becomes "". */
void hull_display_unlink(char *link, const char *monitor);

#endif
