/*
 * pack_fuzz.c -- libFuzzer over store/pack.c's hull_pack_inspect, which
 * reads somebody else's pack (`hull inspect`, project.md section 3.13):
 * its header, its index, its manifest's zstd frame and the data's length.
 * Each input is a pack, given on a memfd; what is said goes to /dev/null.
 * A crash, a leak or undefined behaviour stops the run. `make check-fuzz`
 * runs it.
 */

#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>

#include "store.h"

static FILE *sink;

int LLVMFuzzerInitialize(int *argc, char ***argv)
{
	(void)argc;
	(void)argv;
	if ((sink = fopen("/dev/null", "w")) == NULL)
		abort();
	return 0;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	char why[4400];
	int  fd = memfd_create("pack", MFD_CLOEXEC);

	if (fd < 0 || (size != 0u && write(fd, data, size) != (ssize_t)size) ||
	    lseek(fd, 0, SEEK_SET) != 0)
		abort();
	(void)hull_pack_inspect(fd, sink, why, sizeof why);
	close(fd);
	return 0;
}
