/*
 * vmconf.c -- VMware's .vmx and VirtualBox's .vbox as a hull machine
 * (vmconf.h, project.md section 3.13).
 *
 * Each file says what its machine is -- memory, processors, firmware, a
 * TPM, a network card, and which disk and disc image sits on which port
 * -- and the pc class runs such a machine from its images as they are,
 * qemu reading VMDK and VDI. So the file is translated, not imported: a
 * (machine ...) form of the pc class, its media the file's images by
 * their paths, written in the order the file attaches them. What has no
 * meaning here, or cannot be carried, is said, never dropped quietly; an
 * image qemu cannot read as the machine's current state -- a VirtualBox
 * snapshot's differencing disk -- refuses the file, naming it.
 */

#define _GNU_SOURCE

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vmconf.h"

#define MAX_BYTES   (4u << 20)
#define MAX_MEDIA   32u
#define MAX_IMAGES  128u
#define MAX_KEYS    2048u

typedef struct medium {
	char type[8];     /* disk, cdrom or floppy */
	char path[4096];
	int  ide;         /* on an IDE controller, which a guest of its age may need */
} medium;

typedef struct machine {
	const char *file;
	FILE       *notes;
	char        name[64];
	unsigned long memory;   /* MiB, 0 where unsaid */
	unsigned    cpus;
	int         uefi;
	int         secure;
	int         tpm;
	int         network;
	char        nic[16];
	int         boot_cdrom;
	medium      media[MAX_MEDIA];
	unsigned    media_count;
	int         failed;
} machine;

static void note(machine *m, const char *what)
{
	if (m->notes != NULL)
		fprintf(m->notes, "%s: %s\n", m->file, what);
}

static void refuse(machine *m, const char *what)
{
	fprintf(stderr, "%s: %s\n", m->file, what);
	m->failed = 1;
}

static char *read_all(const char *path)
{
	FILE  *file = fopen(path, "re");
	char  *data;
	size_t n;

	if (file == NULL) {
		perror(path);
		return NULL;
	}
	if ((data = malloc(MAX_BYTES + 1u)) == NULL) {
		fclose(file);
		return NULL;
	}
	n = fread(data, 1u, MAX_BYTES + 1u, file);
	fclose(file);
	if (n > MAX_BYTES) {
		fprintf(stderr, "%s: larger than a machine's file is\n", path);
		free(data);
		return NULL;
	}
	data[n] = '\0';
	return data;
}

/* The machine's name as a hostname: lowercase letters, digits and '-'. */
static void hostname_of(machine *m, const char *name)
{
	size_t n = 0u;

	for (; *name != '\0' && n + 1u < sizeof m->name && n < 63u; name++) {
		unsigned char c = (unsigned char)*name;

		if (isalnum(c))
			m->name[n++] = (char)tolower(c);
		else if (n > 0u && m->name[n - 1u] != '-')
			m->name[n++] = '-';
	}
	while (n > 0u && m->name[n - 1u] == '-')
		n--;
	m->name[n] = '\0';
}

static void add_medium(machine *m, const char *type, const char *path, int ide)
{
	medium *d;

	if (m->media_count == MAX_MEDIA) {
		refuse(m, "more drives than a machine is given");
		return;
	}
	d = &m->media[m->media_count++];
	snprintf(d->type, sizeof d->type, "%s", type);
	snprintf(d->path, sizeof d->path, "%s", path);
	d->ide = ide;
}

/* --- VMware's .vmx -------------------------------------------------------- */

typedef struct vmx {
	char *key[MAX_KEYS];
	char *value[MAX_KEYS];
	unsigned count;
} vmx;

/* `|22` and the like, VMware's escape for a byte in a value. */
static void unescape_vmx(char *s)
{
	char *out = s;

	for (; *s != '\0'; s++) {
		if (s[0] == '|' && isxdigit((unsigned char)s[1]) && isxdigit((unsigned char)s[2])) {
			char hex[3] = { s[1], s[2], '\0' };

			*out++ = (char)strtol(hex, NULL, 16);
			s += 2;
		} else {
			*out++ = *s;
		}
	}
	*out = '\0';
}

