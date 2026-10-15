/*
 * plan.c -- the resolved machine, written and then judged.
 *
 * Writing it is copying: each entry the user wrote is copied whole, and
 * the planner adds only what was left out -- a default inside a record
 * that was written, or a whole entry that was not. Three records are
 * opened up to add to them: `security`, each `port` and `volume`, and the
 * `tuning` sections that apply. Everything else is copied as it stands,
 * including tuning for other engines, which the matrix reports ignored.
 *
 * `(projection auto)` is not written as such: section 3.9's rule 3 has
 * the plan choose, so each projection is tried in turn and the first that
 * leaves nothing rejected is kept.
 */

#include <string.h>

#include "plan.h"

/* An entry the planner writes where the user wrote none. */
typedef struct fallback {
	const char *key;
	const char *entry;
} fallback;

/* J.2 */
static const fallback security_defaults[] = {
	{ "readonly_root", "(readonly_root false)" },
	{ "no_new_privs",  "(no_new_privs true)" },
};

/* Section 4.3 settled these: the Appendix D example writes no proto. */
static const fallback port_defaults[]   = { { "proto", "(proto tcp)" } };
static const fallback volume_defaults[] = { { "mode", "(mode rw)" } };

/* J.5, by the section that holds them. */
static const fallback vm_defaults[] = {
	{ "profile",     "(profile micro)" },
	{ "kernel",      "(kernel bundled)" },
	{ "cpu_profile", "(cpu_profile host)" },
};
static const fallback emu_defaults[]        = { { "cores_scale", "(cores_scale 1)" } };
static const fallback ns_defaults[]         = { { "binfmt", "(binfmt false)" } };
static const fallback qemu_defaults[]       = { { "accel", "(accel auto)" } };
static const fallback virtualbox_defaults[] = { { "gui", "(gui false)" } };

typedef struct section {
	const char     *name;
	const fallback *defaults;
	unsigned        count;
} section;

#define SECTION(name, list) { name, list, (unsigned)(sizeof list / sizeof list[0]) }

static const section sections[] = {
	SECTION("vm", vm_defaults),
	SECTION("emu", emu_defaults),
	SECTION("ns", ns_defaults),
	SECTION("qemu", qemu_defaults),
	SECTION("virtualbox", virtualbox_defaults),
};

#define COUNT(list) ((unsigned)(sizeof list / sizeof list[0]))

/* Tried in this order when the user leaves the choice to the plan. */
static const char *const projections[] = { "bind", "passthrough", "block" };

typedef struct writer {
	hull_plan             *plan;
	const hull_spec       *spec;
	const hull_sexpr_form *machine;
	const char            *name;
	const hull_choice     *choice;
	const char            *host_arch;
	int                    overflow;
} writer;

/* --- the text and its map ------------------------------------------------ */

static void add(writer *w, const char *bytes, uint32_t length, uint32_t source, int copied)
{
	hull_plan         *plan = w->plan;
	hull_plan_segment *segment;

	if (length == 0u || w->overflow)
		return;
	if (plan->length + length >= sizeof plan->text ||
	    plan->segment_count == HULL_PLAN_SEGMENTS) {
		w->overflow = 1;
		return;
	}
	segment = &plan->segments[plan->segment_count++];
	segment->at     = (uint32_t)plan->length;
	segment->length = length;
	segment->source = source;
	segment->copied = copied;
	memcpy(plan->text + plan->length, bytes, length);
	plan->length += length;
}

/* Text the planner adds, attributed to `owner` in the user's file. */
static void put(writer *w, const char *text, uint32_t owner)
{
	add(w, text, (uint32_t)strlen(text), owner, 0);
}

/* A form the user wrote, as they wrote it. */
static void copy(writer *w, const hull_sexpr_form *form)
{
	add(w, (const char *)w->spec->doc.data + form->offset, form->extent, form->offset, 1);
}

uint32_t hull_plan_source(const hull_plan *plan, uint32_t offset)
{
	for (uint32_t i = 0u; i < plan->segment_count; i++) {
		const hull_plan_segment *s = &plan->segments[i];

		if (offset >= s->at && offset - s->at < s->length)
			return s->copied ? s->source + (offset - s->at) : s->source;
	}
	return 0u;
}

