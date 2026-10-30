/*
 * spec.c -- hull.conf held to Appendix J.
 *
 * The inventory is data. Each record is a table of keys, each key a value
 * type and whatever that type needs: a set of symbols, the record it
 * holds, a rule for a string. One walk applies every table, so a key added
 * to Appendix J is a row here rather than a function.
 *
 * The reader has already checked the syntax, so the walk only ever meets
 * well-formed lists, symbols and strings. A string's rule reads its
 * content as written, escapes and all; no rule here accepts a backslash,
 * so an escape fails a rule rather than slipping past one.
 */

#include <string.h>
#include <arpa/inet.h>

#include "spec.h"

/* The most keys one record has, which bounds the walk's bookkeeping. */
#define MAX_KEYS 64u  /* a schema's keys, each a bit of a uint64_t present */

typedef enum value_type {
	STRING,    /* one string */
	STRINGS,   /* any number of strings */
	SYMBOL,    /* one symbol from an open set: an arch, an engine */
	SYMBOLS,   /* any number of them */
	CHOICE,    /* one symbol from `choices` */
	BOOLEAN,   /* true or false */
	COUNT,     /* a whole number, from 1 */
	PORT,      /* a whole number, from 1 to 65535 */
	NUMBER,    /* a positive decimal: 2, 1.5 */
	KERNEL,    /* bundled, image, or "path:<file>" */
	RECORD,    /* (key (entry ...) ...), held to `record` */
	ENTRIES,   /* a plural key holding (singular ...) records */
	MAP,       /* names the user chooses: (name ...) records, or (name "value") */
	NAMES,     /* names the user chooses, each a string or a (name ...) record */
	RESERVED,  /* named by Appendix J, defined with its adapter */
} value_type;

/* A rule returns NULL for content it accepts, or why it refuses it. */
typedef const char *(*rule)(const uint8_t *text, uint32_t length);

struct schema;

typedef struct key {
	const char          *name;
	value_type           type;
	int                  required;
	int                  nonempty;  /* STRINGS, SYMBOLS, ENTRIES, MAP: at least one */
	const char *const   *choices;   /* CHOICE, NULL-terminated */
	const struct schema *record;    /* RECORD, ENTRIES; MAP when its names hold records */
	const char          *entry;     /* ENTRIES: the singular key */
	rule                 value;     /* strings, and symbols from an open set */
	rule                 names;     /* MAP: the names */
} key;

typedef struct walk walk;

typedef struct schema {
	const key  *keys;
	unsigned    count;
	/* Rules across keys, run once every key has passed on its own. */
	hull_spec_status (*across)(walk *w, const hull_sexpr_form *record,
	                           const hull_sexpr_form *found, uint64_t present);
} schema;

struct walk {
	const hull_sexpr_doc *doc;
	hull_spec_error      *error;
	char                  path[sizeof ((hull_spec_error *)0)->path];
	size_t                path_length;
};

/* --- the walk's own state ------------------------------------------------ */

static hull_spec_status fail(walk *w, hull_spec_status status, uint32_t offset,
                             const char *reason)
{
	if (w->error == NULL)
		return status;
	hull_sexpr_locate(w->doc, offset, &w->error->line, &w->error->column);
	w->error->status = status;
	w->error->offset = offset;
	w->error->reason = reason;
	memcpy(w->error->path, w->path, w->path_length + 1u);
	return status;
}

/* Add a name to the path, and return what to cut it back to. */
static size_t push(walk *w, const uint8_t *name, uint32_t length)
{
	size_t saved = w->path_length;
	size_t room  = sizeof w->path - 1u - w->path_length;

	if (w->path_length != 0u && room != 0u) {
		w->path[w->path_length++] = '.';
		room--;
	}
	if (length > room)
		length = (uint32_t)room;
	memcpy(w->path + w->path_length, name, length);
	w->path_length += length;
	w->path[w->path_length] = '\0';
	return saved;
}

static void pop(walk *w, size_t saved)
{
	w->path_length = saved;
	w->path[saved] = '\0';
}

static size_t push_form(walk *w, const hull_sexpr_form *form)
{
	return push(w, w->doc->data + form->content, form->content_length);
}

static size_t push_text(walk *w, const char *name)
{
	return push(w, (const uint8_t *)name, (uint32_t)strlen(name));
}

static int same_name(const hull_sexpr_doc *doc, const hull_sexpr_form *a,
                     const hull_sexpr_form *b)
{
	return a->content_length == b->content_length &&
	       memcmp(doc->data + a->content, doc->data + b->content, a->content_length) == 0;
}

/* --- rules --------------------------------------------------------------- */

static int is_digit(uint8_t byte)
{
	return byte >= '0' && byte <= '9';
}

static int is_lower(uint8_t byte)
{
	return byte >= 'a' && byte <= 'z';
}

static int is_upper(uint8_t byte)
{
	return byte >= 'A' && byte <= 'Z';
}

static const char *nonempty(const uint8_t *text, uint32_t length)
{
	(void)text;
	return length == 0u ? "may not be empty" : NULL;
}

static const char *absolute(const uint8_t *text, uint32_t length)
{
	return length != 0u && text[0] == '/' ? NULL : "must be an absolute path";
}

/* A USB vendor or product id: four hex digits, as lsusb prints them. */
static const char *usb_id(const uint8_t *text, uint32_t length)
{
	if (length != 4u)
		return "must be four hex digits, as lsusb prints them: \"0403\"";
	for (uint32_t i = 0u; i < length; i++)
		if (!is_digit(text[i]) && !(text[i] >= 'a' && text[i] <= 'f') &&
		    !(text[i] >= 'A' && text[i] <= 'F'))
			return "must be four hex digits, as lsusb prints them: \"0403\"";
	return NULL;
}

/* A USB port path, as sysfs names a device: a bus, then the ports from
 * its root hub, "1-4.2". */
static const char *usb_port(const uint8_t *text, uint32_t length)
{
	uint32_t i = 0u;
	int      ports = 0;

	while (i < length && is_digit(text[i]))
		i++;
	if (i == 0u || i == length || text[i] != '-')
		return "must be a bus and its ports, as sysfs names them: \"1-4.2\"";
	while (i < length && (text[i] == '-' || text[i] == '.')) {
		uint32_t start = ++i;

		while (i < length && is_digit(text[i]))
			i++;
		if (i == start)
			return "must be a bus and its ports, as sysfs names them: \"1-4.2\"";
		ports++;
	}
	return i == length && ports != 0 ? NULL :
	       "must be a bus and its ports, as sysfs names them: \"1-4.2\"";
}

/* A PCI address as sysfs names a device, "0000:01:00.0": domain, bus,
 * device and function, lowercase hex. */
