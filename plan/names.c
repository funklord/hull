/*
 * names.c -- a file's machines given addresses on the segments hull
 * addresses, and each other's names (Appendix D).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "names.h"

/* A (segments ...) element's name, a string or a list's head, and its
 * subnet's text into `subnet` where it has one, else "". 0 if unreadable. */
static int element(const hull_spec *spec, const hull_sexpr_form *item, char *name,
                   size_t name_cap, char *subnet, size_t subnet_cap)
{
	hull_sexpr_form head = *item;
	size_t          length;

	subnet[0] = '\0';
	if (item->kind == HULL_SEXPR_LIST) {
		hull_sexpr_form entry;
		hull_sexpr_form key;
		hull_sexpr_form value;

		if (!hull_sexpr_child(&spec->doc, item, &head) ||
		    !hull_spec_find(spec, item, "subnet", &entry) ||
		    !hull_sexpr_child(&spec->doc, &entry, &key) ||
		    !hull_sexpr_next(&spec->doc, &key, &value) ||
		    hull_sexpr_value(&spec->doc, &value, (uint8_t *)subnet, subnet_cap - 1u,
		                     &length) != HULL_SEXPR_OK)
			return 0;
		subnet[length] = '\0';
	}
	if (hull_sexpr_value(&spec->doc, &head, (uint8_t *)name, name_cap - 1u, &length) !=
	    HULL_SEXPR_OK)
		return 0;
	name[length] = '\0';
	return 1;
}

/* "a.b.c.d/n", which the spec has checked, into its network and prefix. */
static void parse_subnet(const char *text, uint32_t *network, unsigned *prefix)
{
	unsigned a, b, c, d;

	sscanf(text, "%u.%u.%u.%u/%u", &a, &b, &c, &d, prefix);
	*network = (uint32_t)a << 24 | (uint32_t)b << 16 | (uint32_t)c << 8 | (uint32_t)d;
}

static uint32_t mask_of(unsigned prefix)
{
	return 0xffffffffu << (32u - prefix);
}

static uint32_t draw(const char *id, const char *segment)
{
	uint32_t hash = 2166136261u;

	for (const char *p = id; *p != '\0'; p++)
		hash = (hash ^ (unsigned char)*p) * 16777619u;
	hash = (hash ^ '/') * 16777619u;
	for (const char *p = segment; *p != '\0'; p++)
		hash = (hash ^ (unsigned char)*p) * 16777619u;
	return hash;
}

static hull_names_segment *find(hull_names *names, const char *name)
{
	for (unsigned i = 0u; i < names->count; i++)
		if (strcmp(names->segment[i].name, name) == 0)
			return &names->segment[i];
	return NULL;
}

static int refuse(hull_names *names, const char *format, const char *a, const char *b)
{
	snprintf(names->reason, sizeof names->reason, format, a, b);
	return -1;
}

int hull_names_draw(hull_names *names, const hull_spec *spec,
                    const hull_sexpr_form *machines, const char *const *ids, unsigned count)
{
	memset(names, 0, sizeof *names);
	if (count > HULL_NAMES_MEMBERS)
		return refuse(names, "more machines than %s%s", "a file's segments can address", "");
	for (unsigned i = 0u; i < count; i++)
		snprintf(names->ids[i], sizeof names->ids[i], "%s", ids[i]);

	/* Every addressed segment, and its subnet, held the same everywhere. */
	for (unsigned i = 0u; i < count; i++) {
		hull_sexpr_form entry;
		hull_sexpr_form item;

		if (!hull_spec_find(spec, &machines[i], "segments", &entry))
			continue;
		hull_sexpr_child(&spec->doc, &entry, &item);
		while (hull_sexpr_next(&spec->doc, &item, &item)) {
			char                name[64];
			char                subnet[32];
			uint32_t            network;
			unsigned            prefix;
			hull_names_segment *s;

			if (!element(spec, &item, name, sizeof name, subnet, sizeof subnet))
				return refuse(names, "a segment of %s cannot be read%s", ids[i], "");
			if (subnet[0] == '\0')
				continue;
			parse_subnet(subnet, &network, &prefix);
			s = find(names, name);
			if (s != NULL && (s->network != network || s->prefix != prefix))
				return refuse(names, "segment %s is given two subnets, one by %s", name, ids[i]);
			if (s != NULL)
				continue;
			for (unsigned j = 0u; j < names->count; j++) {
				const hull_names_segment *o = &names->segment[j];
				uint32_t                  mask = mask_of(prefix < o->prefix ? prefix : o->prefix);

				if ((network & mask) == (o->network & mask))
					return refuse(names, "segments %s and %s have subnets that overlap",
					              o->name, name);
			}
			if (names->count == HULL_NAMES_SEGMENTS)
				return refuse(names, "more addressed segments than %s%s", "hull draws", "");
			s = &names->segment[names->count++];
			snprintf(s->name, sizeof s->name, "%s", name);
			s->network = network;
			s->prefix  = prefix;
		}
	}
	if (names->count == 0u)
		return 0;

	/* Each machine on one, in the order of their ids, so that who keeps a
	 * drawn address on a clash is the membership's choice and not the
	 * file's; a machine naming an addressed segment bare is refused, as
	 * its siblings would hold a name for it with no address. */
	for (unsigned i = 0u; i < count; i++) {
		unsigned        at = i;
		hull_sexpr_form entry;
		hull_sexpr_form item;

		/* The ith machine by id, found by counting those before it. */
		for (unsigned j = 0u; j < count; j++) {
			unsigned before = 0u;

			for (unsigned k = 0u; k < count; k++)
				before += strcmp(ids[k], ids[j]) < 0 || (strcmp(ids[k], ids[j]) == 0 && k < j);
			if (before == i)
				at = j;
		}
		if (!hull_spec_find(spec, &machines[at], "segments", &entry))
			continue;
		hull_sexpr_child(&spec->doc, &entry, &item);
		while (hull_sexpr_next(&spec->doc, &item, &item)) {
			char                name[64];
			char                subnet[32];
			hull_names_segment *s;
			uint32_t            usable;
			uint32_t            slot;

			element(spec, &item, name, sizeof name, subnet, sizeof subnet);
			if ((s = find(names, name)) == NULL)
				continue;
			if (subnet[0] == '\0')
				return refuse(names, "segment %s is addressed, and %s names it without its "
				              "subnet: give the subnet in both", name, ids[at]);
			usable = (1u << (32u - s->prefix)) - 3u;
			if (s->count == usable || s->count == HULL_NAMES_MEMBERS)
				return refuse(names, "segment %s holds no address for %s", name, ids[at]);
			slot = draw(ids[at], name) % usable;
			for (int taken = 1; taken;) {
				taken = 0;
				for (unsigned k = 0u; k < s->count && !taken; k++)
					taken = s->address[k] == s->network + 2u + slot;
				if (taken)
					slot = (slot + 1u) % usable;
			}
			s->member[s->count]  = at;
			s->address[s->count] = s->network + 2u + slot;
			s->count++;
		}
	}
	return 0;
}

