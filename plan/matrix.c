/*
 * matrix.c -- a machine's fields, named and looked up in one engine's
 * declaration.
 *
 * The names are derived from the spec as written (project.md section
 * 4.4): a key's path below the machine, carrying its value where the
 * value is chosen from a set, so that `(projection block)` asks for
 * `projection.block` and an engine can support one projection and refuse
 * another. spec_open has checked every value already, so nothing here
 * re-checks a shape: a form that is not what J says cannot reach it.
 */

#include <string.h>

#include "matrix.h"

#define ID_SIZE 96u

typedef struct resolver {
	const hull_spec         *spec;
	const hull_capabilities *caps;
	const char              *class_name;
	const char              *host_arch;
	const char *const       *accepted;
	hull_matrix_report       report;
	void                    *context;
	unsigned                 rejected;
} resolver;

/* --- names --------------------------------------------------------------- */

/* `prefix`, a dot, then `length` bytes of `name`; cut short if too long. */
static void join(char *id, const char *prefix, const uint8_t *name, uint32_t length)
{
	size_t n = 0u;

	if (prefix != NULL && prefix[0] != '\0') {
		n = strlen(prefix);
		if (n > ID_SIZE - 2u)
			n = ID_SIZE - 2u;
		memcpy(id, prefix, n);
		id[n++] = '.';
	}
	if (length > ID_SIZE - 1u - n)
		length = (uint32_t)(ID_SIZE - 1u - n);
	memcpy(id + n, name, length);
	id[n + length] = '\0';
}

static void join_form(const resolver *r, char *id, const char *prefix,
                      const hull_sexpr_form *form)
{
	join(id, prefix, r->spec->doc.data + form->content, form->content_length);
}

static void join_text(char *id, const char *prefix, const char *name)
{
	join(id, prefix, (const uint8_t *)name, (uint32_t)strlen(name));
}

static int listed(const char *const *list, const hull_sexpr_doc *doc,
                  const hull_sexpr_form *form)
{
	if (list == NULL)
		return 0;
	for (; *list != NULL; list++)
		if (hull_sexpr_is(doc, form, *list))
			return 1;
	return 0;
}

static int listed_text(const char *const *list, const char *name)
{
	if (list == NULL || name == NULL)
		return 0;
	for (; *list != NULL; list++)
		if (strcmp(*list, name) == 0)
			return 1;
	return 0;
}

/* --- reporting ----------------------------------------------------------- */

static void emit(resolver *r, const char *id, hull_outcome outcome, const char *note,
                 const char *token, uint32_t offset)
{
	hull_resolution resolution;

	resolution.id      = id;
	resolution.outcome = outcome;
	resolution.note    = note;
	resolution.token   = token;
	resolution.offset  = offset;
	if (outcome == HULL_OUTCOME_REJECTED)
		r->rejected++;
	if (r->report != NULL)
		r->report(r->context, &resolution);
}

/* Look `id` up in the engine's column. */
static void resolve(resolver *r, const char *id, uint32_t offset)
{
	const hull_cap_entry *entry = r->caps->entries;

	for (; entry != NULL && entry->field != NULL; entry++)
		if (strcmp(entry->field, id) == 0)
			break;
	if (entry == NULL || entry->field == NULL) {
		emit(r, id, HULL_OUTCOME_REJECTED, "the engine does not declare this field",
		     NULL, offset);
		return;
	}
	switch (entry->support) {
	case HULL_SUP_NATIVE:
		emit(r, id, HULL_OUTCOME_NATIVE, entry->note, NULL, offset);
		return;
	case HULL_SUP_EMULATED:
		emit(r, id, HULL_OUTCOME_EMULATED, entry->note, NULL, offset);
		return;
	case HULL_SUP_REJECTED:
		break;
	}
	if (entry->accept != NULL && listed_text(r->accepted, entry->accept))
		emit(r, id, HULL_OUTCOME_ACCEPTED, entry->note, entry->accept, offset);
	else
		emit(r, id, HULL_OUTCOME_REJECTED, entry->note, entry->accept, offset);
}

/* --- entries ------------------------------------------------------------- */

static int is_number(const resolver *r, const hull_sexpr_form *form)
{
	return form->kind == HULL_SEXPR_SYMBOL && form->content_length != 0u &&
	       r->spec->doc.data[form->content] >= '0' && r->spec->doc.data[form->content] <= '9';
}

