/*
 * vmconf.h -- another hypervisor's own machine file as a hull machine
 * (project.md section 3.13, Attach): VMware's .vmx and VirtualBox's
 * .vbox read as a (machine ...) form of the pc class, in memory, so an
 * existing virtual machine runs as it stands, its disks in place.
 */

#ifndef HULL_VMCONF_H
#define HULL_VMCONF_H

#include <stdio.h>

/* Whether a spec reader takes `path` as one: a name ending .vmx or .vbox. */
int hull_vmconf_is_file(const char *path);

/* The machine file at `path` as a (machine ...) form, allocated, what it
 * does not carry over said on `notes` as `PATH: what`. NULL, having said
 * why, where the file cannot be read or names what qemu cannot run. */
char *hull_vmconf_form(const char *path, FILE *notes);

#endif