static const char *pci_address(const uint8_t *text, uint32_t length)
{
	static const char shape[] = "hhhh:hh:hh.h";

	if (length != sizeof shape - 1u)
		return "must be a PCI address as sysfs names it: \"0000:01:00.0\"";
	for (uint32_t i = 0u; i < length; i++)
		if (shape[i] == 'h' ? !(is_digit(text[i]) || (text[i] >= 'a' && text[i] <= 'f'))
		                    : text[i] != (uint8_t)shape[i])
			return "must be a PCI address as sysfs names it: \"0000:01:00.0\"";
	return NULL;
}

/* Lowercase words: an arch, an engine, a medium's type, a fidelity need. */
static const char *identifier(const uint8_t *text, uint32_t length)
{
	if (length == 0u || !(is_lower(text[0]) || is_digit(text[0])))
		return "must be lowercase letters, digits, `_` and `-`";
	for (uint32_t i = 1u; i < length; i++)
		if (!is_lower(text[i]) && !is_digit(text[i]) && text[i] != '_' && text[i] != '-')
			return "must be lowercase letters, digits, `_` and `-`";
	return NULL;
}

/* A DNS label, since a machine's name and hostname become `<name>.hull`. */
static const char *label(const uint8_t *text, uint32_t length)
{
	static const char reason[] = "must be a DNS label: up to 63 lowercase "
	                             "letters, digits and inner `-`";

	if (length == 0u || length > 63u || text[0] == '-' || text[length - 1u] == '-')
		return reason;
	for (uint32_t i = 0u; i < length; i++)
		if (!is_lower(text[i]) && !is_digit(text[i]) && text[i] != '-')
			return reason;
	return NULL;
}

/* A Linux interface's name, a host bridge's: 1 to 15 bytes, none of them
 * `/`, `:`, a space or a control, as the kernel takes one (IFNAMSIZ). */
static const char *interface(const uint8_t *text, uint32_t length)
{
	if (length == 0u || length > 15u)
		return "must be an interface's name: 1 to 15 characters";
	for (uint32_t i = 0u; i < length; i++)
		if (text[i] <= ' ' || text[i] == '/' || text[i] == ':' || text[i] >= 0x7fu)
			return "must be an interface's name: no `/`, `:`, space or control";
	return NULL;
}

/* An IPv6 network, "fd42:99::/64", beside a segment's IPv4 one: its host
 * bits clear and a prefix from 48 to 120, so that it holds hull's ::1 and
 * every machine a segment can, each drawn an address of its own. */
static const char *subnet6(const uint8_t *text, uint32_t length)
{
	static const char reason[] = "must be an IPv6 network, an address/n with n from 48 to "
	                             "120 and the host bits clear";
	char            copy[64];
	const char     *slash;
	struct in6_addr address;
	unsigned        prefix = 0u;
	unsigned        digits = 0u;

	if (length >= sizeof copy)
		return reason;
	memcpy(copy, text, length);
	copy[length] = '\0';
	if ((slash = strchr(copy, '/')) == NULL)
		return reason;
	for (const char *c = slash + 1; *c != '\0'; c++, digits++) {
		if (!is_digit((uint8_t)*c) || digits == 3u)
			return reason;
		prefix = prefix * 10u + (unsigned)(*c - '0');
	}
	copy[slash - copy] = '\0';
	if (digits == 0u || slash[1] == '0' || prefix < 48u || prefix > 120u ||
	    inet_pton(AF_INET6, copy, &address) != 1)
		return reason;
	for (unsigned bit = prefix; bit < 128u; bit++)
		if (address.s6_addr[bit / 8u] & (0x80u >> (bit % 8u)))
			return reason;
	return NULL;
}

/* An IPv4 network, "10.99.0.0/24": a dotted quad with its host bits
 * clear, and a prefix from 8 to 29, so that a segment holds the network,
 * the broadcast, hull's .1 and at least five machines (Appendix D). */
static const char *subnet(const uint8_t *text, uint32_t length)
{
	static const char reason[] = "must be an IPv4 network, a.b.c.d/n with n from 8 to 29 "
	                             "and the host bits clear";
	uint32_t address = 0u;
	uint32_t i = 0u;

	for (unsigned part = 0u; part < 5u; part++) {
		uint32_t value = 0u;
		uint32_t digits = 0u;

		while (i < length && is_digit(text[i]) && digits < 3u) {
			value = value * 10u + (uint32_t)(text[i++] - '0');
			digits++;
		}
		if (digits == 0u || (digits > 1u && text[i - digits] == '0') || value > 255u)
			return reason;
		if (part == 4u) {
			if (i != length || value < 8u || value > 29u ||
			    (address & ~(0xffffffffu << (32u - value))) != 0u)
				return reason;
			return NULL;
		}
		address = address << 8 | value;
		if (i == length || text[i++] != (part == 3u ? '/' : '.'))
			return reason;
	}
	return reason;
}

static const char *env_name(const uint8_t *text, uint32_t length)
{
	static const char reason[] = "must be letters, digits and `_`, not starting "
	                             "with a digit";

	if (length == 0u || is_digit(text[0]))
		return reason;
	for (uint32_t i = 0u; i < length; i++)
		if (!is_lower(text[i]) && !is_upper(text[i]) && !is_digit(text[i]) && text[i] != '_')
			return reason;
	return NULL;
}

/* A secret is materialised as /run/hull/secrets/<name>, so its name is
 * one path component, and never `.` or `..`. */
static const char *secret_name(const uint8_t *text, uint32_t length)
{
	static const char reason[] = "must be letters, digits, `_` and `-`";

	if (length == 0u || length > 255u)
		return reason;
	for (uint32_t i = 0u; i < length; i++)
		if (!is_lower(text[i]) && !is_upper(text[i]) && !is_digit(text[i]) &&
		    text[i] != '_' && text[i] != '-')
			return reason;
	return NULL;
}

/* Digits, not zero, then at most one byte from `units`. */
static int amount(const uint8_t *text, uint32_t length, const char *units, int unit_required)
{
	uint32_t digits = 0u;
	int      zero   = 1;

	while (digits < length && is_digit(text[digits])) {
		if (text[digits] != '0')
			zero = 0;
		digits++;
	}
	if (digits == 0u || zero)
		return 0;
	if (digits == length)
		return !unit_required;
	return digits + 1u == length && strchr(units, text[digits]) != NULL;
}

static const char *size(const uint8_t *text, uint32_t length)
{
	return amount(text, length, "KMGT", 0) ? NULL :
	       "must be a size above zero: digits, then K, M, G or T or nothing";
}

static const char *duration(const uint8_t *text, uint32_t length)
{
	return amount(text, length, "smhdw", 1) ? NULL :
	       "must be a duration above zero: digits, then s, m, h, d or w";
}

/* One of `schemes`, with something after it. */
static int has_scheme(const uint8_t *text, uint32_t length, const char *const *schemes)
{
	for (; *schemes != NULL; schemes++) {
		size_t n = strlen(*schemes);

		if (length > n && memcmp(text, *schemes, n) == 0)
			return 1;
	}
	return 0;
}