static void parse_vmx(vmx *v, char *text)
{
	for (char *save = NULL, *line = strtok_r(text, "\r\n", &save); line != NULL;
	     line = strtok_r(NULL, "\r\n", &save)) {
		char *eq = strchr(line, '=');
		char *key = line;
		char *value;
		char *end;

		while (isspace((unsigned char)*key))
			key++;
		if (*key == '#' || *key == '.' || eq == NULL || v->count == MAX_KEYS)
			continue;
		end = eq;
		while (end > key && isspace((unsigned char)end[-1]))
			end--;
		*end = '\0';
		value = eq + 1;
		while (isspace((unsigned char)*value))
			value++;
		if (*value == '"') {
			value++;
			if ((end = strrchr(value, '"')) != NULL)
				*end = '\0';
		}
		for (char *c = key; *c != '\0'; c++)
			*c = (char)tolower((unsigned char)*c);
		unescape_vmx(value);
		v->key[v->count] = key;
		v->value[v->count] = value;
		v->count++;
	}
}

static const char *vmx_get(const vmx *v, const char *key)
{
	/* The last of a key is VMware's. */
	for (unsigned i = v->count; i-- > 0u;)
		if (strcasecmp(v->key[i], key) == 0)
			return v->value[i];
	return NULL;
}

static int vmx_true(const vmx *v, const char *key)
{
	const char *value = vmx_get(v, key);

	return value != NULL && (strcasecmp(value, "true") == 0 || strcmp(value, "1") == 0);
}

/* A drive at `slot`, "sata0:1" say: its image, if present. */
static void vmx_drive(machine *m, const vmx *v, const char *slot, int ide)
{
	char        key[64];
	const char *file;
	const char *type;
	char        said[4400];

	snprintf(key, sizeof key, "%s.present", slot);
	if (!vmx_true(v, key))
		return;
	snprintf(key, sizeof key, "%s.filename", slot);
	file = vmx_get(v, key);
	snprintf(key, sizeof key, "%s.devicetype", slot);
	type = vmx_get(v, key);
	if (type != NULL && (strcasecmp(type, "atapi-cdrom") == 0 || strcasecmp(type, "cdrom-raw") == 0)) {
		snprintf(said, sizeof said, "%s, the host's own drive, is not given", slot);
		note(m, said);
		return;
	}
	if (file == NULL || file[0] == '\0' || strcasecmp(file, "auto detect") == 0)
		return;
	if (type != NULL && strcasecmp(type, "cdrom-image") == 0) {
		snprintf(key, sizeof key, "%s.startconnected", slot);
		if (vmx_get(v, key) != NULL && !vmx_true(v, key)) {
			snprintf(said, sizeof said, "%s, %s, is not connected at start, and is left out",
			         slot, file);
			note(m, said);
			return;
		}
		add_medium(m, "cdrom", file, ide);
		return;
	}
	add_medium(m, "disk", file, ide);
}