/* --- records ------------------------------------------------------------- */

static int has(const writer *w, const hull_sexpr_form *record, const char *key)
{
	hull_sexpr_form entry;

	return hull_spec_find(w->spec, record, key, &entry);
}

/* ` (head entry ... default ...)`: the record as written, then whatever
 * of `defaults` it left out. */
static void put_record(writer *w, const hull_sexpr_form *record, const fallback *defaults,
                       unsigned count)
{
	hull_sexpr_form item;
	int             more;

	put(w, " (", record->offset);
	hull_sexpr_child(&w->spec->doc, record, &item);
	copy(w, &item);
	for (more = hull_sexpr_next(&w->spec->doc, &item, &item); more;
	     more = hull_sexpr_next(&w->spec->doc, &item, &item)) {
		put(w, " ", item.offset);
		copy(w, &item);
	}
	for (unsigned i = 0u; i < count; i++) {
		if (!has(w, record, defaults[i].key)) {
			put(w, " ", record->offset);
			put(w, defaults[i].entry, record->offset);
		}
	}
	put(w, ")", record->offset);
}

/* ` (ports (port ...) ...)`, each item given its defaults. */
static void put_items(writer *w, const hull_sexpr_form *entry, const fallback *defaults,
                      unsigned count)
{
	hull_sexpr_form item;
	int             more;

	put(w, " (", entry->offset);
	hull_sexpr_child(&w->spec->doc, entry, &item);
	copy(w, &item);
	for (more = hull_sexpr_next(&w->spec->doc, &item, &item); more;
	     more = hull_sexpr_next(&w->spec->doc, &item, &item))
		put_record(w, &item, defaults, count);
	put(w, ")", entry->offset);
}

/* The tuning sections that apply: the class's, the engine's, and vm's
 * under emu, which adds to vm rather than replacing it (sections 4, 5.3). */
static int applies(const writer *w, const char *name)
{
	return strcmp(name, w->choice->class_name) == 0 ||
	       strcmp(name, w->choice->engine->caps->engine_name) == 0 ||
	       (strcmp(name, "vm") == 0 && strcmp(w->choice->class_name, "emu") == 0);
}

/* `(tuning ...)` with every section that applies, written or not. `entry`
 * is NULL when the user wrote no tuning at all. */
static void put_tuning(writer *w, const hull_sexpr_form *entry)
{
	uint32_t        owner = entry != NULL ? entry->offset : w->machine->offset;
	hull_sexpr_form item;
	int             any   = 0;
	int             more;

	for (unsigned i = 0u; i < COUNT(sections); i++)
		any |= applies(w, sections[i].name);
	if (entry == NULL && !any)
		return;
	put(w, " (tuning", owner);
	if (entry != NULL) {
		hull_sexpr_child(&w->spec->doc, entry, &item);
		for (more = hull_sexpr_next(&w->spec->doc, &item, &item); more;
		     more = hull_sexpr_next(&w->spec->doc, &item, &item)) {
			hull_sexpr_form name;
			unsigned        i;

			hull_sexpr_child(&w->spec->doc, &item, &name);
			for (i = 0u; i < COUNT(sections); i++)
				if (hull_sexpr_is(&w->spec->doc, &name, sections[i].name))
					break;
			if (i < COUNT(sections) && applies(w, sections[i].name)) {
				put_record(w, &item, sections[i].defaults, sections[i].count);
			} else {
				put(w, " ", item.offset);
				copy(w, &item);
			}
		}
	}
	for (unsigned i = 0u; i < COUNT(sections); i++) {
		if (!applies(w, sections[i].name) ||
		    (entry != NULL && has(w, entry, sections[i].name)))
			continue;
		put(w, " (", owner);
		put(w, sections[i].name, owner);
		for (unsigned d = 0u; d < sections[i].count; d++) {
			put(w, " ", owner);
			put(w, sections[i].defaults[d].entry, owner);
		}
		put(w, ")", owner);
	}
	put(w, ")", owner);
}

/* --- the machine --------------------------------------------------------- */

