/*
 * names.h -- the addresses hull gives a file's machines on the segments
 * it addresses, and the names they know each other by (Appendix D).
 *
 * A segment is addressed where a machine names it with a subnet,
 * `(segments (wire (subnet "10.99.0.0/24")))`; a bare `(segments "wire")`
 * is left to its machines, as it always was. The file is drawn whole,
 * since it is planned whole (section 4.8): every machine on an addressed
 * segment gets an address, and is given every other's.
 *
 * The address is the subnet's base plus 2 plus FNV-1a over the machine's
 * id, `/` and the segment, modulo what the subnet holds past its network,
 * its .1 and its broadcast: so a machine keeps it across runs and engines.
 * Where two draw one, the id that sorts first keeps it and the other takes
 * the next free, so the result is the membership's and not the file's
 * order. .1 is hull's, kept for a resolver and a gateway.
 */

#ifndef HULL_NAMES_H
#define HULL_NAMES_H

#include <stddef.h>
#include <stdint.h>

#include "spec.h"

#ifdef __cplusplus
extern "C" {
#endif

#define HULL_NAMES_SEGMENTS 64u
#define HULL_NAMES_MEMBERS  64u

typedef struct hull_names_segment {
	char     name[64];
	uint32_t network;                       /* host order */
	unsigned prefix;
	unsigned count;
	unsigned member[HULL_NAMES_MEMBERS];    /* which of the file's machines */
	uint32_t address[HULL_NAMES_MEMBERS];   /* its address, host order */
} hull_names_segment;

typedef struct hull_names {
	hull_names_segment segment[HULL_NAMES_SEGMENTS];
	unsigned           count;
	char               ids[HULL_NAMES_MEMBERS][64];
	char               reason[256];         /* why hull_names_draw refused */
} hull_names;

/*
 * Draw the addresses of `count` machines, `machines[i]` with id `ids[i]`,
 * each a DNS label, which `names` keeps copies of. Returns 0, or -1 with
 * `names->reason` saying why: a segment given two subnets, two segments
 * whose subnets overlap, a segment addressed for one machine and not for
 * another, or more machines than a subnet holds.
 */
int hull_names_draw(hull_names *names, const hull_spec *spec,
                    const hull_sexpr_form *machines, const char *const *ids, unsigned count);

/*
 * The plan's `(names ...)` entry for machine `member`, canonical, into
 * `out`: `(names (SEGMENT (address A) (hosts (ID A) ...) (prefix N)) ...)`
 * for each addressed segment it is on, sorted. Its length; 0 where it is
 * on none, and -1 where `cap` is too small.
 */
long hull_names_entry(const hull_names *names, unsigned member, uint8_t *out, size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* HULL_NAMES_H */