/*
 * One `(key value ...)` below `prefix`, for the keys that need no more
 * than their shape: an empty list or `false` asks nothing; a symbol from a
 * set is carried in the id; anything else asks for the key itself.
 */
static void resolve_entry(resolver *r, const char *prefix, const hull_sexpr_form *entry)
{
	hull_sexpr_form head;
	hull_sexpr_form value;
	hull_sexpr_form extra;
	char            id[ID_SIZE];

	hull_sexpr_child(&r->spec->doc, entry, &head);
	join_form(r, id, prefix, &head);
	if (!hull_sexpr_next(&r->spec->doc, &head, &value))
		return;
	/* Symbols from an open set, which one alone does not make a choice:
	 * capability grants and device ids, the only such list below the
	 * machine that the call does not name itself. */
	if (hull_sexpr_is(&r->spec->doc, &head, "allow")) {
		resolve(r, id, entry->offset);
		return;
	}
	if (value.kind == HULL_SEXPR_SYMBOL && !hull_sexpr_next(&r->spec->doc, &value, &extra)) {
		if (hull_sexpr_is(&r->spec->doc, &value, "false"))
			return;
		if (!hull_sexpr_is(&r->spec->doc, &value, "true") && !is_number(r, &value)) {
			char qualified[ID_SIZE];

			join_form(r, qualified, id, &value);
			resolve(r, qualified, entry->offset);
			return;
		}
	}
	resolve(r, id, entry->offset);
}

/* Every entry of a record below `prefix`. */
static void resolve_record(resolver *r, const char *prefix, const hull_sexpr_form *record)
{
	hull_sexpr_form item;
	int             more;

	hull_sexpr_child(&r->spec->doc, record, &item);
	for (more = hull_sexpr_next(&r->spec->doc, &item, &item); more;
	     more = hull_sexpr_next(&r->spec->doc, &item, &item))
		resolve_entry(r, prefix, &item);
}

/* `(source "dir:./rootfs")`, with `(mutable ...)` or its default. */
static void resolve_source(resolver *r, const hull_sexpr_form *machine,
                           const hull_sexpr_form *entry)
{
	static const char *const schemes[] = { "dir", "tar", "pack", "image", NULL };
	hull_sexpr_form          head;
	hull_sexpr_form          value;
	hull_sexpr_form          mutable_entry;
	const uint8_t           *text;
	const char              *scheme = "ref";
	const char              *mutability;
	char                     id[ID_SIZE];
	char                     with_scheme[ID_SIZE];

	hull_sexpr_child(&r->spec->doc, entry, &head);
	hull_sexpr_next(&r->spec->doc, &head, &value);
	text = r->spec->doc.data + value.content;
	for (const char *const *s = schemes; *s != NULL; s++) {
		size_t n = strlen(*s);

		if (value.content_length > n && memcmp(text, *s, n) == 0 && text[n] == ':') {
			scheme = *s;
			break;
		}
	}
	/* J.1: direct for a dir source, overlay for anything else. */
	mutability = strcmp(scheme, "dir") == 0 ? "direct" : "overlay";
	join_text(with_scheme, "source", scheme);
	if (hull_spec_find(r->spec, machine, "mutable", &mutable_entry)) {
		hull_sexpr_form mutable_head;
		hull_sexpr_form mutable_value;

		hull_sexpr_child(&r->spec->doc, &mutable_entry, &mutable_head);
		hull_sexpr_next(&r->spec->doc, &mutable_head, &mutable_value);
		join_form(r, id, with_scheme, &mutable_value);
	} else {
		join_text(id, with_scheme, mutability);
	}
	resolve(r, id, entry->offset);
}

/*
 * `(arch arm64)`. The host's own asks nothing, by name or as `host`.
 * Another is a class's question before it is an engine's (section 6): ns
 * shares the host's kernel and vm is accelerated, so only emu runs a
 * foreign arch, and then only one the engine lists.
 */