/* Whether the source is a dir, by its scheme. */
static int dir_source(const writer *w, const hull_sexpr_form *source_entry)
{
	hull_sexpr_form head;
	hull_sexpr_form value;

	hull_sexpr_child(&w->spec->doc, source_entry, &head);
	hull_sexpr_next(&w->spec->doc, &head, &value);
	return value.content_length > 4u &&
	       memcmp(w->spec->doc.data + value.content, "dir:", 4u) == 0;
}

/* A bare image ref: a store manifest supplies its command and workdir. */
static int ref_source(const writer *w, const hull_sexpr_form *source_entry)
{
	static const char *const schemes[] = { "dir:", "tar:", "pack:", "image:" };
	hull_sexpr_form          head;
	hull_sexpr_form          value;

	hull_sexpr_child(&w->spec->doc, source_entry, &head);
	hull_sexpr_next(&w->spec->doc, &head, &value);
	for (unsigned i = 0u; i < COUNT(schemes); i++) {
		size_t n = strlen(schemes[i]);

		if (value.content_length > n &&
		    memcmp(w->spec->doc.data + value.content, schemes[i], n) == 0)
			return 0;
	}
	return 1;
}

/* An entry the user left out, attributed to `owner`. */
static void put_default(writer *w, const char *key, const char *entry, uint32_t owner)
{
	if (has(w, w->machine, key))
		return;
	put(w, " ", owner);
	put(w, entry, owner);
}

static void write_machine(writer *w, const char *projection)
{
	const hull_sexpr_form *machine = w->machine;
	hull_sexpr_form        entry;
	hull_sexpr_form        source;
	uint32_t               projection_at = machine->offset;
	int                    more;

	w->plan->length        = 0u;
	w->plan->segment_count = 0u;
	w->overflow            = 0;
	hull_spec_find(w->spec, machine, "source", &source);

	put(w, "(machine", machine->offset);
	hull_sexpr_child(&w->spec->doc, machine, &entry);
	for (more = hull_sexpr_next(&w->spec->doc, &entry, &entry); more;
	     more = hull_sexpr_next(&w->spec->doc, &entry, &entry)) {
		hull_sexpr_form key;

		hull_sexpr_child(&w->spec->doc, &entry, &key);
		if (hull_sexpr_is(&w->spec->doc, &key, "projection")) {
			projection_at = entry.offset;
		} else if (hull_sexpr_is(&w->spec->doc, &key, "security")) {
			put_record(w, &entry, security_defaults, COUNT(security_defaults));
		} else if (hull_sexpr_is(&w->spec->doc, &key, "ports")) {
			put_items(w, &entry, port_defaults, COUNT(port_defaults));
		} else if (hull_sexpr_is(&w->spec->doc, &key, "volumes")) {
			put_items(w, &entry, volume_defaults, COUNT(volume_defaults));
		} else if (hull_sexpr_is(&w->spec->doc, &key, "tuning")) {
			put_tuning(w, &entry);
		} else {
			put(w, " ", entry.offset);
			copy(w, &entry);
		}
	}

	/* J.1's defaults, each where the user would look for it. */
	put_default(w, "mutable", dir_source(w, &source) ? "(mutable direct)" : "(mutable overlay)",
	            source.offset);
	put_default(w, "writeback", "(writeback none)", machine->offset);
	put_default(w, "ssh", "(ssh false)", machine->offset);
	put_default(w, "on_disconnect", "(on_disconnect stop)", machine->offset);
	if (!ref_source(w, &source))
		put_default(w, "workdir", "(workdir \"/\")", machine->offset);
	if (w->name != NULL && !has(w, machine, "hostname")) {
		put(w, " (hostname \"", machine->offset);
		put(w, w->name, machine->offset);
		put(w, "\")", machine->offset);
	}
	if (!has(w, machine, "security")) {
		put(w, " (security", machine->offset);
		for (unsigned i = 0u; i < COUNT(security_defaults); i++) {
			put(w, " ", machine->offset);
			put(w, security_defaults[i].entry, machine->offset);
		}
		put(w, ")", machine->offset);
	}
	if (!has(w, machine, "tuning"))
		put_tuning(w, NULL);
	put(w, " (projection ", projection_at);
	put(w, projection, projection_at);
	put(w, ")", projection_at);
	put(w, ")\n", machine->offset);
}

/* --- judging it ---------------------------------------------------------- */

