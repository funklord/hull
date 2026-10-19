/*
 * usage.c -- what the host spends on a machine, read from /proc.
 */

#define _POSIX_C_SOURCE 200809L

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "usage.h"

/* The first number after `key` on a line of `path` that starts with it,
 * into `out`; 0 for a line not there, -1 for a file that cannot be read. */
static int field(const char *path, const char *key, unsigned long long *out)
{
	char   line[256];
	size_t n = strlen(key);
	FILE  *f = fopen(path, "re");
	int    found = 0;

	if (f == NULL)
		return -1;
	while (!found && fgets(line, sizeof line, f) != NULL)
		if (strncmp(line, key, n) == 0)
			found = sscanf(line + n, "%llu", out) == 1;
	fclose(f);
	return found;
}

/* utime, stime, cutime and cstime, the 14th to 17th fields of
 * /proc/<pid>/stat, in microseconds. The name, the 2nd, may hold spaces
 * and parentheses, so the fields are counted from its last `)`. */
static int cpu_of(pid_t pid, unsigned long long *out)
{
	char               path[64];
	char               line[1024];
	unsigned long long ticks[4];
	long               hz = sysconf(_SC_CLK_TCK);
	char              *at;
	FILE              *f;
	size_t             n;

	snprintf(path, sizeof path, "/proc/%ld/stat", (long)pid);
	if ((f = fopen(path, "re")) == NULL)
		return 0;
	n = fread(line, 1u, sizeof line - 1u, f);
	fclose(f);
	line[n] = '\0';
	if ((at = strrchr(line, ')')) == NULL || hz <= 0 ||
	    sscanf(at + 1, " %*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %llu %llu %llu %llu",
	           &ticks[0], &ticks[1], &ticks[2], &ticks[3]) != 4)
		return 0;
	*out = (ticks[0] + ticks[1] + ticks[2] + ticks[3]) * 1000000ull / (unsigned long long)hz;
	return 1;
}

int hull_usage_add(pid_t pid, hull_usage *usage)
{
	char               path[64];
	unsigned long long cpu;
	unsigned long long read_bytes;
	unsigned long long write_bytes;
	unsigned long long pss = 0u;

	if (!cpu_of(pid, &cpu))
		return 0;
	snprintf(path, sizeof path, "/proc/%ld/io", (long)pid);
	if (field(path, "read_bytes:", &read_bytes) != 1 ||
	    field(path, "write_bytes:", &write_bytes) != 1)
		return 0;
	/* A zombie's has no lines, and holds nothing. */
	snprintf(path, sizeof path, "/proc/%ld/smaps_rollup", (long)pid);
	if (field(path, "Pss:", &pss) < 0)
		return 0;
	usage->cpu += cpu;
	usage->io += read_bytes + write_bytes;
	usage->memory += pss * 1024u;
	return 1;
}

int hull_usage_namespace(pid_t pid, hull_usage *usage)
{
	char           path[64];
	struct stat    own;
	struct stat    other;
	struct dirent *entry;
	DIR           *proc;

	snprintf(path, sizeof path, "/proc/%ld/ns/pid", (long)pid);
	if (stat(path, &own) != 0 || (proc = opendir("/proc")) == NULL)
		return 0;
	while ((entry = readdir(proc)) != NULL) {
		char *end;
		long  n = strtol(entry->d_name, &end, 10);

		if (*end != '\0' || n <= 0)
			continue;
		snprintf(path, sizeof path, "/proc/%ld/ns/pid", n);
		if (stat(path, &other) == 0 && other.st_ino == own.st_ino &&
		    other.st_dev == own.st_dev)
			hull_usage_add((pid_t)n, usage);
	}
	closedir(proc);
	return 1;
}

size_t hull_usage_encode(const hull_usage *usage, unsigned char *out, size_t cap)
{
	char cpu[24];
	char io[24];
	char memory[24];
	int  n;

	snprintf(cpu, sizeof cpu, "%llu", usage->cpu);
	snprintf(io, sizeof io, "%llu", usage->io);
	snprintf(memory, sizeof memory, "%llu", usage->memory);
	n = snprintf((char *)out, cap, "(5:stats(3:cpu%zu:%s)(2:io%zu:%s)(6:memory%zu:%s))",
	             strlen(cpu), cpu, strlen(io), io, strlen(memory), memory);
	return n < 0 || (size_t)n >= cap ? 0u : (size_t)n;
}