static void read_vmx(machine *m, char *text)
{
	vmx        *v = calloc(1u, sizeof *v);
	const char *value;
	char        slot[32];

	if (v == NULL) {
		refuse(m, "out of memory");
		return;
	}
	parse_vmx(v, text);
	if ((value = vmx_get(v, "displayname")) != NULL)
		hostname_of(m, value);
	if ((value = vmx_get(v, "memsize")) != NULL)
		m->memory = strtoul(value, NULL, 10);
	if ((value = vmx_get(v, "numvcpus")) != NULL)
		m->cpus = (unsigned)strtoul(value, NULL, 10);
	if ((value = vmx_get(v, "firmware")) != NULL && strcasecmp(value, "efi") == 0) {
		m->uefi = 1;
		m->secure = vmx_true(v, "uefi.secureboot.enabled");
	}
	m->tpm = vmx_true(v, "vtpm.present");
	if (vmx_true(v, "ethernet0.present")) {
		m->network = 1;
		value = vmx_get(v, "ethernet0.virtualdev");
		if (value == NULL || strcasecmp(value, "e1000") == 0)
			strcpy(m->nic, "e1000");
		else if (strcasecmp(value, "e1000e") == 0)
			strcpy(m->nic, "e1000e");
		else if (strcasecmp(value, "vlance") == 0)
			strcpy(m->nic, "pcnet");
		else {
			char said[160];

			snprintf(said, sizeof said, "ethernet0, %.40s, is given as an e1000, which qemu has", value);
			note(m, said);
			strcpy(m->nic, "e1000");
		}
		value = vmx_get(v, "ethernet0.connectiontype");
		if (value != NULL && strcasecmp(value, "nat") != 0) {
			char said[160];

			snprintf(said, sizeof said, "ethernet0, %.40s, is qemu's NAT here", value);
			note(m, said);
		}
	}
	for (int i = 1; i < 10; i++) {
		snprintf(slot, sizeof slot, "ethernet%d.present", i);
		if (vmx_true(v, slot)) {
			note(m, "only ethernet0 is given a card");
			break;
		}
	}
	for (int c = 0; c < 2; c++)
		for (int u = 0; u < 2; u++) {
			snprintf(slot, sizeof slot, "ide%d:%d", c, u);
			vmx_drive(m, v, slot, 1);
		}
	for (int c = 0; c < 4; c++)
		for (int u = 0; u < 30; u++) {
			snprintf(slot, sizeof slot, "sata%d:%d", c, u);
			vmx_drive(m, v, slot, 0);
		}
	for (int c = 0; c < 4; c++)
		for (int u = 0; u < 16; u++) {
			snprintf(slot, sizeof slot, "scsi%d:%d", c, u);
			vmx_drive(m, v, slot, 0);
		}
	for (int c = 0; c < 4; c++)
		for (int u = 0; u < 16; u++) {
			snprintf(slot, sizeof slot, "nvme%d:%d", c, u);
			vmx_drive(m, v, slot, 0);
		}
	if (vmx_true(v, "scsi0.present") && m->media_count != 0u) {
		const char *kind = vmx_get(v, "scsi0.virtualdev");
		char        said[200];

		snprintf(said, sizeof said, "scsi0, %.40s, is given as SATA (AHCI): a guest with no AHCI "
		         "driver at boot will not find its disk there", kind != NULL ? kind : "lsilogic");
		note(m, said);
	}
	if (vmx_true(v, "floppy0.present") && (value = vmx_get(v, "floppy0.filename")) != NULL &&
	    (vmx_get(v, "floppy0.filetype") == NULL ||
	     strcasecmp(vmx_get(v, "floppy0.filetype"), "file") == 0))
		add_medium(m, "floppy", value, 1);
	value = vmx_get(v, "bios.bootorder");
	if (value != NULL && strncasecmp(value, "cdrom", 5u) == 0)
		m->boot_cdrom = 1;
	free(v);
}

/* --- VirtualBox's .vbox --------------------------------------------------- */

/* An element read: its name, its attributes' text, whether it closes
 * itself, whether it is a closing tag. */
typedef struct tag {
	char  name[64];
	char *attributes;
	int   closing;
	int   empty;
} tag;

/* The next element from `*at`, 1, or 0 at the end. Comments, declarations
 * and text between elements are passed over. */
static int next_tag(char **at, tag *t)
{
	char  *s = *at;
	char  *end;
	size_t n = 0u;

	for (;;) {
		if ((s = strchr(s, '<')) == NULL)
			return 0;
		if (strncmp(s, "<!--", 4u) == 0) {
			if ((s = strstr(s, "-->")) == NULL)
				return 0;
			continue;
		}
		if (s[1] == '?' || s[1] == '!') {
			s++;
			continue;
		}
		break;
	}
	s++;
	t->closing = *s == '/';
	if (t->closing)
		s++;
	while (*s != '\0' && !isspace((unsigned char)*s) && *s != '>' && *s != '/') {
		if (n + 1u < sizeof t->name)
			t->name[n++] = *s;
		s++;
	}
	t->name[n] = '\0';
	/* The tag's end, not inside a quoted value. */
	for (end = s; *end != '\0' && *end != '>'; end++)
		if (*end == '"' && (end = strchr(end + 1, '"')) == NULL)
			return 0;
	if (*end != '>')
		return 0;
	t->empty = end > s && end[-1] == '/';
	*end = '\0';
	if (t->empty)
		end[-1] = '\0';
	t->attributes = s;
	*at = end + 1;
	return 1;
}