typedef struct relay {
	const hull_plan    *plan;
	hull_matrix_report  report;
	void               *context;
	unsigned            rejected;
} relay;

static void forward(void *context, const hull_resolution *resolution)
{
	relay           *r    = context;
	hull_resolution  moved = *resolution;

	if (resolution->outcome == HULL_OUTCOME_REJECTED)
		r->rejected++;
	if (r->report == NULL)
		return;
	moved.offset = hull_plan_source(r->plan, resolution->offset);
	r->report(r->context, &moved);
}

/* The value of `(key value)` in the resolved machine. */
static int resolved_is(const hull_plan *plan, const char *key, const char *value)
{
	hull_sexpr_form entry;
	hull_sexpr_form head;
	hull_sexpr_form form;

	return hull_spec_find(&plan->resolved, &plan->resolved.machine, key, &entry) &&
	       hull_sexpr_child(&plan->resolved.doc, &entry, &head) &&
	       hull_sexpr_next(&plan->resolved.doc, &head, &form) &&
	       hull_sexpr_is(&plan->resolved.doc, &form, value);
}

/* Section 3.9's rule 2: a block projection writes into an overlay, so a
 * source meant to be written in place goes stale unless it is synced. */
static void check_writeback(const hull_plan *plan, relay *r)
{
	hull_resolution resolution;
	hull_sexpr_form entry;

	if (!resolved_is(plan, "projection", "block") || !resolved_is(plan, "mutable", "direct") ||
	    resolved_is(plan, "writeback", "sync-on-stop"))
		return;
	hull_spec_find(&plan->resolved, &plan->resolved.machine, "mutable", &entry);
	resolution.id      = "writeback";
	resolution.outcome = HULL_OUTCOME_REJECTED;
	resolution.note    = "(mutable direct) under a block projection needs "
	                     "(writeback sync-on-stop), or the directory goes stale";
	resolution.token   = NULL;
	resolution.offset  = entry.offset;
	forward(r, &resolution);
}

/* Write the machine with `projection`, open it, and resolve it. */
static hull_plan_status judge(writer *w, const char *projection, const char *const *accepted,
                              relay *r)
{
	hull_spec_error    error;
	hull_matrix_status status;

	write_machine(w, projection);
	if (w->overflow)
		return HULL_PLAN_ERR_SIZE;
	w->plan->projection = projection;
	if (hull_spec_open(&w->plan->resolved, (const uint8_t *)w->plan->text, w->plan->length,
	                   &error) != HULL_SPEC_OK) {
		w->plan->reason = error.reason;
		return HULL_PLAN_ERR_INTERNAL;
	}
	r->rejected = 0u;
	status = hull_matrix_resolve(&w->plan->resolved, &w->plan->resolved.machine,
	                             w->choice->engine->caps, w->choice->class_name,
	                             w->host_arch, accepted, forward, r);
	if (status == HULL_MATRIX_ERR_CLASS)
		return HULL_PLAN_ERR_CLASS;
	check_writeback(w->plan, r);
	return r->rejected == 0u ? HULL_PLAN_OK : HULL_PLAN_REJECTED;
}

/*
 * `(plan (class vm) (machine ...))`, canonical (A.1): the class entry and
 * the machine built in one part of `work`, then sorted behind the plan's
 * key in another, and closed into `canonical`. The machine's scratch is
 * the first two thirds of `work`, free again once it is written.
 */