static const char *medium_source(const uint8_t *text, uint32_t length)
{
	static const char *const schemes[] = { "dir:", "tar:", "pack:", "image:", NULL };

	return has_scheme(text, length, schemes) ? NULL :
	       "must be \"dir:\", \"tar:\", \"pack:\" or \"image:\" and a path";
}

static const char *secret_source(const uint8_t *text, uint32_t length)
{
	static const char *const schemes[] = { "file:", "age:", "exec:", NULL };

	return has_scheme(text, length, schemes) ? NULL :
	       "must be \"file:\", \"age:\" or \"exec:\" and what it names";
}

/* --- Appendix J ---------------------------------------------------------- */

static const char *const mutable_values[]    = { "direct", "overlay", NULL };
static const char *const writeback_values[]  = { "none", "sync-on-stop", NULL };
static const char *const projection_values[] = { "auto", "bind", "passthrough", "block", NULL };
static const char *const restart_values[]    = { "no", "on-failure", "always", NULL };
static const char *const disconnect_values[] = { "stop", "keep", NULL };
static const char *const network_values[]    = { "bridge", "host", "nat", "none", NULL };
static const char *const proto_values[]      = { "tcp", "udp", NULL };
static const char *const mode_values[]       = { "rw", "ro", NULL };
static const char *const expose_values[]     = { "env", NULL };
static const char *const profile_values[]    = { "micro", "full", NULL };
static const char *const cpu_values[]        = { "host", "migratable", NULL };
static const char *const accel_values[]      = { "auto", "kvm", "tcg", NULL };

#define TABLE(keys) { keys, (unsigned)(sizeof keys / sizeof keys[0]), NULL }
#define TABLE_ACROSS(keys, across) { keys, (unsigned)(sizeof keys / sizeof keys[0]), across }

/* (port (host 8080) (guest 80) (proto tcp)) */
static const key port_keys[] = {
	{ .name = "host",  .type = PORT, .required = 1 },
	{ .name = "guest", .type = PORT, .required = 1 },
	{ .name = "proto", .type = CHOICE, .choices = proto_values },
};
static const schema port = TABLE(port_keys);

static hull_spec_status volume_across(walk *w, const hull_sexpr_form *record,
                                      const hull_sexpr_form *found, uint64_t present);

/* (volume (host "./data") (guest "/var/data") (mode rw)), or a stack of
 * directories in place of the one: (volume (guest "/etc") (layers
 * "dir:./etc-template" "dir:./etc-site") (upper "./etc-data")), 3.7. */
static const key volume_keys[] = {
	{ .name = "host",   .type = STRING, .value = nonempty },
	{ .name = "guest",  .type = STRING, .required = 1, .value = absolute },
	{ .name = "mode",   .type = CHOICE, .choices = mode_values },
	{ .name = "layers", .type = STRINGS, .nonempty = 1, .value = nonempty },
	{ .name = "upper",  .type = STRING, .value = nonempty },
};
static const schema volume = TABLE_ACROSS(volume_keys, volume_across);

/* Where these sit in volume_keys, for volume_across. */
#define VOLUME_HOST   0u
#define VOLUME_MODE   2u
#define VOLUME_LAYERS 3u
#define VOLUME_UPPER  4u

/* Devices passed through (section 4.13): one USB device, by its ids, by
 * the port it is plugged into, or both. */
static hull_spec_status usb_across(walk *w, const hull_sexpr_form *record,
                                   const hull_sexpr_form *found, uint64_t present);
static const key usb_keys[] = {
	{ .name = "vendor",  .type = STRING, .value = usb_id },
	{ .name = "product", .type = STRING, .value = usb_id },
	{ .name = "serial",  .type = STRING, .value = nonempty },
	{ .name = "port",    .type = STRING, .value = usb_port },
};
static const schema usb_device = TABLE_ACROSS(usb_keys, usb_across);

/* Where these sit in usb_keys, for usb_across. */
#define USB_VENDOR  0u
#define USB_PRODUCT 1u
#define USB_SERIAL  2u
#define USB_PORT    3u

static const key device_keys[] = {
	{ .name = "usb",      .type = ENTRIES, .nonempty = 1, .record = &usb_device, .entry = "device" },
	{ .name = "serial",   .type = STRINGS, .nonempty = 1, .value = absolute },
	{ .name = "parallel", .type = STRINGS, .nonempty = 1, .value = absolute },
	{ .name = "node",     .type = STRINGS, .nonempty = 1, .value = absolute },
	{ .name = "pci",      .type = STRINGS, .nonempty = 1, .value = pci_address },
};
static const schema devices = TABLE(device_keys);

/* J.2 */
static const key security_keys[] = {
	{ .name = "readonly_root", .type = BOOLEAN },
	{ .name = "no_new_privs",  .type = BOOLEAN },
	{ .name = "allow",         .type = SYMBOLS },
};
static const schema security = TABLE(security_keys);

/* J.3: what follows a secret's name. */
static const key secret_keys[] = {
	{ .name = "source", .type = STRING, .required = 1, .value = secret_source },
	{ .name = "expose", .type = CHOICE, .choices = expose_values },
};
static const schema secret = TABLE(secret_keys);

/* J.4 */
static const key medium_keys[] = {
	{ .name = "type",   .type = SYMBOL, .required = 1, .value = identifier },
	{ .name = "source", .type = STRING, .required = 1, .value = medium_source },
};
static const schema medium = TABLE(medium_keys);

/* J.5, an entry per class and per engine. */
static const key vm_keys[] = {
	{ .name = "profile",     .type = CHOICE, .choices = profile_values },
	{ .name = "kernel",      .type = KERNEL },
	{ .name = "cpu_profile", .type = CHOICE, .choices = cpu_values },
};
static const schema vm = TABLE(vm_keys);

static const key emu_keys[] = {
	{ .name = "cores_scale", .type = NUMBER },
};
static const schema emu = TABLE(emu_keys);

static const key ns_keys[] = {
	{ .name = "binfmt", .type = BOOLEAN },
};
static const schema ns = TABLE(ns_keys);

static const key qemu_keys[] = {
	{ .name = "accel", .type = CHOICE, .choices = accel_values },
};
static const schema qemu = TABLE(qemu_keys);

static const key virtualbox_keys[] = {
	{ .name = "gui", .type = BOOLEAN },
};
static const schema virtualbox = TABLE(virtualbox_keys);

static const key tuning_keys[] = {
	{ .name = "vm",         .type = RECORD, .record = &vm },
	{ .name = "emu",        .type = RECORD, .record = &emu },
	{ .name = "ns",         .type = RECORD, .record = &ns },
	{ .name = "qemu",       .type = RECORD, .record = &qemu },
	{ .name = "virtualbox", .type = RECORD, .record = &virtualbox },
	{ .name = "docker",     .type = RESERVED },
	{ .name = "vmware",     .type = RESERVED },
	{ .name = "hyperv",     .type = RESERVED },
};
static const schema tuning = TABLE(tuning_keys);