/* The attribute `name` of a tag, unescaped, into `out`; 0 where absent. */
static int attribute(const tag *t, const char *name, char *out, size_t cap)
{
	size_t length = strlen(name);

	for (const char *s = t->attributes; (s = strstr(s, name)) != NULL; s += length) {
		const char *value;
		size_t      n = 0u;

		if ((s != t->attributes && !isspace((unsigned char)s[-1])) || s[length] != '=' ||
		    s[length + 1] != '"')
			continue;
		for (value = s + length + 2; *value != '\0' && *value != '"'; value++) {
			static const struct { const char *entity; char c; } entities[] = {
				{ "&amp;", '&' }, { "&quot;", '"' }, { "&lt;", '<' }, { "&gt;", '>' },
				{ "&apos;", '\'' },
			};
			char c = *value;

			for (size_t e = 0u; e < sizeof entities / sizeof entities[0]; e++)
				if (strncmp(value, entities[e].entity, strlen(entities[e].entity)) == 0) {
					c = entities[e].c;
					value += strlen(entities[e].entity) - 1u;
					break;
				}
			if (n + 1u < cap)
				out[n++] = c;
		}
		out[n] = '\0';
		return 1;
	}
	return 0;
}

typedef struct image {
	char uuid[64];
	char location[4096];
	int  child;      /* a differencing image, a snapshot's */
} image;

static const image *find_image(const image *images, unsigned count, const char *uuid)
{
	for (unsigned i = 0u; i < count; i++)
		if (strcasecmp(images[i].uuid, uuid) == 0)
			return &images[i];
	return NULL;
}