static hull_plan_status encode(hull_plan *plan, const char *class_name, const char *engine_name,
                               const char *base)
{
	uint8_t  *items   = plan->work;
	uint8_t  *entries = plan->work + 2u * HULL_PLAN_TEXT;
	uint8_t   pair[4200];
	uint32_t  n_pair  = 0u;
	uint32_t  n_items = 0u;
	uint32_t  n       = 0u;

	plan->canonical_length = 0u;
	if (base != NULL &&
	    (hull_sexpr_put_atom(pair, sizeof pair, &n_pair, (const uint8_t *)"base", 4u) ||
	     hull_sexpr_put_atom(pair, sizeof pair, &n_pair, (const uint8_t *)base,
	                         (uint32_t)strlen(base)) ||
	     hull_sexpr_put_list(entries, HULL_PLAN_TEXT, &n, pair, n_pair)))
		return HULL_PLAN_ERR_SIZE;
	n_pair = 0u;
	if (hull_sexpr_put_atom(pair, sizeof pair, &n_pair, (const uint8_t *)"class", 5u) ||
	    hull_sexpr_put_atom(pair, sizeof pair, &n_pair, (const uint8_t *)class_name,
	                        (uint32_t)strlen(class_name)) ||
	    hull_sexpr_put_list(entries, HULL_PLAN_TEXT, &n, pair, n_pair))
		return HULL_PLAN_ERR_SIZE;
	n_pair = 0u;
	if (hull_sexpr_put_atom(pair, sizeof pair, &n_pair, (const uint8_t *)"engine", 6u) ||
	    hull_sexpr_put_atom(pair, sizeof pair, &n_pair, (const uint8_t *)engine_name,
	                        (uint32_t)strlen(engine_name)) ||
	    hull_sexpr_put_list(entries, HULL_PLAN_TEXT, &n, pair, n_pair) ||
	    hull_sexpr_to_canonical(&plan->resolved.doc, &plan->resolved.machine, entries,
	                            HULL_PLAN_TEXT, &n, plan->work, 2u * HULL_PLAN_TEXT) ||
	    hull_sexpr_put_atom(items, 2u * HULL_PLAN_TEXT, &n_items, (const uint8_t *)"plan", 4u) ||
	    hull_sexpr_put_sorted(items, 2u * HULL_PLAN_TEXT, &n_items, entries, n) ||
	    hull_sexpr_put_list(plan->canonical, sizeof plan->canonical, &plan->canonical_length,
	                        items, n_items))
		return HULL_PLAN_ERR_SIZE;
	return HULL_PLAN_OK;
}

hull_plan_status hull_plan_build(hull_plan *plan, const hull_spec *spec,
                                 const hull_sexpr_form *machine, const char *name,
                                 const char *base, const hull_choice *choice,
                                 const char *host_arch, const char *const *accepted,
                                 hull_matrix_report report, void *context)
{
	writer           w;
	relay            r;
	hull_sexpr_form  written;
	const char      *projection = NULL;
	hull_plan_status status;

	memset(&w, 0, sizeof w);
	w.plan    = plan;
	w.spec    = spec;
	w.machine = machine;
	w.name    = name;
	w.choice  = choice;
	w.host_arch = host_arch;
	plan->reason = NULL;
	r.plan    = plan;
	r.report  = NULL;
	r.context = NULL;

	/* A projection written by name is the user's; auto is the plan's. */
	if (hull_spec_find(spec, machine, "projection", &written)) {
		hull_sexpr_form head;
		hull_sexpr_form value;

		hull_sexpr_child(&spec->doc, &written, &head);
		hull_sexpr_next(&spec->doc, &head, &value);
		for (unsigned i = 0u; i < COUNT(projections); i++)
			if (hull_sexpr_is(&spec->doc, &value, projections[i]))
				projection = projections[i];
	}
	if (projection == NULL) {
		/* Silently, the first that leaves nothing rejected; else the
		 * first the engine declares, so the report names what is wrong. */
		for (unsigned i = 0u; i < COUNT(projections) && projection == NULL; i++) {
			status = judge(&w, projections[i], accepted, &r);
			if (status == HULL_PLAN_OK)
				projection = projections[i];
			else if (status != HULL_PLAN_REJECTED)
				return status;
		}
		for (unsigned i = 0u; i < COUNT(projections) && projection == NULL; i++) {
			char id[32] = "projection.";

			strcat(id, projections[i]);
			for (const hull_cap_entry *e = choice->engine->caps->entries; e->field != NULL; e++)
				if (strcmp(e->field, id) == 0 && projection == NULL)
					projection = projections[i];
		}
		if (projection == NULL)
			projection = projections[0];
	}
	r.report  = report;
	r.context = context;
	status = judge(&w, projection, accepted, &r);
	if (status != HULL_PLAN_OK && status != HULL_PLAN_REJECTED)
		return status;
	return encode(plan, choice->class_name, choice->engine->caps->engine_name, base) !=
	       HULL_PLAN_OK ? HULL_PLAN_ERR_SIZE : status;
}
