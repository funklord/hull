/*
 * import_fuzz.c -- libFuzzer over cli/import.c, which reads a compose
 * file or a Dockerfile somebody else wrote: each input written to a file
 * under $FUZZ_DIR/import-work and read as both, the compose half's notes
 * and the Dockerfile half's output thrown away. A crash, a sanitizer's
 * report or a leak stops the run. `make check-fuzz` runs it.
 */

#define _GNU_SOURCE
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "import.h"

static char base[3900];

int LLVMFuzzerInitialize(int *argc, char ***argv)
{
	const char *dir = getenv("FUZZ_DIR");
	int         null = open("/dev/null", O_WRONLY | O_CLOEXEC);

	(void)argc;
	(void)argv;
	snprintf(base, sizeof base, "%s/import-work", dir != NULL ? dir : ".");
	mkdir(base, 0700);
	/* The Dockerfile half prints its (build ...) on stdout. */
	if (null < 0 || dup2(null, 1) < 0)
		abort();
	close(null);
	return 0;
}

static void put(const char *path, const uint8_t *data, size_t size)
{
	int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);

	if (fd < 0 || (size > 0u && write(fd, data, size) != (ssize_t)size))
		abort();
	close(fd);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	char  compose[4000];
	char  dockerfile[4000];
	char *made;
	FILE *notes = fopen("/dev/null", "w");

	if (notes == NULL)
		abort();
	snprintf(compose, sizeof compose, "%s/compose.yaml", base);
	snprintf(dockerfile, sizeof dockerfile, "%s/Dockerfile", base);
	put(compose, data, size);
	put(dockerfile, data, size);
	made = hull_import_compose(compose, notes);
	free(made);
	fclose(notes);
	hull_import(dockerfile);
	fflush(stdout);
	return 0;
}
