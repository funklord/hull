/*
 * vmconf.c -- VMware's .vmx, VirtualBox's .vbox and an appliance's .ovf
 * or .ova as a hull machine (vmconf.h, project.md section 3.13).
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
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "store.h"
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
	char        sound[8];   /* the pc's card, "" for none */
	int         tablet;     /* USB there: a tablet, the pointer where the viewer's is */
	int         boot_cdrom;
	medium      media[MAX_MEDIA];
	unsigned    media_count;
	const char *base;       /* where an .ova's files were unpacked, or NULL */
	int         direct;     /* imported: its own copies, written in place */
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
	if (m->base != NULL && path[0] != '/')
		snprintf(d->path, sizeof d->path, "%s/%s", m->base, path);
	else
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
	/* Its sound card, by the nearest qemu has: VMware's default is
	 * Ensoniq's AudioPCI, an ES1371, whose ES1370 sibling qemu makes. */
	if (vmx_true(v, "sound.present")) {
		value = vmx_get(v, "sound.virtualdev");
		strcpy(m->sound, value != NULL && strcasecmp(value, "hdaudio") == 0 ? "hda" :
		                 value != NULL && strcasecmp(value, "sb16") == 0 ? "sb16" : "es1370");
	}
	m->tablet = vmx_true(v, "usb.present");
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
		} else if (strcmp(name, "AudioAdapter") == 0) {
			/* AC'97 where the file names no controller, VirtualBox's
			 * default: Intel's 82801AA, the chip qemu's ac97 is, so the
			 * guest's driver for it is already installed. */
			if (!attribute(&t, "enabled", value, sizeof value) || strcmp(value, "false") != 0) {
				if (!attribute(&t, "controller", value, sizeof value))
					strcpy(value, "AC97");
				strcpy(m->sound, strcmp(value, "HDA") == 0 ? "hda" :
				                 strcmp(value, "SB16") == 0 ? "sb16" : "ac97");
			}
		} else if (strcmp(name, "Controller") == 0 || (strcmp(name, "HID") == 0 &&
		           attribute(&t, "Pointing", value, sizeof value) &&
		           strncmp(value, "USB", 3u) == 0)) {
			/* A USB controller, or a USB pointer: the guest drives USB,
			 * so a tablet's absolute pointer stands in for the Guest
			 * Additions' mouse integration, which qemu has none of. */
			m->tablet = 1;
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

/* --- OVF, an appliance's .ovf and the .ova around it ---------------------- */

/* A name without its namespace's prefix: rasd:ResourceType's ResourceType. */
static const char *local_name(const char *name)
{
	const char *colon = strrchr(name, ':');

	return colon != NULL ? colon + 1 : name;
}

/* An attribute by its local name, whatever prefix the file gives it. */
static int any_attribute(const tag *t, const char *name, char *out, size_t cap)
{
	static const char *const prefixes[] = { "ovf:", "vmw:", "vbox:", "" };
	char full[64];

	for (size_t i = 0u; i < sizeof prefixes / sizeof prefixes[0]; i++) {
		snprintf(full, sizeof full, "%s%s", prefixes[i], name);
		if (attribute(t, full, out, cap))
			return 1;
	}
	return 0;
}

/* The text after a tag, to the next one, its entities unescaped. */
static void element_text(const char *at, char *out, size_t cap)
{
	size_t n = 0u;

	while (isspace((unsigned char)*at))
		at++;
	for (; *at != '\0' && *at != '<'; at++) {
		char c = *at;

		if (strncmp(at, "&amp;", 5u) == 0) {
			c = '&';
			at += 4;
		} else if (strncmp(at, "&lt;", 4u) == 0) {
			c = '<';
			at += 3;
		} else if (strncmp(at, "&gt;", 4u) == 0) {
			c = '>';
			at += 3;
		} else if (strncmp(at, "&quot;", 6u) == 0) {
			c = '"';
			at += 5;
		}
		if (n + 1u < cap)
			out[n++] = c;
	}
	while (n > 0u && isspace((unsigned char)out[n - 1u]))
		n--;
	out[n] = '\0';
}

typedef struct ovf_item {
	char type[8];
	char quantity[32];
	char units[64];
	char resource[256];
	char subtype[64];
	char instance[16];
	char parent[16];
} ovf_item;

typedef struct ovf_ref {
	char id[128];
	char href[4096];
} ovf_ref;

static const char *ovf_lookup(const ovf_ref *refs, unsigned count, const char *id)
{
	for (unsigned i = 0u; i < count; i++)
		if (strcmp(refs[i].id, id) == 0)
			return refs[i].href;
	return NULL;
}

/* Whether an OVF href names a file of the package: relative, no ".."
 * among its parts, not a URL. */
static int inside_package(const char *href)
{
	const char *part = href;

	if (href[0] == '\0' || href[0] == '/' || strstr(href, "://") != NULL)
		return 0;
	while (*part != '\0') {
		size_t n = strcspn(part, "/");

		if (n == 2u && part[0] == '.' && part[1] == '.')
			return 0;
		part += n;
		part += strspn(part, "/");
	}
	return 1;
}

/* An item of the hardware section, as it closes. */
static void ovf_hardware(machine *m, const ovf_item *item, const ovf_item *controllers,
                         unsigned controller_count, const ovf_ref *files, unsigned file_count,
                         const ovf_ref *disks, unsigned disk_count)
{
	int         type = atoi(item->type);
	const char *href = NULL;
	char        said[4400];

	switch (type) {
	case 3:
		m->cpus = (unsigned)strtoul(item->quantity, NULL, 10);
		return;
	case 4: {
		unsigned long quantity = strtoul(item->quantity, NULL, 10);

		/* Megabytes unless the units say otherwise, as OVF's own
		 * examples give them: "byte * 2^20" or "MegaBytes". */
		if (strstr(item->units, "2^30") != NULL || strcasestr(item->units, "giga") != NULL)
			quantity *= 1024u;
		else if (strstr(item->units, "2^10") != NULL || strcasestr(item->units, "kilo") != NULL)
			quantity /= 1024u;
		else if (strcmp(item->units, "byte") == 0 || strcmp(item->units, "bytes") == 0)
			quantity /= 1024u * 1024u;
		m->memory = quantity;
		return;
	}
	case 10:
		if (m->network) {
			note(m, "only the first network adapter is given a card");
			return;
		}
		m->network = 1;
		if (strcasecmp(item->subtype, "PCNet32") == 0)
			strcpy(m->nic, "pcnet");
		else if (strcasecmp(item->subtype, "E1000e") == 0)
			strcpy(m->nic, "e1000e");
		else if (strcasecmp(item->subtype, "virtio") == 0)
			strcpy(m->nic, "virtio");
		else {
			strcpy(m->nic, "e1000");
			if (item->subtype[0] != '\0' && strcasecmp(item->subtype, "E1000") != 0) {
				snprintf(said, sizeof said, "its network adapter, %.40s, is given as an e1000",
				         item->subtype);
				note(m, said);
			}
		}
		return;
	case 14:
	case 15:
	case 16:
	case 17:
		break;
	default:
		return;
	}
	/* A drive: its image by the disk or file the resource names. */
	if (item->resource[0] == '\0')
		return;
	{
		const char *resource = strncmp(item->resource, "ovf:", 4u) == 0 ? item->resource + 4 :
		                       item->resource;
		const char *id = strrchr(resource, '/');

		/* A disk's id names a file's, whose href is the image; a file's
		 * id, a disc's say, names it directly. */
		if (id != NULL && strncmp(resource, "/disk/", 6u) == 0) {
			const char *file = ovf_lookup(disks, disk_count, id + 1);

			href = file != NULL ? ovf_lookup(files, file_count, file) : NULL;
		} else if (id != NULL && strncmp(resource, "/file/", 6u) == 0) {
			href = ovf_lookup(files, file_count, id + 1);
		}
	}
	if (href == NULL) {
		snprintf(said, sizeof said, "a drive names %.200s, which the appliance does not hold",
		         item->resource);
		refuse(m, said);
		return;
	}
	/* An appliance is a package handed on: its images are its own files,
	 * beside its descriptor or below it. A path out of it -- absolute, up
	 * through "..", or a URL -- would give its guest a file of the user's
	 * as a drive to read. */
	if (!inside_package(href)) {
		snprintf(said, sizeof said, "a drive's image, %.200s, is outside the appliance", href);
		refuse(m, said);
		return;
	}
	{
		int ide = 0;

		for (unsigned i = 0u; i < controller_count; i++)
			if (strcmp(controllers[i].instance, item->parent) == 0)
				ide = strcmp(controllers[i].type, "5") == 0;
		add_medium(m, type == 17 ? "disk" : type == 14 ? "floppy" : "cdrom", href, ide);
	}
}

static void read_ovf(machine *m, char *text)
{
	ovf_ref  *files = calloc(MAX_IMAGES, sizeof *files);
	ovf_ref  *disks = calloc(MAX_IMAGES, sizeof *disks);
	ovf_item *controllers = calloc(MAX_MEDIA, sizeof *controllers);
	ovf_item  item;
	unsigned  file_count = 0u;
	unsigned  disk_count = 0u;
	unsigned  controller_count = 0u;
	int       in_item = 0;
	int       in_system = 0;
	int       systems = 0;
	char      value[4096];
	char     *at = text;
	tag       t;

	if (files == NULL || disks == NULL || controllers == NULL) {
		refuse(m, "out of memory");
		goto done;
	}
	memset(&item, 0, sizeof item);
	while (!m->failed && next_tag(&at, &t)) {
		const char *name = local_name(t.name);

		if (t.closing) {
			if (in_item && (strcmp(name, "Item") == 0 || strcmp(name, "StorageItem") == 0 ||
			                strcmp(name, "EthernetPortItem") == 0)) {
				int type = atoi(item.type);

				in_item = 0;
				/* Controllers first in the section, and remembered. */
				if ((type == 5 || type == 6 || type == 20) && controller_count < MAX_MEDIA)
					controllers[controller_count++] = item;
				else
					ovf_hardware(m, &item, controllers, controller_count, files, file_count,
					             disks, disk_count);
			} else if (strcmp(name, "VirtualSystem") == 0) {
				in_system = 0;
			}
			continue;
		}
		if (strcmp(name, "File") == 0 && file_count < MAX_IMAGES) {
			if (any_attribute(&t, "id", files[file_count].id, sizeof files[file_count].id) &&
			    any_attribute(&t, "href", files[file_count].href, sizeof files[file_count].href))
				file_count++;
		} else if (strcmp(name, "Disk") == 0 && disk_count < MAX_IMAGES) {
			if (any_attribute(&t, "diskId", disks[disk_count].id, sizeof disks[disk_count].id) &&
			    any_attribute(&t, "fileRef", disks[disk_count].href, sizeof disks[disk_count].href))
				disk_count++;
		} else if (strcmp(name, "VirtualSystem") == 0) {
			if (++systems > 1) {
				note(m, "the appliance holds more than one machine, and only its first is given");
				break;
			}
			in_system = 1;
			if (any_attribute(&t, "id", value, sizeof value))
				hostname_of(m, value);
		} else if (in_system && !in_item && strcmp(name, "Name") == 0 && !t.empty) {
			element_text(at, value, sizeof value);
			if (value[0] != '\0')
				hostname_of(m, value);
		} else if (strcmp(name, "Item") == 0 || strcmp(name, "StorageItem") == 0 ||
		           strcmp(name, "EthernetPortItem") == 0) {
			in_item = !t.empty;
			memset(&item, 0, sizeof item);
		} else if (in_item && !t.empty) {
			static const struct { const char *name; size_t at; size_t cap; } fields[] = {
				{ "ResourceType", offsetof(ovf_item, type), sizeof item.type },
				{ "VirtualQuantity", offsetof(ovf_item, quantity), sizeof item.quantity },
				{ "AllocationUnits", offsetof(ovf_item, units), sizeof item.units },
				{ "HostResource", offsetof(ovf_item, resource), sizeof item.resource },
				{ "ResourceSubType", offsetof(ovf_item, subtype), sizeof item.subtype },
				{ "InstanceID", offsetof(ovf_item, instance), sizeof item.instance },
				{ "InstanceId", offsetof(ovf_item, instance), sizeof item.instance },
				{ "Parent", offsetof(ovf_item, parent), sizeof item.parent },
			};

			for (size_t i = 0u; i < sizeof fields / sizeof fields[0]; i++)
				if (strcmp(name, fields[i].name) == 0)
					element_text(at, (char *)&item + fields[i].at, fields[i].cap);
		} else if (strcmp(name, "Config") == 0 && any_attribute(&t, "key", value, sizeof value)) {
			/* VMware's extra configuration: its firmware, Secure Boot and TPM. */
			char setting[64] = "";

			any_attribute(&t, "value", setting, sizeof setting);
			if (strcmp(value, "firmware") == 0)
				m->uefi = strcmp(setting, "efi") == 0;
			else if (strcmp(value, "uefi.secureBoot.enabled") == 0)
				m->secure = strcmp(setting, "true") == 0;
		} else if (strcmp(name, "Firmware") == 0) {
			/* VirtualBox's own machine, carried inside its appliance. */
			m->uefi = attribute(&t, "type", value, sizeof value) && strncmp(value, "EFI", 3u) == 0;
		}
	}
	m->secure = m->secure && m->uefi;
done:
	free(files);
	free(disks);
	free(controllers);
}

/* Runs `argv`, its output on stderr; its status, or 1. */
static int run_tool(const char *const *argv)
{
	int   status = 1;
	pid_t pid = fork();

	if (pid == 0) {
		dup2(2, 1);
		execvp(argv[0], (char *const *)argv);
		_exit(127);
	}
	if (pid < 0 || waitpid(pid, &status, 0) < 0)
		return 1;
	return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
}

/* The file in an unpacked appliance naming what it came from. */
#define SOURCE_NAME ".hull-source"

/* An .ova unpacked once into the cache, `$XDG_CACHE_HOME/hull/ova/KEY`,
 * KEY the appliance's real path, size and mtime: a later read of the
 * same file finds it there. Into `dir`; 0, or -1 having said why. */
static int ova_cache(char *out, size_t cap);
static int only_files(int dir, unsigned depth);

static int unpack_ova(const char *path, char *dir, size_t cap)
{
	char        real[PATH_MAX];
	char        work[PATH_MAX + 64];
	char        parent[PATH_MAX];
	struct stat st;
	uint64_t    key = 1469598103934665603ull;

	if (realpath(path, real) == NULL || stat(real, &st) != 0) {
		perror(path);
		return -1;
	}
	for (const char *c = real; *c != '\0'; c++)
		key = (key ^ (unsigned char)*c) * 1099511628211ull;
	key = (key ^ (uint64_t)st.st_size) * 1099511628211ull;
	key = (key ^ (uint64_t)st.st_mtime) * 1099511628211ull;
	if (ova_cache(parent, sizeof parent) != 0) {
		fprintf(stderr, "%s: no cache to unpack the appliance into: HOME is not set\n", path);
		return -1;
	}
	if ((size_t)snprintf(dir, cap, "%s/%016llx", parent, (unsigned long long)key) >= cap) {
		fprintf(stderr, "%s: the cache's path is too long\n", path);
		return -1;
	}
	if (access(dir, F_OK) == 0)
		return 0;
	{
		char made[PATH_MAX];

		snprintf(made, sizeof made, "%s", parent);
		for (char *slash = strchr(made + 1, '/'); slash != NULL; slash = strchr(slash + 1, '/')) {
			*slash = '\0';
			mkdir(made, 0700);
			*slash = '/';
		}
		mkdir(made, 0700);
	}
	snprintf(work, sizeof work, "%s.unpacking-XXXXXX", dir);
	if (mkdtemp(work) == NULL) {
		perror(work);
		return -1;
	}
	{
		const char *const argv[] = { "tar", "-xf", real, "-C", work, "--no-same-owner", NULL };
		char              source[2 * PATH_MAX];
		FILE             *said;
		int               written;

		/* What it was unpacked from, which hull gc reads to know whether
		 * the appliance is still there as it was. */
		snprintf(source, sizeof source, "%s/" SOURCE_NAME, work);
		written = run_tool(argv) == 0;
		if (written && !only_files(open(work, O_RDONLY | O_DIRECTORY | O_CLOEXEC), 16u)) {
			fprintf(stderr, "%s: holds a link or a device among its files, which an "
			        "appliance's are not\n", path);
			hull_store_remove_tree(AT_FDCWD, work);
			return -1;
		}
		unlink(source);
		if (written && (said = fopen(source, "wxe")) != NULL) {
			written = fprintf(said, "%s\n%lld\n%lld\n", real, (long long)st.st_size,
			                  (long long)st.st_mtime) > 0;
			written = fclose(said) == 0 && written;
		} else {
			written = 0;
		}
		if (!written || rename(work, dir) != 0) {
			if (access(dir, F_OK) != 0)
				fprintf(stderr, "%s: is not an appliance's tar\n", path);
			hull_store_remove_tree(AT_FDCWD, work);
			return access(dir, F_OK) == 0 ? 0 : -1;
		}
	}
	return 0;
}

/* Whether an unpacked appliance holds only files and directories, `depth`
 * levels deep at most. A symbolic link among them would name a file of
 * the user's under a name the descriptor may give as a drive's image;
 * tar makes one wherever the archive says, so it is looked for after. */
static int only_files(int dir, unsigned depth)
{
	DIR           *list = fdopendir(dir);
	struct dirent *entry;
	int            plain = list != NULL;

	if (list == NULL) {
		close(dir);
		return 0;
	}
	while (plain && (entry = readdir(list)) != NULL) {
		struct stat st;

		if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
			continue;
		if (fstatat(dirfd(list), entry->d_name, &st, AT_SYMLINK_NOFOLLOW) != 0)
			plain = 0;
		else if (S_ISDIR(st.st_mode))
			plain = depth > 0u &&
			        only_files(openat(dirfd(list), entry->d_name,
			                          O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC),
			                   depth - 1u);
		else if (!S_ISREG(st.st_mode))
			plain = 0;
	}
	closedir(list);
	return plain;
}

/* The .ovf among an unpacked appliance's files, into `out`: 0, or -1. */
static int find_ovf(const char *dir, char *out, size_t cap)
{
	DIR           *list = opendir(dir);
	struct dirent *entry;
	int            found = 0;

	if (list == NULL)
		return -1;
	while ((entry = readdir(list)) != NULL) {
		size_t n = strlen(entry->d_name);

		if (n > 4u && strcasecmp(entry->d_name + n - 4u, ".ovf") == 0) {
			/* The least name, should there be two. */
			if (found && strcmp(strrchr(out, '/') + 1, entry->d_name) <= 0)
				continue;
			snprintf(out, cap, "%s/%s", dir, entry->d_name);
			found = 1;
		}
	}
	closedir(list);
	return found ? 0 : -1;
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
	if (m->sound[0] != '\0')
		fprintf(out, " (sound %s)", m->sound);
	if (m->tablet)
		fputs(" (pointer tablet)", out);
	fputs("))", out);
	/* An imported machine's disks are its own copies, so its changes are
	 * kept in them, as its hypervisor kept them. */
	if (m->direct)
		fputs("\n  (mutable direct)", out);
	fputs(")\n", out);
	if (fclose(out) != 0) {
		free(text);
		return NULL;
	}
	return text;
}

int hull_vmconf_is_file(const char *path)
{
	size_t n = strlen(path);

	return (n > 4u && (strcasecmp(path + n - 4u, ".vmx") == 0 ||
	                   strcasecmp(path + n - 4u, ".ovf") == 0 ||
	                   strcasecmp(path + n - 4u, ".ova") == 0)) ||
	       (n > 5u && strcasecmp(path + n - 5u, ".vbox") == 0);
}

/* The machine file at `path` read into a new machine, `unpacked` its
 * appliance's directory where it is an .ova; NULL, having said why. */
static machine *read_machine(const char *path, FILE *notes, char *unpacked)
{
	machine *m = calloc(1u, sizeof *m);
	char    *text = NULL;
	size_t   n = strlen(path);
	char     descriptor[PATH_MAX + 300];
	int      appliance = n > 4u && strcasecmp(path + n - 4u, ".ova") == 0;

	/* An appliance's files unpacked once, and its .ovf read there, its
	 * images by their paths in the cache. */
	if (appliance) {
		if (m == NULL || unpack_ova(path, unpacked, PATH_MAX) != 0) {
			free(m);
			return NULL;
		}
		if (find_ovf(unpacked, descriptor, sizeof descriptor) != 0) {
			fprintf(stderr, "%s: holds no .ovf to say what its machine is\n", path);
			free(m);
			return NULL;
		}
		m->base = unpacked;
		text = read_all(descriptor);
	} else {
		text = read_all(path);
	}
	if (m == NULL || text == NULL) {
		free(m);
		free(text);
		return NULL;
	}
	m->file = path;
	m->notes = notes;
	if (n > 5u && strcasecmp(path + n - 5u, ".vbox") == 0)
		read_vbox(m, text);
	else if (appliance || strcasecmp(path + n - 4u, ".ovf") == 0)
		read_ovf(m, text);
	else
		read_vmx(m, text);
	free(text);
	if (!m->failed && m->media_count == 0u)
		refuse(m, "names no disk or disc image a machine could boot from");
	if (!m->failed && m->tpm)
		note(m, "its TPM's keys stay with its own hypervisor: swtpm's TPM starts empty, so what "
		        "the guest sealed to it, BitLocker's key say, is not there");
	if (m->failed) {
		free(m);
		return NULL;
	}
	return m;
}

char *hull_vmconf_form(const char *path, FILE *notes)
{
	char     unpacked[PATH_MAX];
	machine *m = read_machine(path, notes, unpacked);
	char    *form;

	if (m == NULL)
		return NULL;
	if ((form = write_form(m)) == NULL)
		fprintf(stderr, "%s: out of memory\n", path);
	free(m);
	return form;
}

/* `name`'s last component with its extension replaced by `.qcow2`, unique
 * among the first `count` of `taken`, into `out`. */
static void copy_name(const char *path, char taken[][256], unsigned count, char *out, size_t cap)
{
	const char *base = strrchr(path, '/') != NULL ? strrchr(path, '/') + 1 : path;
	char        stem[200];
	char       *dot;

	snprintf(stem, sizeof stem, "%.190s", base);
	if ((dot = strrchr(stem, '.')) != NULL && dot != stem)
		*dot = '\0';
	snprintf(out, cap, "%s.qcow2", stem);
	for (unsigned n = 2u, i = 0u; i < count; i++)
		if (strcmp(taken[i], out) == 0) {
			snprintf(out, cap, "%s-%u.qcow2", stem, n++);
			i = (unsigned)-1;
		}
}

int hull_vmconf_import(const char *path, const char *dir, FILE *notes)
{
	char        unpacked[PATH_MAX];
	char        from[PATH_MAX];
	char        conf[PATH_MAX + 16];
	char        taken[MAX_MEDIA][256];
	unsigned    copies = 0u;
	machine    *m;
	char       *form = NULL;
	char       *slash;
	FILE       *out;
	int         fd;
	int         status = 1;

	/* The file's own directory, which its relative image paths are from. */
	if (realpath(path, from) == NULL) {
		perror(path);
		return 1;
	}
	if ((slash = strrchr(from, '/')) != NULL)
		*slash = '\0';
	if (mkdir(dir, 0777) != 0 && errno != EEXIST) {
		perror(dir);
		return 1;
	}
	snprintf(conf, sizeof conf, "%s/hull.conf", dir);
	if (access(conf, F_OK) == 0) {
		fprintf(stderr, "%s: a machine is there already; import into another folder\n", conf);
		return 1;
	}
	if ((m = read_machine(path, notes, unpacked)) == NULL)
		return 1;
	for (unsigned i = 0u; i < m->media_count; i++) {
		medium *d = &m->media[i];
		char    source[sizeof d->path];
		char    target[PATH_MAX + 300];

		if ((size_t)snprintf(source, sizeof source, "%s%s%s", d->path[0] == '/' ? "" : from,
		                     d->path[0] == '/' ? "" : "/", d->path) >= sizeof source) {
			fprintf(stderr, "%s: the path of %s is too long\n", path, d->path);
			goto done;
		}
		/* A disc or floppy is read, not written: it stays where it is,
		 * named by its whole path. */
		if (strcmp(d->type, "disk") != 0) {
			memcpy(d->path, source, sizeof d->path);
			continue;
		}
		copy_name(source, taken, copies, taken[copies], sizeof taken[copies]);
		snprintf(target, sizeof target, "%s/%s", dir, taken[copies]);
		fprintf(stderr, "%s: converting %s to %s\n", path, source, target);
		{
			const char *const argv[] = { "qemu-img", "convert", "-O", "qcow2", source, target,
			                             NULL };

			if (run_tool(argv) != 0) {
				fprintf(stderr, "%s: qemu-img could not convert %s\n", path, source);
				unlink(target);
				goto done;
			}
		}
		snprintf(d->path, sizeof d->path, "./%s", taken[copies]);
		copies++;
	}
	m->direct = 1;
	if ((form = write_form(m)) == NULL) {
		fprintf(stderr, "%s: out of memory\n", path);
		goto done;
	}
	if ((fd = open(conf, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0666)) < 0 ||
	    (out = fdopen(fd, "w")) == NULL) {
		perror(conf);
		goto done;
	}
	fprintf(out, "; Imported by `hull import --machine` from %s; its disks converted to\n"
	        "; qcow2 beside this file, where its changes are kept.\n", path);
	fputs(form, out);
	if (fclose(out) != 0) {
		perror(conf);
		goto done;
	}
	status = 0;
done:
	/* A failed import leaves nothing of its own in the folder. */
	for (unsigned i = 0u; status != 0 && i < copies; i++) {
		char made[PATH_MAX + 300];

		snprintf(made, sizeof made, "%s/%s", dir, taken[i]);
		unlink(made);
	}
	free(form);
	free(m);
	return status;
}

/* Where unpacked appliances are kept, into `out`; 0, or -1. */
static int ova_cache(char *out, size_t cap)
{
	const char *cache = getenv("XDG_CACHE_HOME");
	const char *home = getenv("HOME");

	if (cache != NULL && cache[0] == '/')
		return (size_t)snprintf(out, cap, "%s/hull/ova", cache) < cap ? 0 : -1;
	if (home != NULL && home[0] == '/')
		return (size_t)snprintf(out, cap, "%s/.cache/hull/ova", home) < cap ? 0 : -1;
	return -1;
}

/* Whether the appliance unpacked at `dir` is still there as it was:
 * its source file's path, size and mtime as recorded. */
static int ova_current(const char *dir)
{
	char        path[2 * PATH_MAX];
	char        real[PATH_MAX + 2];
	long long   size = -1;
	long long   mtime = -1;
	FILE       *said;
	struct stat st;
	int         current = 0;

	snprintf(path, sizeof path, "%s/" SOURCE_NAME, dir);
	if ((said = fopen(path, "re")) == NULL)
		return 0;
	if (fgets(real, sizeof real, said) != NULL && fscanf(said, "%lld %lld", &size, &mtime) == 2) {
		real[strcspn(real, "\n")] = '\0';
		current = stat(real, &st) == 0 && (long long)st.st_size == size &&
		          (long long)st.st_mtime == mtime;
	}
	fclose(said);
	return current;
}

int hull_vmconf_gc(unsigned *removed, char *why, size_t cap)
{
	char           cache[PATH_MAX];
	DIR           *list;
	struct dirent *entry;
	time_t         now = time(NULL);

	*removed = 0u;
	if (ova_cache(cache, sizeof cache) != 0 || (list = opendir(cache)) == NULL)
		return 0;
	while ((entry = readdir(list)) != NULL) {
		char        dir[PATH_MAX + 300];
		struct stat st;
		int         unpacking = strstr(entry->d_name, ".unpacking-") != NULL;

		if (entry->d_name[0] == '.')
			continue;
		snprintf(dir, sizeof dir, "%s/%s", cache, entry->d_name);
		if (lstat(dir, &st) != 0 || !S_ISDIR(st.st_mode))
			continue;
		/* One being unpacked is another reader's, unless a day old, when
		 * the reader that made it has died. */
		if (unpacking ? now - st.st_mtime < 86400 : ova_current(dir))
			continue;
		if (hull_store_remove_tree(AT_FDCWD, dir) != 0) {
			snprintf(why, cap, "%s could not be removed", dir);
			closedir(list);
			return -1;
		}
		(*removed)++;
	}
	closedir(list);
	return 0;
}