/* netem's Gilbert-Elliott model (tc-netem(8)): p leaves the good state,
 * r the bad, and each state loses its own share. */
static const key gemodel_keys[] = {
	{ .name = "p",         .type = NUMBER, .required = 1 },
	{ .name = "r",         .type = NUMBER },
	{ .name = "bad_loss",  .type = NUMBER },
	{ .name = "good_loss", .type = NUMBER },
};
static const schema gemodel = TABLE(gemodel_keys);

/* One direction of a machine's link to a segment (Appendix D), in
 * netem's terms: the core checks that a probability is at most 1. */
static const key link_way_keys[] = {
	{ .name = "down",      .type = BOOLEAN },
	{ .name = "mtu",       .type = COUNT },
	{ .name = "loss",      .type = NUMBER },
	{ .name = "gemodel",   .type = RECORD, .record = &gemodel },
	{ .name = "duplicate", .type = NUMBER },
	{ .name = "corrupt",   .type = NUMBER },
	{ .name = "delay",     .type = COUNT },
	{ .name = "jitter",    .type = COUNT },
	{ .name = "reorder",   .type = NUMBER },
	{ .name = "rate",      .type = COUNT },
	{ .name = "limit",     .type = COUNT },
	{ .name = "seed",      .type = COUNT },
};
static const schema link_way = TABLE(link_way_keys);

/* What the machine sends to a segment, and what it is given from it. */
static const key link_keys[] = {
	{ .name = "out", .type = RECORD, .record = &link_way },
	{ .name = "in",  .type = RECORD, .record = &link_way },
};
static const schema link = TABLE(link_keys);

/* A targeted fault on one direction of a link (Appendix D): a matched
 * frame dropped, rewritten or a bit flipped, and replayed. match and set
 * are "OFFSET:HEX"; the core checks the shape and that a probability is
 * at most 1. */
static const key fault_keys[] = {
	{ .name = "match",   .type = STRING, .value = nonempty },
	{ .name = "drop",    .type = NUMBER },
	{ .name = "set",     .type = STRING, .value = nonempty },
	{ .name = "rewrite", .type = NUMBER },
	{ .name = "flip",    .type = NUMBER },
	{ .name = "replay",  .type = NUMBER },
	{ .name = "copies",  .type = COUNT },
	{ .name = "every",   .type = COUNT },
};
static const schema fault = TABLE(fault_keys);

static const key fault_link_keys[] = {
	{ .name = "out", .type = RECORD, .record = &fault },
	{ .name = "in",  .type = RECORD, .record = &fault },
};
static const schema fault_link = TABLE(fault_link_keys);

/* A segment hull addresses (Appendix D): its machines given an address
 * each in the subnet, the IPv6 one, or both, and each other's names. */
static hull_spec_status segment_across(walk *w, const hull_sexpr_form *record,
                                       const hull_sexpr_form *found, uint64_t present);
static const key segment_keys[] = {
	{ .name = "subnet", .type = STRING, .value = subnet },
	{ .name = "subnet6", .type = STRING, .value = subnet6 },
};
static const schema segment = TABLE_ACROSS(segment_keys, segment_across);

static hull_spec_status segment_across(walk *w, const hull_sexpr_form *record,
                                       const hull_sexpr_form *found, uint64_t present)
{
	(void)found;
	if ((present & 3u) == 0u)
		return fail(w, HULL_SPEC_ERR_MISSING, record->offset,
		            "a segment hull addresses is given its subnet, its subnet6, or both");
	return HULL_SPEC_OK;
}

/* J.1 */
static const key machine_keys[] = {
	{ .name = "source",        .type = STRING, .required = 1, .value = nonempty },
	{ .name = "mutable",       .type = CHOICE, .choices = mutable_values },
	{ .name = "writeback",     .type = CHOICE, .choices = writeback_values },
	{ .name = "projection",    .type = CHOICE, .choices = projection_values },
	{ .name = "fidelity",      .type = SYMBOLS, .value = identifier },
	{ .name = "arch",          .type = SYMBOL, .value = identifier },
	{ .name = "command",       .type = STRINGS, .nonempty = 1 },
	{ .name = "workdir",       .type = STRING, .value = absolute },
	{ .name = "env",           .type = MAP, .names = env_name },
	{ .name = "env_file",      .type = STRING, .value = nonempty },
	{ .name = "ports",         .type = ENTRIES, .record = &port, .entry = "port" },
	{ .name = "volumes",       .type = ENTRIES, .record = &volume, .entry = "volume" },
	{ .name = "cpus",          .type = COUNT },
	{ .name = "memory",        .type = STRING, .value = size },
	{ .name = "backend",       .type = SYMBOL, .value = identifier },
	{ .name = "ssh",           .type = BOOLEAN },
	{ .name = "restart",       .type = CHOICE, .choices = restart_values },
	{ .name = "on_disconnect", .type = CHOICE, .choices = disconnect_values },
	{ .name = "network",       .type = CHOICE, .choices = network_values },
	{ .name = "segments",      .type = NAMES, .record = &segment, .names = label },
	{ .name = "impair",        .type = MAP, .record = &link, .names = label },
	{ .name = "tamper",        .type = MAP, .record = &fault_link, .names = label },
	{ .name = "init",          .type = STRING, .value = absolute },
	{ .name = "export",        .type = STRING, .value = nonempty },
	{ .name = "owners",        .type = STRING, .value = nonempty },
	{ .name = "layers",        .type = STRINGS, .value = nonempty },
	{ .name = "upper",         .type = STRING, .value = nonempty },
	{ .name = "hostname",      .type = STRING, .value = label },
	{ .name = "security",      .type = RECORD, .record = &security },
	{ .name = "secrets",       .type = MAP, .record = &secret, .names = secret_name },
	{ .name = "media",         .type = ENTRIES, .record = &medium, .entry = "medium" },
	{ .name = "tuning",        .type = RECORD, .record = &tuning },
	{ .name = "bridge",        .type = STRING, .value = interface },
	{ .name = "devices",       .type = RECORD, .record = &devices },
};
static hull_spec_status machine_across(walk *w, const hull_sexpr_form *record,
                                       const hull_sexpr_form *found, uint64_t present);
static const schema machine = TABLE_ACROSS(machine_keys, machine_across);

/* Where these sit in machine_keys, for machine_across; spec_test holds
 * both, a key moved making (network bridge) (bridge NAME) refused. */
#define MACHINE_NETWORK 18u
#define MACHINE_BRIDGE  32u

/* J.6: (path (from "./src") (to "/app")), what a step copies in. */
static const key path_keys[] = {
	{ .name = "from", .type = STRING, .required = 1, .value = nonempty },
	{ .name = "to",   .type = STRING, .required = 1, .value = absolute },
};
static const schema path = TABLE(path_keys);