static void resolve_arch(resolver *r, const hull_sexpr_form *entry)
{
	hull_sexpr_form head;
	hull_sexpr_form value;

	hull_sexpr_child(&r->spec->doc, entry, &head);
	hull_sexpr_next(&r->spec->doc, &head, &value);
	if (hull_sexpr_is(&r->spec->doc, &value, "host") ||
	    (r->host_arch != NULL && hull_sexpr_is(&r->spec->doc, &value, r->host_arch)))
		return;
	if (strcmp(r->class_name, "emu") != 0)
		emit(r, "arch", HULL_OUTCOME_REJECTED,
		     "only the emu class runs an arch other than the host's", NULL, entry->offset);
	else if (listed(r->caps->arches, &r->spec->doc, &value))
		emit(r, "arch", HULL_OUTCOME_NATIVE, NULL, NULL, entry->offset);
	else
		emit(r, "arch", HULL_OUTCOME_REJECTED, "the engine does not run this arch",
		     NULL, entry->offset);
}

/* `(fidelity inotify flock)`: each need is a field of its own. */
static void resolve_fidelity(resolver *r, const hull_sexpr_form *entry)
{
	hull_sexpr_form need;
	char            id[ID_SIZE];
	int             more;

	hull_sexpr_child(&r->spec->doc, entry, &need);
	for (more = hull_sexpr_next(&r->spec->doc, &need, &need); more;
	     more = hull_sexpr_next(&r->spec->doc, &need, &need)) {
		join_form(r, id, "fidelity", &need);
		resolve(r, id, need.offset);
	}
}

/* `(media (medium (type cdrom) ...))`: the key, then each type against the
 * engine's media_types (J.4). */
static void resolve_media(resolver *r, const hull_sexpr_form *entry)
{
	hull_sexpr_form medium;
	hull_sexpr_form type;
	int             more;

	hull_sexpr_child(&r->spec->doc, entry, &medium);
	if (!hull_sexpr_next(&r->spec->doc, &medium, &medium))
		return;
	resolve(r, "media", entry->offset);
	for (more = 1; more; more = hull_sexpr_next(&r->spec->doc, &medium, &medium)) {
		hull_sexpr_form head;
		hull_sexpr_form value;

		hull_spec_find(r->spec, &medium, "type", &type);
		hull_sexpr_child(&r->spec->doc, &type, &head);
		hull_sexpr_next(&r->spec->doc, &head, &value);
		if (!listed(r->caps->media_types, &r->spec->doc, &value))
			emit(r, "media", HULL_OUTCOME_REJECTED,
			     "the engine does not list this medium type", NULL, medium.offset);
	}
}

/* `(kernel "path:./bzImage")` is the one string a choice is made with. */
static void resolve_tuning_entry(resolver *r, const char *prefix, const hull_sexpr_form *entry)
{
	hull_sexpr_form head;
	hull_sexpr_form value;
	char            id[ID_SIZE];
	char            qualified[ID_SIZE];

	hull_sexpr_child(&r->spec->doc, entry, &head);
	hull_sexpr_next(&r->spec->doc, &head, &value);
	if (value.kind != HULL_SEXPR_STRING) {
		resolve_entry(r, prefix, entry);
		return;
	}
	join_form(r, id, prefix, &head);
	join_text(qualified, id, "path");
	resolve(r, qualified, entry->offset);
}

/*
 * `(tuning (vm ...) (qemu ...))`. An entry applies when it names the class
 * the engine runs the machine as, or the engine itself, and vm's applies
 * under emu too, which adds to vm's tuning rather than replacing it
 * (sections 4 and 5.3: "same profiles, same kernel strategy"). Any other
 * is ignored with a warning (section 4).
 *
 * J.5 also says an engine's entry overrides its class's, but no key of J
 * is under both, and spec_open refuses any other, so there is nothing yet
 * for an override to decide (section 4.4).
 */
static void resolve_tuning(resolver *r, const hull_sexpr_form *entry)
{
	hull_sexpr_form section;
	int             more;

	hull_sexpr_child(&r->spec->doc, entry, &section);
	for (more = hull_sexpr_next(&r->spec->doc, &section, &section); more;
	     more = hull_sexpr_next(&r->spec->doc, &section, &section)) {
		hull_sexpr_form name;
		hull_sexpr_form item;
		char            prefix[ID_SIZE];
		int             items;

		hull_sexpr_child(&r->spec->doc, &section, &name);
		join_form(r, prefix, NULL, &name);
		if (!hull_sexpr_is(&r->spec->doc, &name, r->caps->engine_name) &&
		    !hull_sexpr_is(&r->spec->doc, &name, r->class_name) &&
		    !(hull_sexpr_is(&r->spec->doc, &name, "vm") && strcmp(r->class_name, "emu") == 0)) {
			emit(r, prefix, HULL_OUTCOME_IGNORED,
			     "tuning for another class or engine", NULL, section.offset);
			continue;
		}
		hull_sexpr_child(&r->spec->doc, &section, &item);
		for (items = hull_sexpr_next(&r->spec->doc, &item, &item); items;
		     items = hull_sexpr_next(&r->spec->doc, &item, &item))
			resolve_tuning_entry(r, prefix, &item);
	}
}