/* --- the plan's entry ---------------------------------------------------- */

typedef struct out {
	uint8_t *data;
	size_t   cap;
	size_t   at;
	int      over;
} out;

static void put(out *o, const char *bytes, size_t n)
{
	if (o->at + n > o->cap) {
		o->over = 1;
		return;
	}
	memcpy(o->data + o->at, bytes, n);
	o->at += n;
}

static void put_atom(out *o, const char *text)
{
	char length[24];

	snprintf(length, sizeof length, "%zu:", strlen(text));
	put(o, length, strlen(length));
	put(o, text, strlen(text));
}

static void dotted(uint32_t address, char text[16])
{
	snprintf(text, 16, "%u.%u.%u.%u", address >> 24, address >> 16 & 255u, address >> 8 & 255u,
	         address & 255u);
}

static int by_name(const void *a, const void *b)
{
	return strcmp((*(const hull_names_segment *const *)a)->name,
	              (*(const hull_names_segment *const *)b)->name);
}

long hull_names_entry(const hull_names *names, unsigned member, uint8_t *data, size_t cap)
{
	const hull_names_segment *on[HULL_NAMES_SEGMENTS];
	unsigned                  n = 0u;
	out                       o = { data, cap, 0u, 0 };

	for (unsigned i = 0u; i < names->count; i++)
		for (unsigned k = 0u; k < names->segment[i].count; k++)
			if (names->segment[i].member[k] == member)
				on[n++] = &names->segment[i];
	if (n == 0u)
		return 0;
	qsort(on, n, sizeof on[0], by_name);
	put(&o, "(5:names", 8u);
	for (unsigned i = 0u; i < n; i++) {
		const hull_names_segment *s = on[i];
		unsigned                  order[HULL_NAMES_MEMBERS];
		char                      text[16];
		char                      prefix[4];

		put(&o, "(", 1u);
		put_atom(&o, s->name);
		for (unsigned k = 0u; k < s->count; k++)
			if (s->member[k] == member) {
				dotted(s->address[k], text);
				put(&o, "(7:address", 10u);
				put_atom(&o, text);
				put(&o, ")", 1u);
			}
		/* The hosts, by id: insertion, as there are at most 256. */
		for (unsigned k = 0u; k < s->count; k++) {
			unsigned j = k;

			while (j > 0u && strcmp(names->ids[s->member[order[j - 1u]]],
			                        names->ids[s->member[k]]) > 0) {
				order[j] = order[j - 1u];
				j--;
			}
			order[j] = k;
		}
		put(&o, "(5:hosts", 8u);
		for (unsigned k = 0u; k < s->count; k++) {
			put(&o, "(", 1u);
			put_atom(&o, names->ids[s->member[order[k]]]);
			dotted(s->address[order[k]], text);
			put_atom(&o, text);
			put(&o, ")", 1u);
		}
		snprintf(prefix, sizeof prefix, "%u", s->prefix);
		put(&o, ")(6:prefix", 10u);
		put_atom(&o, prefix);
		put(&o, "))", 2u);
	}
	put(&o, ")", 1u);
	return o.over ? -1 : (long)o.at;
}