static hull_spec_status step_across(walk *w, const hull_sexpr_form *record,
                                    const hull_sexpr_form *found, uint64_t present);

static const key step_keys[] = {
	{ .name = "name",    .type = STRING, .required = 1, .value = nonempty },
	{ .name = "run",     .type = STRING, .value = nonempty },
	{ .name = "copy",    .type = ENTRIES, .record = &path, .entry = "path" },
	{ .name = "inputs",  .type = STRINGS, .value = nonempty },
	{ .name = "network", .type = BOOLEAN },
	{ .name = "refresh", .type = STRING, .value = duration },
	{ .name = "backend", .type = SYMBOL, .value = identifier },
	{ .name = "discard", .type = STRINGS, .value = absolute },
	{ .name = "env",     .type = MAP, .names = env_name },
	{ .name = "workdir", .type = STRING, .value = absolute },
};
static const schema step = TABLE_ACROSS(step_keys, step_across);

/* Where these sit in step_keys, for step_across. */
#define STEP_NETWORK 4u
#define STEP_REFRESH 5u

/* J.6: what the build's image carries, A.4's runtime in its keys, laid
 * over the base's. */
static const key runtime_keys[] = {
	{ .name = "cmd",         .type = STRINGS },
	{ .name = "entrypoint",  .type = STRINGS },
	{ .name = "env",         .type = MAP, .names = env_name },
	{ .name = "ports",       .type = STRINGS, .value = nonempty },
	{ .name = "stop_signal", .type = STRING, .value = nonempty },
	{ .name = "user",        .type = STRING, .value = nonempty },
	{ .name = "volumes",     .type = STRINGS, .value = absolute },
	{ .name = "workdir",     .type = STRING, .value = absolute },
};
static const schema runtime = TABLE(runtime_keys);

static const key build_keys[] = {
	{ .name = "base",    .type = STRING, .required = 1, .value = nonempty },
	{ .name = "steps",   .type = ENTRIES, .record = &step, .entry = "step" },
	{ .name = "runtime", .type = RECORD, .record = &runtime },
};
static const schema build = TABLE(build_keys);

static hull_spec_status file_across(walk *w, const hull_sexpr_form *record,
                                    const hull_sexpr_form *found, uint64_t present);

/* The top of a file (J.7): one machine, or several by name, and a build. */
static const key file_keys[] = {
	{ .name = "machine",  .type = RECORD, .record = &machine },
	{ .name = "machines", .type = MAP, .record = &machine, .names = label, .nonempty = 1 },
	{ .name = "build",    .type = RECORD, .record = &build },
};
static const schema file = TABLE_ACROSS(file_keys, file_across);

#define FILE_MACHINE  0u
#define FILE_MACHINES 1u
#define FILE_BUILD    2u

_Static_assert(sizeof machine_keys / sizeof machine_keys[0] <= MAX_KEYS,
               "a record has more keys than the walk can track");

/* --- values -------------------------------------------------------------- */

static hull_spec_status check_entries(walk *w, const hull_sexpr_form *first, int has_first,
                                      const hull_sexpr_form *record, uint32_t missing_at,
                                      const schema *s, hull_sexpr_form *found,
                                      uint64_t *present);

/* The one value of `entry`, whose key is `head`, which must be of `kind`. */
static hull_spec_status one_value(walk *w, const hull_sexpr_form *entry,
                                  const hull_sexpr_form *head, hull_sexpr_kind kind,
                                  hull_sexpr_form *value, const char *reason)
{
	hull_sexpr_form extra;

	if (!hull_sexpr_next(w->doc, head, value))
		return fail(w, HULL_SPEC_ERR_SHAPE, entry->offset, reason);
	if (value->kind != kind)
		return fail(w, HULL_SPEC_ERR_SHAPE, value->offset, reason);
	if (hull_sexpr_next(w->doc, value, &extra))
		return fail(w, HULL_SPEC_ERR_SHAPE, extra.offset, reason);
	return HULL_SPEC_OK;
}

/* A whole number in minimal decimal, from 1 to `max`. */
static int whole(const hull_sexpr_doc *doc, const hull_sexpr_form *form, uint32_t max)
{
	const uint8_t *text  = doc->data + form->content;
	uint32_t       value = 0u;

	if (form->content_length == 0u || text[0] == '0')
		return 0;
	for (uint32_t i = 0u; i < form->content_length; i++) {
		if (!is_digit(text[i]) || value > (max - (uint32_t)(text[i] - '0')) / 10u)
			return 0;
		value = value * 10u + (uint32_t)(text[i] - '0');
	}
	return 1;
}

/* Digits, then optionally a point and more digits, and not zero. */
static int decimal(const hull_sexpr_doc *doc, const hull_sexpr_form *form)
{
	const uint8_t *text   = doc->data + form->content;
	uint32_t       length = form->content_length;
	uint32_t       i      = 0u;
	int            zero   = 1;

	while (i < length && is_digit(text[i])) {
		zero &= text[i] == '0';
		i++;
	}
	if (i == 0u || (i > 1u && text[0] == '0'))
		return 0;
	if (i < length) {
		uint32_t point = i++;

		while (i < length && is_digit(text[i])) {
			zero &= text[i] == '0';
			i++;
		}
		if (i == point + 1u)
			return 0;
	}
	return i == length && !zero;
}

static hull_spec_status apply(walk *w, rule r, const hull_sexpr_form *form)
{
	const char *reason;

	if (r == NULL)
		return HULL_SPEC_OK;
	reason = r(w->doc->data + form->content, form->content_length);
	return reason == NULL ? HULL_SPEC_OK : fail(w, HULL_SPEC_ERR_VALUE, form->offset, reason);
}

static int chosen(const hull_sexpr_doc *doc, const hull_sexpr_form *form,
                  const char *const *choices)
{
	for (; *choices != NULL; choices++)
		if (hull_sexpr_is(doc, form, *choices))
			return 1;
	return 0;
}

/* Every value after `head` is of `kind` and passes `r`. */
static hull_spec_status each_value(walk *w, const hull_sexpr_form *entry,
                                   const hull_sexpr_form *head, const key *k,
                                   hull_sexpr_kind kind, const char *reason)
{
	hull_sexpr_form  value;
	hull_spec_status status;
	int              more = hull_sexpr_next(w->doc, head, &value);

	if (!more && k->nonempty)
		return fail(w, HULL_SPEC_ERR_SHAPE, entry->offset, reason);
	for (; more; more = hull_sexpr_next(w->doc, &value, &value)) {
		if (value.kind != kind)
			return fail(w, HULL_SPEC_ERR_SHAPE, value.offset, reason);
		status = apply(w, k->value, &value);
		if (status != HULL_SPEC_OK)
			return status;
	}
	return HULL_SPEC_OK;
}