static void read_vbox(machine *m, char *text)
{
	image   *images = calloc(MAX_IMAGES, sizeof *images);
	unsigned image_count = 0u;
	unsigned disk_depth = 0u;    /* HardDisk elements open */
	int      in_registry = 0;
	int      in_adapter = 0;
	int      controller_ide = 0;
	int      have_controller = 0;
	char     first_boot[32] = "";
	char     value[4096];
	char    *at = text;
	tag      t;
	/* An attachment waits for its Image, the element inside it. */
	char     attached_type[16] = "";

	if (images == NULL) {
		refuse(m, "out of memory");
		return;
	}
	while (!m->failed && next_tag(&at, &t)) {
		const char *name = t.name;

		if (t.closing) {
			if (strcmp(name, "MediaRegistry") == 0)
				in_registry = 0;
			else if (strcmp(name, "HardDisk") == 0 && disk_depth > 0u)
				disk_depth--;
			else if (strcmp(name, "Adapter") == 0)
				in_adapter = 0;
			else if (strcmp(name, "AttachedDevice") == 0)
				attached_type[0] = '\0';
			continue;
		}
		if (strcmp(name, "Machine") == 0) {
			if (attribute(&t, "name", value, sizeof value))
				hostname_of(m, value);
		} else if (strcmp(name, "MediaRegistry") == 0) {
			in_registry = !t.empty;
		} else if (in_registry && (strcmp(name, "HardDisk") == 0 || strcmp(name, "Image") == 0)) {
			if (image_count < MAX_IMAGES && attribute(&t, "uuid", images[image_count].uuid,
			                                          sizeof images[image_count].uuid) &&
			    attribute(&t, "location", images[image_count].location,
			              sizeof images[image_count].location)) {
				images[image_count].child = strcmp(name, "HardDisk") == 0 && disk_depth > 0u;
				image_count++;
			}
			if (strcmp(name, "HardDisk") == 0 && !t.empty)
				disk_depth++;
		} else if (strcmp(name, "CPU") == 0) {
			m->cpus = attribute(&t, "count", value, sizeof value) ?
			          (unsigned)strtoul(value, NULL, 10) : 1u;
		} else if (strcmp(name, "Memory") == 0) {
			if (attribute(&t, "RAMSize", value, sizeof value))
				m->memory = strtoul(value, NULL, 10);
		} else if (strcmp(name, "Firmware") == 0) {
			m->uefi = attribute(&t, "type", value, sizeof value) && strncmp(value, "EFI", 3u) == 0;
		} else if (strcmp(name, "TrustedPlatformModule") == 0) {
			m->tpm = attribute(&t, "type", value, sizeof value) && strcmp(value, "v2_0") == 0;
			if (attribute(&t, "type", value, sizeof value) && strcmp(value, "v1_2") == 0)
				note(m, "its TPM 1.2 is not given; swtpm's TPM 2.0 is, under (tpm true)");
		} else if (strcmp(name, "Order") == 0) {
			if (first_boot[0] == '\0' && attribute(&t, "position", value, sizeof value) &&
			    strcmp(value, "1") == 0)
				attribute(&t, "device", first_boot, sizeof first_boot);
		} else if (strcmp(name, "Adapter") == 0) {
			in_adapter = !t.empty;
			if (attribute(&t, "enabled", value, sizeof value) && strcmp(value, "true") == 0 &&
			    attribute(&t, "slot", value, sizeof value)) {
				if (strcmp(value, "0") != 0) {
					note(m, "only the first network adapter is given a card");
				} else {
					m->network = 1;
					if (!attribute(&t, "type", value, sizeof value) || strncmp(value, "825", 3u) == 0)
						strcpy(m->nic, "e1000");
					else if (strncmp(value, "Am79C", 5u) == 0)
						strcpy(m->nic, "pcnet");
					else if (strcmp(value, "virtio") == 0)
						strcpy(m->nic, "virtio");
					else
						strcpy(m->nic, "e1000");
				}
			}
		} else if (in_adapter && m->network &&
		           (strcmp(name, "BridgedInterface") == 0 || strcmp(name, "HostOnlyInterface") == 0 ||
		            strcmp(name, "InternalNetwork") == 0)) {
			char said[120];

			snprintf(said, sizeof said, "its adapter's %s is qemu's NAT here", name);
			note(m, said);
		} else if (strcmp(name, "StorageController") == 0) {
			have_controller = 1;
			controller_ide = attribute(&t, "type", value, sizeof value) &&
			                 (strcmp(value, "PIIX3") == 0 || strcmp(value, "PIIX4") == 0 ||
			                  strcmp(value, "ICH6") == 0);
			if (attribute(&t, "type", value, sizeof value) &&
			    (strncmp(value, "Lsi", 3u) == 0 || strcmp(value, "BusLogic") == 0)) {
				char said[200];

				snprintf(said, sizeof said, "its %.40s controller is given as SATA (AHCI): a guest "
				         "with no AHCI driver at boot will not find its disk there", value);
				note(m, said);
			}
		} else if (strcmp(name, "AttachedDevice") == 0 && have_controller) {
			if (!attribute(&t, "type", attached_type, sizeof attached_type))
				attached_type[0] = '\0';
		} else if (strcmp(name, "HostDrive") == 0 && attached_type[0] != '\0') {
			note(m, "a host's own drive is not given");
		} else if (strcmp(name, "Image") == 0 && attached_type[0] != '\0') {
			const image *found;
			char         said[4400];

			if (!attribute(&t, "uuid", value, sizeof value) ||
			    (found = find_image(images, image_count, value)) == NULL) {
				refuse(m, "an attached image is not in its media registry");
				break;
			}
			if (found->child) {
				snprintf(said, sizeof said, "%s is a snapshot's differencing image, which qemu "
				         "does not read as the machine's disk: VBoxManage clonemedium makes it "
				         "a whole one", found->location);
				refuse(m, said);
				break;
			}
			add_medium(m, strcmp(attached_type, "HardDisk") == 0 ? "disk" :
			              strcmp(attached_type, "DVD") == 0 ? "cdrom" : "floppy",
			           found->location, controller_ide);
		}
	}
	if (strcmp(first_boot, "DVD") == 0)
		m->boot_cdrom = 1;
	if (m->uefi)
		note(m, "its UEFI variables, Secure Boot's among them, stay VirtualBox's: OVMF starts "
		        "with its own");
	free(images);
}

/* --- the form ------------------------------------------------------------- */

