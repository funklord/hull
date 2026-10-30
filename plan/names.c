/*
 * names.c -- a file's machines given addresses on the segments hull
 * addresses, and each other's names (Appendix D).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>

#include "names.h"

/* The text of `item`'s entry `key` into `out`, "" where it has none: 1,
 * or 0 where it is there and unreadable. */
static int entry_text(const hull_spec *spec, const hull_sexpr_form *item, const char *key,
                      char *out, size_t cap)
{
	hull_sexpr_form entry;
	hull_sexpr_form head;
	hull_sexpr_form value;
	size_t          length;

	out[0] = '\0';
	if (!hull_spec_find(spec, item, key, &entry))
		return 1;
	if (!hull_sexpr_child(&spec->doc, &entry, &head) ||
	    !hull_sexpr_next(&spec->doc, &head, &value) ||
	    hull_sexpr_value(&spec->doc, &value, (uint8_t *)out, cap - 1u, &length) != HULL_SEXPR_OK)
		return 0;
	out[length] = '\0';
	return 1;
}

/* A (segments ...) element's name, a string or a list's head, and its
 * subnet's text into `subnet` where it has one, else "", and its IPv6
 * subnet's into `subnet6` likewise. 0 if unreadable. */
static int element(const hull_spec *spec, const hull_sexpr_form *item, char *name,
                   size_t name_cap, char *subnet, size_t subnet_cap, char *subnet6,
                   size_t subnet6_cap)
{
	hull_sexpr_form head = *item;
	size_t          length;

	subnet[0] = '\0';
	subnet6[0] = '\0';
	if (item->kind == HULL_SEXPR_LIST) {
		if (!hull_sexpr_child(&spec->doc, item, &head) ||
		    !entry_text(spec, item, "subnet", subnet, subnet_cap) ||
		    !entry_text(spec, item, "subnet6", subnet6, subnet6_cap) ||
		    (subnet[0] == '\0' && subnet6[0] == '\0'))
			return 0;
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

/* "address/n", which the spec has checked, into its network and prefix. */
static void parse_subnet6(const char *text, uint8_t network[16], unsigned *prefix)
{
	char  copy[64];
	char *slash;

	snprintf(copy, sizeof copy, "%s", text);
	slash = strchr(copy, '/');
	*slash = '\0';
	inet_pton(AF_INET6, copy, network);
	*prefix = (unsigned)strtoul(slash + 1, NULL, 10);
}

/* Whether `a` and `b` agree in their first `bits`. */
static int same_prefix(const uint8_t a[16], const uint8_t b[16], unsigned bits)
{
	for (unsigned bit = 0u; bit < bits; bit++)
		if ((a[bit / 8u] ^ b[bit / 8u]) & (0x80u >> (bit % 8u)))
			return 0;
	return 1;
}

/* `network` with `host` added into its last 64 bits, into `out`. */
static void host6(const uint8_t network[16], uint64_t host, uint8_t out[16])
{
	unsigned carry = 0u;

	memcpy(out, network, 16u);
	for (int i = 15; i >= 8; i--) {
		unsigned sum = out[i] + (unsigned)(host & 0xffu) + carry;

		out[i] = (uint8_t)sum;
		carry  = sum >> 8;
		host >>= 8;
	}
}

static uint64_t draw6(const char *id, const char *segment)
{
	uint64_t hash = 14695981039346656037u;

	for (const char *p = id; *p != '\0'; p++)
		hash = (hash ^ (unsigned char)*p) * 1099511628211u;
	hash = (hash ^ '/') * 1099511628211u;
	for (const char *p = segment; *p != '\0'; p++)
		hash = (hash ^ (unsigned char)*p) * 1099511628211u;
	return hash;
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
			char                subnet6[64];
			char                advertise[8];
			uint32_t            network = 0u;
			unsigned            prefix = 0u;
			uint8_t             network6[16];
			unsigned            prefix6 = 0u;
			hull_names_segment *s;

			if (!element(spec, &item, name, sizeof name, subnet, sizeof subnet, subnet6,
			             sizeof subnet6))
				return refuse(names, "a segment of %s cannot be read%s", ids[i], "");
			if (subnet[0] == '\0' && subnet6[0] == '\0')
				continue;
			if (subnet[0] != '\0')
				parse_subnet(subnet, &network, &prefix);
			if (subnet6[0] != '\0')
				parse_subnet6(subnet6, network6, &prefix6);
			if (!entry_text(spec, &item, "advertise", advertise, sizeof advertise))
				return refuse(names, "a segment of %s cannot be read%s", ids[i], "");
			s = find(names, name);
			/* Advertised where any machine asks it: one advertiser on a
			 * segment, hull at ::1, whoever names it. */
			if (s != NULL && strcmp(advertise, "true") == 0)
				s->advertise = 1;
			/* Each family's network named by any machine is the segment's;
			 * two that differ are refused. */
			if (s != NULL && subnet[0] != '\0' && s->has4 &&
			    (s->network != network || s->prefix != prefix))
				return refuse(names, "segment %s is given two subnets, one by %s", name, ids[i]);
			if (subnet[0] != '\0' && (s == NULL || !s->has4))
				for (unsigned j = 0u; j < names->count; j++) {
					const hull_names_segment *o = &names->segment[j];
					uint32_t                  mask = mask_of(prefix < o->prefix ? prefix : o->prefix);

					if (o != s && o->has4 && (network & mask) == (o->network & mask))
						return refuse(names, "segments %s and %s have subnets that overlap",
						              o->name, name);
				}
			/* An IPv6 network named by any machine is the segment's; two
			 * that differ are refused as two IPv4 ones are. */
			if (s != NULL && subnet6[0] != '\0' && s->has6 &&
			    (s->prefix6 != prefix6 || memcmp(s->network6, network6, 16u) != 0))
				return refuse(names, "segment %s is given two IPv6 subnets, one by %s", name,
				              ids[i]);
			if (subnet6[0] != '\0' && (s == NULL || !s->has6))
				for (unsigned j = 0u; j < names->count; j++) {
					const hull_names_segment *o = &names->segment[j];

					if (o != s && o->has6 &&
					    same_prefix(o->network6, network6,
					                prefix6 < o->prefix6 ? prefix6 : o->prefix6))
						return refuse(names, "segments %s and %s have IPv6 subnets that "
						              "overlap", o->name, name);
				}
			if (s != NULL) {
				if (subnet[0] != '\0' && !s->has4) {
					s->has4    = 1;
					s->network = network;
					s->prefix  = prefix;
				}
				if (subnet6[0] != '\0' && !s->has6) {
					s->has6 = 1;
					memcpy(s->network6, network6, 16u);
					s->prefix6 = prefix6;
				}
				continue;
			}
			if (names->count == HULL_NAMES_SEGMENTS)
				return refuse(names, "more addressed segments than %s%s", "hull draws", "");
			s = &names->segment[names->count++];
			snprintf(s->name, sizeof s->name, "%s", name);
			s->advertise = strcmp(advertise, "true") == 0;
			s->has4    = subnet[0] != '\0';
			s->network = network;
			s->prefix  = prefix;
			if (subnet6[0] != '\0') {
				s->has6 = 1;
				memcpy(s->network6, network6, 16u);
				s->prefix6 = prefix6;
			}
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
			char                subnet6[64];
			hull_names_segment *s;
			uint32_t            usable;
			uint32_t            slot;

			element(spec, &item, name, sizeof name, subnet, sizeof subnet, subnet6,
			        sizeof subnet6);
			if ((s = find(names, name)) == NULL)
				continue;
			if (subnet[0] == '\0' && subnet6[0] == '\0')
				return refuse(names, "segment %s is addressed, and %s names it without its "
				              "subnet: give the subnet in both", name, ids[at]);
			if (s->count == HULL_NAMES_MEMBERS)
				return refuse(names, "segment %s holds no address for %s", name, ids[at]);
			s->member[s->count]  = at;
			s->address[s->count] = 0u;
			if (s->has4) {
				usable = (1u << (32u - s->prefix)) - 3u;
				if (s->count == usable)
					return refuse(names, "segment %s holds no address for %s", name, ids[at]);
				slot = draw(ids[at], name) % usable;
				for (int taken = 1; taken;) {
					taken = 0;
					for (unsigned k = 0u; k < s->count && !taken; k++)
						taken = s->address[k] == s->network + 2u + slot;
					if (taken)
						slot = (slot + 1u) % usable;
				}
				s->address[s->count] = s->network + 2u + slot;
			}
			/* Its IPv6 address the same way, past ::0 and hull's ::1, in
			 * the host part's last 64 bits at most. */
			if (s->has6) {
				unsigned bits    = 128u - s->prefix6;
				uint64_t usable6 = bits >= 64u ? UINT64_MAX - 1u : (UINT64_C(1) << bits) - 2u;
				uint64_t slot6   = draw6(ids[at], name) % usable6;

				for (int taken = 1; taken;) {
					uint8_t candidate[16];

					taken = 0;
					host6(s->network6, slot6 + 2u, candidate);
					for (unsigned k = 0u; k < s->count && !taken; k++)
						taken = memcmp(s->address6[k], candidate, 16u) == 0;
					if (taken)
						slot6 = (slot6 + 1u) % usable6;
					else
						memcpy(s->address6[s->count], candidate, 16u);
				}
			}
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
				if (s->has4) {
					dotted(s->address[k], text);
					put(&o, "(7:address", 10u);
					put_atom(&o, text);
					put(&o, ")", 1u);
				}
				if (s->has6) {
					char text6[INET6_ADDRSTRLEN];

					inet_ntop(AF_INET6, s->address6[k], text6, sizeof text6);
					put(&o, "(8:address6", 11u);
					put_atom(&o, text6);
					put(&o, ")", 1u);
				}
			}
		if (s->advertise)
			put(&o, "(9:advertise4:true)", 19u);
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
		if (s->has4) {
			put(&o, "(5:hosts", 8u);
			for (unsigned k = 0u; k < s->count; k++) {
				put(&o, "(", 1u);
				put_atom(&o, names->ids[s->member[order[k]]]);
				dotted(s->address[order[k]], text);
				put_atom(&o, text);
				put(&o, ")", 1u);
			}
			put(&o, ")", 1u);
		}
		if (s->has6) {
			put(&o, "(6:hosts6", 9u);
			for (unsigned k = 0u; k < s->count; k++) {
				char text6[INET6_ADDRSTRLEN];

				put(&o, "(", 1u);
				put_atom(&o, names->ids[s->member[order[k]]]);
				inet_ntop(AF_INET6, s->address6[order[k]], text6, sizeof text6);
				put_atom(&o, text6);
				put(&o, ")", 1u);
			}
			put(&o, ")", 1u);
		}
		if (s->has4) {
			snprintf(prefix, sizeof prefix, "%u", s->prefix);
			put(&o, "(6:prefix", 9u);
			put_atom(&o, prefix);
			put(&o, ")", 1u);
		}
		if (s->has6) {
			snprintf(prefix, sizeof prefix, "%u", s->prefix6);
			put(&o, "(7:prefix6", 10u);
			put_atom(&o, prefix);
			put(&o, ")", 1u);
		}
		put(&o, ")", 1u);
	}
	put(&o, ")", 1u);
	return o.over ? -1 : (long)o.at;
}
