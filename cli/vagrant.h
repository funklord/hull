/*
 * vagrant.h -- the Vagrant dialect (project.md section 9): a Vagrantfile's
 * declarative subset as the (machines ...) form, in memory, so the file
 * stays the source of truth as a compose file does.
 */

#ifndef HULL_VAGRANT_H
#define HULL_VAGRANT_H

#include <stdio.h>

/* Whether a spec reader takes `path` as one: named Vagrantfile. */
int hull_vagrant_is_file(const char *path);

/* The Vagrantfile at `path` as a (machines ...) form, allocated, what it
 * does not translate said on `notes` as `PATH: what`. A Vagrantfile is
 * Ruby: a line that is not one of the declarations read here is refused,
 * naming it, rather than run or guessed at. NULL, having said why. */
char *hull_vagrant_form(const char *path, FILE *notes);

/* What the last translation's machines provision: each a (build ...) form
 * of the box and its shell provisioners, and the ref the machine runs
 * from. How many, `list` their array. */
#define MAX_VAGRANT_MACHINES 16u

typedef struct hull_vagrant_build {
	char  ref[256];
	char *form;
} hull_vagrant_build;

unsigned hull_vagrant_builds(const hull_vagrant_build **list);

#endif