static hull_spec_status check_record(walk *w, const hull_sexpr_form *record,
                                     const hull_sexpr_form *head, const schema *s)
{
	hull_sexpr_form found[MAX_KEYS];
	hull_sexpr_form first;
	uint64_t        present = 0u;
	int             has     = hull_sexpr_next(w->doc, head, &first);

	return check_entries(w, &first, has, record, record->offset, s, found, &present);
}

/* (plural (singular ...) ...) */
static hull_spec_status check_list(walk *w, const hull_sexpr_form *entry,
                                   const hull_sexpr_form *head, const key *k)
{
	hull_sexpr_form  item;
	hull_sexpr_form  name;
	hull_spec_status status;
	int              more = hull_sexpr_next(w->doc, head, &item);

	if (!more && k->nonempty)
		return fail(w, HULL_SPEC_ERR_SHAPE, entry->offset, "holds at least one entry");
	for (; more; more = hull_sexpr_next(w->doc, &item, &item)) {
		size_t saved;

		if (item.kind != HULL_SEXPR_LIST || !hull_sexpr_child(w->doc, &item, &name) ||
		    name.kind != HULL_SEXPR_SYMBOL)
			return fail(w, HULL_SPEC_ERR_SHAPE, item.offset,
			            "holds only entries, each a list opening with its key");
		saved = push_form(w, &name);
		if (!hull_sexpr_is(w->doc, &name, k->entry))
			return fail(w, HULL_SPEC_ERR_UNKNOWN, name.offset,
			            "is not the entry this list holds");
		status = check_record(w, &item, &name, k->record);
		if (status != HULL_SPEC_OK)
			return status;
		pop(w, saved);
	}
	return HULL_SPEC_OK;
}

/* (key (name ...) ...), the names the user's own. */
static hull_spec_status check_map(walk *w, const hull_sexpr_form *entry,
                                  const hull_sexpr_form *head, const key *k)
{
	hull_sexpr_form  item;
	hull_sexpr_form  name;
	hull_sexpr_form  value;
	hull_spec_status status;
	int              more = hull_sexpr_next(w->doc, head, &item);

	if (!more && k->nonempty)
		return fail(w, HULL_SPEC_ERR_SHAPE, entry->offset, "holds at least one name");
	for (; more; more = hull_sexpr_next(w->doc, &item, &item)) {
		hull_sexpr_form earlier;
		hull_sexpr_form earlier_name;
		size_t          saved;

		if (item.kind != HULL_SEXPR_LIST || !hull_sexpr_child(w->doc, &item, &name) ||
		    name.kind != HULL_SEXPR_SYMBOL)
			return fail(w, HULL_SPEC_ERR_SHAPE, item.offset,
			            "holds only entries, each a list opening with its name");
		saved = push_form(w, &name);
		status = apply(w, k->names, &name);
		if (status != HULL_SPEC_OK)
			return status;
		hull_sexpr_next(w->doc, head, &earlier);
		for (; earlier.offset != item.offset; hull_sexpr_next(w->doc, &earlier, &earlier)) {
			hull_sexpr_child(w->doc, &earlier, &earlier_name);
			if (same_name(w->doc, &earlier_name, &name))
				return fail(w, HULL_SPEC_ERR_DUPLICATE, item.offset, "is named twice");
		}
		if (k->record != NULL)
			status = check_record(w, &item, &name, k->record);
		else
			status = one_value(w, &item, &name, HULL_SEXPR_STRING, &value,
			                   "takes one string");
		if (status != HULL_SPEC_OK)
			return status;
		pop(w, saved);
	}
	return HULL_SPEC_OK;
}

/* (key "name" (name ...) ...): each a name alone, as a string, or a name
 * with a record; no name twice, whichever its form. */
static hull_spec_status check_names(walk *w, const hull_sexpr_form *head, const key *k)
{
	hull_sexpr_form  item;
	hull_spec_status status;
	int              more = hull_sexpr_next(w->doc, head, &item);

	for (; more; more = hull_sexpr_next(w->doc, &item, &item)) {
		hull_sexpr_form earlier;
		hull_sexpr_form name = item;
		hull_sexpr_form earlier_name;
		size_t          saved;

		if (item.kind == HULL_SEXPR_LIST &&
		    (!hull_sexpr_child(w->doc, &item, &name) || name.kind != HULL_SEXPR_SYMBOL))
			return fail(w, HULL_SPEC_ERR_SHAPE, item.offset,
			            "holds names, each a string or a list opening with its name");
		if (item.kind != HULL_SEXPR_LIST && item.kind != HULL_SEXPR_STRING)
			return fail(w, HULL_SPEC_ERR_SHAPE, item.offset,
			            "holds names, each a string or a list opening with its name");
		status = apply(w, k->names, &name);
		if (status != HULL_SPEC_OK)
			return status;
		hull_sexpr_next(w->doc, head, &earlier);
		for (; earlier.offset != item.offset; hull_sexpr_next(w->doc, &earlier, &earlier)) {
			earlier_name = earlier;
			if (earlier.kind == HULL_SEXPR_LIST)
				hull_sexpr_child(w->doc, &earlier, &earlier_name);
			if (same_name(w->doc, &earlier_name, &name))
				return fail(w, HULL_SPEC_ERR_DUPLICATE, item.offset, "is named twice");
		}
		if (item.kind != HULL_SEXPR_LIST)
			continue;
		saved = push_form(w, &name);
		status = check_record(w, &item, &name, k->record);
		if (status != HULL_SPEC_OK)
			return status;
		pop(w, saved);
	}
	return HULL_SPEC_OK;
}

