/*
 * tree_tar.c -- engine/tree.c's tar of the directory argv[1], on stdout,
 * of its own filesystem alone with `--one-fs` before it,
 * for test/tree_test.sh to read back with tools that share none of it.
 * Exits 1 with the writer's reason on stderr.
 */

#include <stdio.h>
#include <string.h>

#include "tree.h"

static int out(void *context, const uint8_t *data, size_t n)
{
	(void)context;
	return fwrite(data, 1u, n, stdout) == n ? 0 : -1;
}

int main(int argc, char **argv)
{
	char detail[512];
	int  one_fs = argc == 3 && strcmp(argv[1], "--one-fs") == 0;

	if (argc != 2 && !one_fs) {
		fprintf(stderr, "usage: tree_tar [--one-fs] DIR\n");
		return 2;
	}
	if (hull_tree_tar_fs(argv[argc - 1], one_fs, out, NULL, detail, sizeof detail) != 0) {
		fprintf(stderr, "tree_tar: %s\n", detail);
		return 1;
	}
	return fflush(stdout) == 0 ? 0 : 1;
}
