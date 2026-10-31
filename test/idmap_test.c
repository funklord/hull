/*
 * idmap_test.c -- the map hull asks newuidmap and newgidmap for: the
 * user's own id at 0, its subordinate range from 1, and each further
 * line of the user's in /etc/subgid as itself, the range split around it.
 */

#include <stdio.h>
#include <string.h>

#include "idmap.h"

static unsigned cases;
static unsigned failures;

/* The map of `own`, `main` and `extras`, each line `inside outside count`
 * after a space, against `want`. */
static void check(const char *what, unsigned long own, hull_id_range main,
                  const hull_id_range *extras, size_t extra_count, const char *want)
{
	hull_id_line lines[HULL_ID_LINES];
	size_t       n = hull_id_map(own, main, extras, extra_count, lines, HULL_ID_LINES);
	char         got[512] = "";
	size_t       at = 0u;

	for (size_t i = 0u; i < n && at < sizeof got; i++)
		at += (size_t)snprintf(got + at, sizeof got - at, " %lu %lu %lu", lines[i].inside,
		                       lines[i].outside, lines[i].count);
	cases++;
	if (strcmp(got, want) != 0) {
		failures++;
		printf("FAIL %s:\n  got  %s\n  want %s\n", what, got, want);
	}
}

int main(void)
{
	const hull_id_range main_range = { 100000u, 65536u };
	const hull_id_range users[] = { { 100u, 1u } };
	const hull_id_range two[] = { { 3000u, 2u }, { 100u, 1u } };
	const hull_id_range far[] = { { 200000u, 10u } };
	const hull_id_range useless[] = { { 0u, 5u }, { 1000u, 1u }, { 100500u, 1u } };
	const hull_id_range nested[] = { { 100u, 10u }, { 105u, 1u } };

	check("the user and its range, as before", 1000u, main_range, NULL, 0u,
	      " 0 1000 1 1 100000 65536");
	/* The workspace's case: a tree of the group users, 100, which the
	 * user's line user:100:1 grants. */
	check("a group of the user's as itself, the range split around it", 1000u, main_range, users,
	      1u, " 0 1000 1 1 100000 99 100 100 1 101 100100 65436");
	check("two, given out of order", 1000u, main_range, two, 2u,
	      " 0 1000 1 1 100000 99 100 100 1 101 100100 2899 3000 3000 2 3002 103001 62535");
	check("one past the range inside, as itself, the range whole", 1000u, main_range, far, 1u,
	      " 0 1000 1 1 100000 65536 200000 200000 10");
	check("0, the user's own and one the range gives out, left out", 1000u, main_range, useless,
	      3u, " 0 1000 1 1 100000 65536");
	check("one within another, once", 1000u, main_range, nested, 2u,
	      " 0 1000 1 1 100000 99 100 100 10 110 100109 65427");
	printf("idmap: %u cases, %u failed\n", cases, failures);
	return failures != 0u;
}