/* The values of `entry`, whose key `head` is `k`. */
static hull_spec_status check_value(walk *w, const hull_sexpr_form *entry,
                                    const hull_sexpr_form *head, const key *k)
{
	hull_sexpr_form  value;
	hull_spec_status status;

	switch (k->type) {
	case STRING:
		status = one_value(w, entry, head, HULL_SEXPR_STRING, &value, "takes one string");
		return status != HULL_SPEC_OK ? status : apply(w, k->value, &value);
	case STRINGS:
		return each_value(w, entry, head, k, HULL_SEXPR_STRING,
		                  k->nonempty ? "takes one string or more" : "takes only strings");
	case SYMBOL:
		status = one_value(w, entry, head, HULL_SEXPR_SYMBOL, &value, "takes one symbol");
		return status != HULL_SPEC_OK ? status : apply(w, k->value, &value);
	case SYMBOLS:
		return each_value(w, entry, head, k, HULL_SEXPR_SYMBOL, "takes only symbols");
	case CHOICE:
		status = one_value(w, entry, head, HULL_SEXPR_SYMBOL, &value, "takes one symbol");
		if (status == HULL_SPEC_OK && !chosen(w->doc, &value, k->choices))
			return fail(w, HULL_SPEC_ERR_VALUE, value.offset, "is not one of this key's values");
		return status;
	case BOOLEAN:
		status = one_value(w, entry, head, HULL_SEXPR_SYMBOL, &value, "takes true or false");
		if (status == HULL_SPEC_OK && !hull_sexpr_is(w->doc, &value, "true") &&
		    !hull_sexpr_is(w->doc, &value, "false"))
			return fail(w, HULL_SPEC_ERR_VALUE, value.offset, "takes true or false");
		return status;
	case COUNT:
	case PORT:
		status = one_value(w, entry, head, HULL_SEXPR_SYMBOL, &value, "takes one number");
		if (status == HULL_SPEC_OK && !whole(w->doc, &value, k->type == PORT ? 65535u : UINT32_MAX))
			return fail(w, HULL_SPEC_ERR_VALUE, value.offset, k->type == PORT ?
			            "must be a port, from 1 to 65535" :
			            "must be a whole number from 1, without leading zeros");
		return status;
	case NUMBER:
		status = one_value(w, entry, head, HULL_SEXPR_SYMBOL, &value, "takes one number");
		if (status == HULL_SPEC_OK && !decimal(w->doc, &value))
			return fail(w, HULL_SPEC_ERR_VALUE, value.offset,
			            "must be a number above zero: 2, 1.5");
		return status;
	case KERNEL:
		if (hull_sexpr_next(w->doc, head, &value) && value.kind == HULL_SEXPR_STRING)
			status = one_value(w, entry, head, HULL_SEXPR_STRING, &value, "takes one value");
		else
			status = one_value(w, entry, head, HULL_SEXPR_SYMBOL, &value, "takes one value");
		if (status != HULL_SPEC_OK)
			return status;
		if (value.kind == HULL_SEXPR_STRING) {
			static const char *const schemes[] = { "path:", NULL };

			if (!has_scheme(w->doc->data + value.content, value.content_length, schemes))
				return fail(w, HULL_SPEC_ERR_VALUE, value.offset,
				            "must be bundled, image or \"path:<file>\"");
		} else if (!hull_sexpr_is(w->doc, &value, "bundled") &&
		           !hull_sexpr_is(w->doc, &value, "image")) {
			return fail(w, HULL_SPEC_ERR_VALUE, value.offset,
			            "must be bundled, image or \"path:<file>\"");
		}
		return HULL_SPEC_OK;
	case RECORD:
		return check_record(w, entry, head, k->record);
	case ENTRIES:
		return check_list(w, entry, head, k);
	case MAP:
		return check_map(w, entry, head, k);
	case NAMES:
		return check_names(w, head, k);
	case RESERVED:
		return fail(w, HULL_SPEC_ERR_UNKNOWN, head->offset,
		            "is reserved, and defined with its engine's adapter");
	}
	return fail(w, HULL_SPEC_ERR_UNKNOWN, head->offset, "has no type");
}

/* --- records ------------------------------------------------------------- */

/*
 * The entries from `first` on, held to `s`. `found` and `present` say
 * which keys were given and where, for the rules across keys and for the
 * caller. A required key that is missing is reported at `missing_at`.
 */
static hull_spec_status check_entries(walk *w, const hull_sexpr_form *first, int has_first,
                                      const hull_sexpr_form *record, uint32_t missing_at,
                                      const schema *s, hull_sexpr_form *found,
                                      uint64_t *present)
{
	hull_sexpr_form  entry = *first;
	hull_sexpr_form  head;
	hull_spec_status status;
	int              more  = has_first;

	*present = 0u;
	for (; more; more = hull_sexpr_next(w->doc, &entry, &entry)) {
		unsigned i;
		size_t   saved;

		if (entry.kind != HULL_SEXPR_LIST || !hull_sexpr_child(w->doc, &entry, &head) ||
		    head.kind != HULL_SEXPR_SYMBOL)
			return fail(w, HULL_SPEC_ERR_SHAPE, entry.offset,
			            "is not an entry: a list opening with its key");
		saved = push_form(w, &head);
		for (i = 0u; i < s->count; i++)
			if (hull_sexpr_is(w->doc, &head, s->keys[i].name))
				break;
		if (i == s->count)
			return fail(w, HULL_SPEC_ERR_UNKNOWN, head.offset, "is not a key here");
		if (*present & ((uint64_t)1 << i))
			return fail(w, HULL_SPEC_ERR_DUPLICATE, entry.offset, "is given twice");
		*present |= (uint64_t)1 << i;
		found[i] = entry;
		status = check_value(w, &entry, &head, &s->keys[i]);
		if (status != HULL_SPEC_OK)
			return status;
		pop(w, saved);
	}
	for (unsigned i = 0u; i < s->count; i++) {
		if (s->keys[i].required && !(*present & ((uint64_t)1 << i))) {
			size_t saved = push_text(w, s->keys[i].name);

			status = fail(w, HULL_SPEC_ERR_MISSING, missing_at, "is required");
			pop(w, saved);
			return status;
		}
	}
	return s->across == NULL ? HULL_SPEC_OK : s->across(w, record, found, *present);
}

/* (bridge NAME) names the host bridge (network bridge) joins, and means
 * nothing on another network. */
static hull_spec_status machine_across(walk *w, const hull_sexpr_form *record,
                                       const hull_sexpr_form *found, uint64_t present)
{
	hull_sexpr_form head;
	hull_sexpr_form value;
	size_t          saved;

	(void)record;
	if (!(present & ((uint64_t)1 << MACHINE_BRIDGE)))
		return HULL_SPEC_OK;
	if (present & ((uint64_t)1 << MACHINE_NETWORK)) {
		hull_sexpr_child(w->doc, &found[MACHINE_NETWORK], &head);
		hull_sexpr_next(w->doc, &head, &value);
		if (hull_sexpr_is(w->doc, &value, "bridge"))
			return HULL_SPEC_OK;
	}
	hull_sexpr_child(w->doc, &found[MACHINE_BRIDGE], &head);
	hull_sexpr_next(w->doc, &head, &value);
	saved = push_text(w, "bridge");
	fail(w, HULL_SPEC_ERR_VALUE, value.offset, "needs (network bridge)");
	pop(w, saved);
	return HULL_SPEC_ERR_VALUE;
}

/* J.6: refresh dates a step's cache, which is only dated with a network. */
static hull_spec_status step_across(walk *w, const hull_sexpr_form *record,
                                    const hull_sexpr_form *found, uint64_t present)
{
	hull_sexpr_form head;
	hull_sexpr_form value;
	size_t          saved;

	(void)record;
	if (!(present & ((uint64_t)1 << STEP_REFRESH)))
		return HULL_SPEC_OK;
	if (present & ((uint64_t)1 << STEP_NETWORK)) {
		hull_sexpr_child(w->doc, &found[STEP_NETWORK], &head);
		hull_sexpr_next(w->doc, &head, &value);
		if (hull_sexpr_is(w->doc, &value, "true"))
			return HULL_SPEC_OK;
	}
	saved = push_text(w, "refresh");
	fail(w, HULL_SPEC_ERR_VALUE, found[STEP_REFRESH].offset, "needs (network true)");
	pop(w, saved);
	return HULL_SPEC_ERR_VALUE;
}