/* --- the call ------------------------------------------------------------ */

hull_matrix_status hull_matrix_resolve(const hull_spec *spec, const hull_sexpr_form *machine,
                                       const hull_capabilities *caps, const char *class_name,
                                       const char *host_arch, const char *const *accepted,
                                       hull_matrix_report report, void *context)
{
	resolver        r;
	hull_sexpr_form entry;
	int             more;

	if (class_name == NULL || !listed_text(caps->classes, class_name))
		return HULL_MATRIX_ERR_CLASS;
	r.spec       = spec;
	r.caps       = caps;
	r.class_name = class_name;
	r.host_arch  = host_arch;
	r.accepted   = accepted;
	r.report     = report;
	r.context    = context;
	r.rejected   = 0u;

	hull_sexpr_child(&spec->doc, machine, &entry);
	for (more = hull_sexpr_next(&spec->doc, &entry, &entry); more;
	     more = hull_sexpr_next(&spec->doc, &entry, &entry)) {
		hull_sexpr_form key;
		hull_sexpr_form first;

		hull_sexpr_child(&spec->doc, &entry, &key);
		if (hull_sexpr_is(&spec->doc, &key, "source"))
			resolve_source(&r, machine, &entry);
		else if (hull_sexpr_is(&spec->doc, &key, "mutable"))
			continue;  /* carried by the source's id */
		else if (hull_sexpr_is(&spec->doc, &key, "arch"))
			resolve_arch(&r, &entry);
		else if (hull_sexpr_is(&spec->doc, &key, "fidelity"))
			resolve_fidelity(&r, &entry);
		else if (hull_sexpr_is(&spec->doc, &key, "media"))
			resolve_media(&r, &entry);
		else if (hull_sexpr_is(&spec->doc, &key, "tuning"))
			resolve_tuning(&r, &entry);
		else if (hull_sexpr_is(&spec->doc, &key, "security"))
			resolve_record(&r, "security", &entry);
		/* devices.usb, devices.serial and the rest: each kind an engine
		 * may pass or not, whatever the devices it names. */
		else if (hull_sexpr_is(&spec->doc, &key, "devices"))
			resolve_record(&r, "devices", &entry);
		else if (hull_sexpr_is(&spec->doc, &key, "backend") ||
		         hull_sexpr_is(&spec->doc, &key, "restart") ||
		         hull_sexpr_is(&spec->doc, &key, "env_file")) {
			char id[ID_SIZE];

			/* Hull's own: backend chose the engine, the systemd generator
			 * reads restart, and the core folds env_file into env. */
			join_form(&r, id, NULL, &key);
			emit(&r, id, HULL_OUTCOME_NATIVE, "hull's own, not the engine's", NULL,
			     entry.offset);
		} else if (hull_sexpr_next(&spec->doc, &key, &first) &&
		           first.kind == HULL_SEXPR_LIST) {
			char id[ID_SIZE];

			/* ports, volumes, env, secrets: the key, once. */
			join_form(&r, id, NULL, &key);
			resolve(&r, id, entry.offset);
			/* A volume that is a stack of layers (3.7) asks for more
			 * than a directory: once, at the first. */
			if (hull_sexpr_is(&spec->doc, &key, "volumes")) {
				hull_sexpr_form item = first;
				hull_sexpr_form layers;

				do {
					if (hull_spec_find(spec, &item, "layers", &layers)) {
						resolve(&r, "volumes.layers", layers.offset);
						break;
					}
				} while (hull_sexpr_next(&spec->doc, &item, &item));
			}
		} else {
			resolve_entry(&r, NULL, &entry);
		}
	}
	return r.rejected == 0u ? HULL_MATRIX_OK : HULL_MATRIX_REJECTED;
}