static void quoted(FILE *out, const char *text)
{
	fputc('"', out);
	for (; *text != '\0'; text++) {
		if (*text == '"' || *text == '\\')
			fputc('\\', out);
		fputc(*text, out);
	}
	fputc('"', out);
}

/* An image's path as the spec gives it: relative to the file, as the
 * file gives it, or absolute. */
static void image_source(FILE *out, const char *path)
{
	char source[4200];

	snprintf(source, sizeof source, "image:%s%s", path[0] == '/' || strncmp(path, "./", 2u) == 0 ?
	         "" : "./", path);
	quoted(out, source);
}

static char *write_form(machine *m)
{
	char   *text = NULL;
	size_t  length = 0u;
	FILE   *out = open_memstream(&text, &length);
	int     cdrom = 0;
	int     disk = 0;
	int     ide = 0;

	if (out == NULL)
		return NULL;
	for (unsigned i = 0u; i < m->media_count; i++) {
		if (strcmp(m->media[i].type, "cdrom") == 0)
			cdrom = 1;
		if (strcmp(m->media[i].type, "disk") == 0) {
			/* The first disk's controller says the machine's age. */
			if (!disk)
				ide = m->media[i].ide;
			disk = 1;
		}
	}
	fputs("(machine\n  (backend pc)\n", out);
	if (m->name[0] != '\0') {
		fputs("  (hostname ", out);
		quoted(out, m->name);
		fputs(")\n", out);
	}
	fprintf(out, "  (source \"media:%s\")\n", (m->boot_cdrom && cdrom) || !disk ? "cdrom" : "disk");
	if (m->memory != 0u)
		fprintf(out, "  (memory \"%luM\")\n", m->memory);
	if (m->cpus > 1u)
		fprintf(out, "  (cpus %u)\n", m->cpus);
	if (m->network)
		fputs("  (network nat)\n", out);
	fputs("  (media", out);
	for (unsigned i = 0u; i < m->media_count; i++) {
		fprintf(out, "\n    (medium (type %s) (source ", m->media[i].type);
		image_source(out, m->media[i].path);
		fputs("))", out);
	}
	fputs(")\n", out);
	/* A machine whose disk was IDE is an i440FX, whose IDE a guest of that
	 * age knows; one whose disk was anything later, q35, whose AHCI a
	 * modern guest has a driver for. Secure Boot is q35's alone. */
	fprintf(out, "  (tuning (pc (machine %s) (cpu max)",
	        ide && !m->secure ? "pc" : "q35");
	if (m->uefi)
		fprintf(out, " (firmware %s)", m->secure ? "uefi-secure" : "uefi");
	if (m->tpm)
		fputs(" (tpm true)", out);
	if (m->network)
		fprintf(out, " (nic %s)", m->nic);
	fputs(")))\n", out);
	if (fclose(out) != 0) {
		free(text);
		return NULL;
	}
	return text;
}

int hull_vmconf_is_file(const char *path)
{
	size_t n = strlen(path);

	return (n > 4u && strcasecmp(path + n - 4u, ".vmx") == 0) ||
	       (n > 5u && strcasecmp(path + n - 5u, ".vbox") == 0);
}

char *hull_vmconf_form(const char *path, FILE *notes)
{
	machine *m = calloc(1u, sizeof *m);
	char    *text = read_all(path);
	char    *form = NULL;
	size_t   n = strlen(path);

	if (m == NULL || text == NULL) {
		free(m);
		free(text);
		return NULL;
	}
	m->file = path;
	m->notes = notes;
	if (n > 5u && strcasecmp(path + n - 5u, ".vbox") == 0)
		read_vbox(m, text);
	else
		read_vmx(m, text);
	if (!m->failed && m->media_count == 0u)
		refuse(m, "names no disk or disc image a machine could boot from");
	if (!m->failed && m->tpm)
		note(m, "its TPM's keys stay with its own hypervisor: swtpm's TPM starts empty, so what "
		        "the guest sealed to it, BitLocker's key say, is not there");
	if (!m->failed && (form = write_form(m)) == NULL)
		fprintf(stderr, "%s: out of memory\n", path);
	free(text);
	free(m);
	return form;
}