static hull_spec_status file_across(walk *w, const hull_sexpr_form *record,
                                    const hull_sexpr_form *found, uint64_t present)
{
	(void)record;
	if ((present & ((uint64_t)1 << FILE_MACHINE)) && (present & ((uint64_t)1 << FILE_MACHINES))) {
		size_t saved = push_text(w, "machines");

		fail(w, HULL_SPEC_ERR_SHAPE, found[FILE_MACHINES].offset,
		     "cannot stand beside machine: a file holds one or the other");
		pop(w, saved);
		return HULL_SPEC_ERR_SHAPE;
	}
	if (present == 0u)
		return fail(w, HULL_SPEC_ERR_MISSING, 0u, "a file holds machine, machines or build");
	return HULL_SPEC_OK;
}

/* A device is its ids, both, or its port; a serial number tells apart
 * two of one vendor's and product's, so it needs them. */
static hull_spec_status usb_across(walk *w, const hull_sexpr_form *record,
                                   const hull_sexpr_form *found, uint64_t present)
{
	int         vendor  = (present & ((uint64_t)1 << USB_VENDOR)) != 0u;
	int         product = (present & ((uint64_t)1 << USB_PRODUCT)) != 0u;
	const char *why;
	unsigned    at;
	size_t      saved;

	if (!vendor && !product && !(present & ((uint64_t)1 << USB_PORT)))
		return fail(w, HULL_SPEC_ERR_MISSING, record->offset,
		            "a USB device is its vendor and product, or the port it is plugged into");
	if (vendor != product) {
		at = vendor ? USB_VENDOR : USB_PRODUCT;
		why = vendor ? "needs product beside it: a device is both ids" :
		               "needs vendor beside it: a device is both ids";
	} else if ((present & ((uint64_t)1 << USB_SERIAL)) && !vendor) {
		at = USB_SERIAL;
		why = "tells apart devices of one vendor and product: it needs both";
	} else {
		return HULL_SPEC_OK;
	}
	saved = push_text(w, usb_keys[at].name);
	fail(w, HULL_SPEC_ERR_SHAPE, found[at].offset, why);
	pop(w, saved);
	return HULL_SPEC_ERR_SHAPE;
}

static hull_spec_status volume_across(walk *w, const hull_sexpr_form *record,
                                      const hull_sexpr_form *found, uint64_t present)
{
	const char *why = NULL;
	unsigned    at = VOLUME_LAYERS;
	size_t      saved;

	if (!(present & ((uint64_t)1 << VOLUME_HOST)) && !(present & ((uint64_t)1 << VOLUME_LAYERS)))
		return fail(w, HULL_SPEC_ERR_MISSING, record->offset,
		            "a volume is a host directory or a stack of layers: host or layers");
	if ((present & ((uint64_t)1 << VOLUME_HOST)) && (present & ((uint64_t)1 << VOLUME_LAYERS))) {
		why = "cannot stand beside host: a volume is one directory or a stack of them";
	} else if ((present & ((uint64_t)1 << VOLUME_UPPER)) && !(present & ((uint64_t)1 << VOLUME_LAYERS))) {
		at = VOLUME_UPPER;
		why = "is a stack's: it needs layers";
	} else if ((present & ((uint64_t)1 << VOLUME_UPPER)) && (present & ((uint64_t)1 << VOLUME_MODE))) {
		hull_sexpr_form head;
		hull_sexpr_form value;

		hull_sexpr_child(w->doc, &found[VOLUME_MODE], &head);
		hull_sexpr_next(w->doc, &head, &value);
		if (hull_sexpr_is(w->doc, &value, "ro")) {
			at = VOLUME_UPPER;
			why = "keeps writes, which a volume of (mode ro) has none of";
		}
	}
	if (why == NULL)
		return HULL_SPEC_OK;
	saved = push_text(w, at == VOLUME_UPPER ? "upper" : "layers");
	fail(w, HULL_SPEC_ERR_SHAPE, found[at].offset, why);
	pop(w, saved);
	return HULL_SPEC_ERR_SHAPE;
}

/* --- the calls ----------------------------------------------------------- */

hull_spec_status hull_spec_open(hull_spec *spec, const uint8_t *data, size_t length,
                                hull_spec_error *error)
{
	hull_sexpr_error  syntax;
	hull_sexpr_form   found[MAX_KEYS];
	hull_sexpr_form   first;
	hull_spec_status  status;
	uint64_t          present = 0u;
	walk              w;

	memset(spec, 0, sizeof *spec);
	if (error != NULL)
		memset(error, 0, sizeof *error);
	if (hull_sexpr_open(&spec->doc, data, length, HULL_SEXPR_READABLE, &syntax) != HULL_SEXPR_OK) {
		if (error != NULL) {
			error->status = HULL_SPEC_ERR_SYNTAX;
			error->offset = syntax.offset;
			error->line   = syntax.line;
			error->column = syntax.column;
			error->reason = syntax.reason;
		}
		return HULL_SPEC_ERR_SYNTAX;
	}
	w.doc         = &spec->doc;
	w.error       = error;
	w.path[0]     = '\0';
	w.path_length = 0u;
	status = check_entries(&w, &first, hull_sexpr_first(&spec->doc, &first), NULL, 0u,
	                       &file, found, &present);
	if (status != HULL_SPEC_OK)
		return status;
	spec->has_machine  = !!(present & ((uint64_t)1 << FILE_MACHINE));
	spec->has_machines = !!(present & ((uint64_t)1 << FILE_MACHINES));
	spec->has_build    = !!(present & ((uint64_t)1 << FILE_BUILD));
	if (spec->has_machine)
		spec->machine = found[FILE_MACHINE];
	if (spec->has_machines)
		spec->machines = found[FILE_MACHINES];
	if (spec->has_build)
		spec->build = found[FILE_BUILD];
	return HULL_SPEC_OK;
}

int hull_spec_find(const hull_spec *spec, const hull_sexpr_form *record,
                   const char *key, hull_sexpr_form *entry)
{
	hull_sexpr_form head;
	hull_sexpr_form item;
	int             more;

	if (!hull_sexpr_child(&spec->doc, record, &head))
		return 0;
	for (more = hull_sexpr_next(&spec->doc, &head, &item); more;
	     more = hull_sexpr_next(&spec->doc, &item, &item)) {
		hull_sexpr_form name;

		if (hull_sexpr_child(&spec->doc, &item, &name) &&
		    hull_sexpr_is(&spec->doc, &name, key)) {
			*entry = item;
			return 1;
		}
	}
	return 0;
}
