/*
 * pc.c -- the pc class's engine: a whole PC whose operating system is
 * its own, booted by its firmware from its media (project.md section
 * 5.0). `(source "media:cdrom")` names the drive it boots; the drives
 * are the machine's (media ...). No agent runs in it, no kernel of
 * hull's and no tree of the host's, so what it can be asked is what a
 * PC can: power, its first serial port and its screen.
 *
 * qemu-system-x86_64 runs it with no defaults of its own: the first
 * serial port is the machine's output and qemu's stderr its stderr, each
 * a pipe the reader hands on, so nothing of the machine is on disk but
 * what its media are. A disk or floppy under (mutable overlay) is qemu's
 * snapshot, thrown away with it; under (mutable direct) the image is
 * written. A dir: collection is reassembled into an unnamed file, whose
 * hash media.c holds to the collection's, its writes going with the fd,
 * so a killed core leaves nothing to sweep.
 *
 * `hull exec -t ID -- console` is the serial bridge: a stream joined to
 * COM1, input written to the port and what the port says copied to it,
 * so a system with a console there can be driven with nothing of hull's
 * in the guest. It is a terminal and no more: no exit status of what is
 * typed, no sftp, no forwarding. One at a time. What the port says goes
 * through a pipe of the console's, dropped when full, for a slow client
 * must not hold the machine's own output, or its guest, back.
 *
 * The guest powering off is the machine's end, status 0, and a stop
 * pulls the plug by SIGTERM, there being no agent to ask and no telling
 * whether a 1990s system answers ACPI.
 */

#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "access.h"
#include "builtin.h"
#include "iso.h"
#include "media.h"
#include "usage.h"

#define PC_MAX_MEDIA 6u    /* four on IDE, two floppy drives */
#define PC_MAX_ARGS  64u
#define PC_MAX_SEGMENTS 4u
#define PC_MAX_PORTS 32u

typedef struct medium {
	int   type;          /* 'c' cdrom, 'd' disk, 'f' floppy */
	char *path;          /* an image: path, or NULL */
	int   fd;            /* a collection reassembled, or -1 */
	char  format[8];     /* qemu's word for an image's container */
} medium;

struct hull_stream {
	hull_machine *machine;
	hull_core    *core;
	int           from;     /* the console's end of its pipe */
	int           to;       /* the reader's, or -1 once the console has ended */
	pthread_t     thread;
};

struct hull_machine {
	hull_core      *core;
	medium          media[PC_MAX_MEDIA];
	unsigned        media_count;
	char            boot;           /* -boot's letter */
	int             direct;         /* (mutable direct) */
	char           *memory;         /* (memory ...), or NULL for qemu's own */
	char           *cpus;
	char           *machine_type;
	char           *vga;
	char           *accel;
	char           *firmware;       /* OVMF's path, for (firmware uefi), or NULL for SeaBIOS */
	char           *nic;            /* (tuning (pc (nic ...))), or NULL for ne2k_pci */
	char           *name;
	int             network;        /* (network nat) */
	char            address[16];    /* the machine's, where its ports are */
	unsigned        port_host[PC_MAX_PORTS];
	unsigned        port_guest[PC_MAX_PORTS];
	int             port_udp[PC_MAX_PORTS];
	unsigned        port_count;
	char           *segment[PC_MAX_SEGMENTS];  /* Appendix D's wires, by name */
	int             segment_fd[PC_MAX_SEGMENTS];
	unsigned        segment_count;
	char           *display;        /* the run's X display, or NULL */
	char           *xauthority;
	int             vnc;            /* (display vnc): the screen at the machine's address */
	char            cache[4096];    /* where qemu's snapshots and made images are */
	pid_t           pid;
	int             out;            /* the first serial port */
	int             err;            /* qemu's stderr */
	int             input;          /* COM1's input: qemu's stdin, a socket */
	hull_stream    *console;        /* the one joined to COM1, or NULL */
	pthread_t       reader;
	int             reading;
	pthread_mutex_t lock;
	pthread_cond_t  done_cond;
	int             done;
	int             paused;
};

static const hull_host_ops *host;

static hull_status fail(hull_error *error, hull_status code, const char *detail)
{
	if (error != NULL) {
		error->code           = code;
		error->detail         = detail;
		error->runtime_detail = NULL;
	}
	return code;
}

static void release(hull_machine *m)
{
	for (unsigned i = 0u; i < m->media_count; i++) {
		free(m->media[i].path);
		if (m->media[i].fd >= 0)
			close(m->media[i].fd);
	}
	for (unsigned i = 0u; i < m->segment_count; i++) {
		free(m->segment[i]);
		if (m->segment_fd[i] >= 0)
			close(m->segment_fd[i]);
	}
	free(m->memory);
	free(m->cpus);
	free(m->machine_type);
	free(m->vga);
	free(m->accel);
	free(m->firmware);
	free(m->nic);
	free(m->name);
	free(m->display);
	free(m->xauthority);
	if (m->out >= 0)
		close(m->out);
	if (m->err >= 0)
		close(m->err);
	if (m->input >= 0)
		close(m->input);
	pthread_mutex_destroy(&m->lock);
	pthread_cond_destroy(&m->done_cond);
	free(m);
}

/* $XDG_CACHE_HOME/hull, or ~/.cache/hull, made: on disk, since a machine's
 * disks are too large for the runtime directory's memory. */
static const char *make_cache(hull_machine *m)
{
	const char *cache = getenv("XDG_CACHE_HOME");
	const char *home = getenv("HOME");
	char        parent[4000];

	if (cache != NULL && cache[0] != '\0')
		snprintf(parent, sizeof parent, "%s", cache);
	else if (home != NULL && home[0] != '\0')
		snprintf(parent, sizeof parent, "%s/.cache", home);
	else
		return "neither XDG_CACHE_HOME nor HOME names a place for the machine's disks";
	snprintf(m->cache, sizeof m->cache, "%s/hull", parent);
	if ((mkdir(parent, 0700) != 0 && errno != EEXIST) ||
	    (mkdir(m->cache, 0700) != 0 && errno != EEXIST))
		return "the cache directory for the machine's disks cannot be made";
	return NULL;
}

/* The container an image is by its first bytes, for qemu to read it by:
 * raw unless it says it is one of these. Named rather than left to qemu's
 * probing, which would take a raw disk whose guest wrote such bytes for
 * one. */
static void read_format(medium *d)
{
	static const char vdi[4] = { 0x7f, 0x10, (char)0xda, (char)0xbe };
	char buffer[72] = { 0 };
	int  fd = open(d->path, O_RDONLY | O_CLOEXEC);

	strcpy(d->format, "raw");
	if (fd < 0)
		return;
	if (read(fd, buffer, sizeof buffer) == (ssize_t)sizeof buffer) {
		if (memcmp(buffer, "QFI\xfb", 4u) == 0)
			strcpy(d->format, "qcow2");
		else if (memcmp(buffer, "KDMV", 4u) == 0)
			strcpy(d->format, "vmdk");
		else if (memcmp(buffer, "conectix", 8u) == 0)
			strcpy(d->format, "vpc");
		else if (memcmp(buffer, "vhdxfile", 8u) == 0)
			strcpy(d->format, "vhdx");
		else if (memcmp(buffer + 64, vdi, sizeof vdi) == 0)
			strcpy(d->format, "vdi");
	}
	close(fd);
}

/* A collection reassembled into an unnamed file of the cache's: NULL, or
 * why not. A cdrom's may be a tree alone, an ISO then made of it. */
static const char *make_image(hull_machine *m, medium *d, const char *collection)
{
	static char why[700];
	char        detail[512];
	char        files[4200];
	struct stat st;
	int         exact = hull_media_exact(collection);

	if (!exact && d->type != 'c')
		return "a floppy's or disk's collection is one `hull media import` made of its image";
	snprintf(files, sizeof files, "%s/files", collection);
	if (stat(files, &st) != 0 || !S_ISDIR(st.st_mode))
		return "a medium is not there: a dir: collection holds its disc's tree in files/";
	if (m->cache[0] == '\0') {
		const char *wrong = make_cache(m);

		if (wrong != NULL)
			return wrong;
	}
	if ((d->fd = open(m->cache, O_TMPFILE | O_RDWR | O_CLOEXEC, 0600)) < 0)
		return "an image cannot be made in the cache directory";
	if ((exact ? hull_media_write(collection, d->fd, detail, sizeof detail) :
	     hull_iso_write(files, "", d->fd, detail, sizeof detail)) != 0) {
		snprintf(why, sizeof why, "a medium's image cannot be made: %s", detail);
		return why;
	}
	strcpy(d->format, "raw");
	return NULL;
}

/* A medium of `type` from the image file `path`, next in the list: NULL,
 * or why not. */
static const char *add_image(hull_machine *m, int type, const char *path)
{
	medium     *d = &m->media[m->media_count];
	struct stat st;

	if (m->media_count == PC_MAX_MEDIA)
		return "there are too many media: four on IDE and two floppy drives are a PC's";
	if (stat(path, &st) != 0 || !S_ISREG(st.st_mode))
		return "a medium's image is not there";
	d->type = type;
	d->fd   = -1;
	if ((d->path = realpath(path, NULL)) == NULL)
		return "out of memory";
	m->media_count++;
	read_format(d);
	return NULL;
}

/* `(media (medium (type cdrom) (source ...)))`: NULL, or why it cannot be
 * had. */
static const char *read_media(hull_machine *m, const hull_sexpr_doc *doc,
                              const hull_sexpr_form *machine)
{
	hull_sexpr_form entry;
	hull_sexpr_form item;

	if (!hull_access_find(doc, machine, "media", &entry))
		return NULL;
	hull_sexpr_child(doc, &entry, &item);
	while (hull_sexpr_next(doc, &item, &item)) {
		char       *type = hull_access_value(doc, &item, "type");
		char       *source = hull_access_value(doc, &item, "source");
		int         image = source != NULL && strncmp(source, "image:", 6u) == 0;
		int         dir = source != NULL && strncmp(source, "dir:", 4u) == 0;
		char       *path = image || dir ? hull_access_path(doc, source + (image ? 6 : 4)) : NULL;
		medium     *d = &m->media[m->media_count];
		int         kind = type == NULL ? 0 : strcmp(type, "cdrom") == 0 ? 'c' :
		                   strcmp(type, "disk") == 0 ? 'd' : 'f';
		const char *wrong = NULL;

		if (path == NULL)
			wrong = "a pc machine's medium is a dir: collection or an image:";
		else if (image)
			wrong = add_image(m, kind, path);
		else if (m->media_count == PC_MAX_MEDIA)
			wrong = "there are too many media: four on IDE and two floppy drives are a PC's";
		else if (kind != 'c' && m->direct)
			wrong = "(mutable direct) writes an image: a disk or floppy made of a collection "
			        "has no file to write, its writes going with the machine";
		else {
			d->type = kind;
			d->fd   = -1;
			m->media_count++;
			wrong = make_image(m, d, path);
		}
		free(type);
		free(source);
		free(path);
		if (wrong != NULL)
			return wrong;
	}
	return NULL;
}

/* (firmware uefi): OVMF where a distribution puts it, as a read-only
 * image with no variable store: a machine's boot entries do not outlive
 * it, and a system finds its loader at the default path. */
static const char *read_firmware(hull_machine *m, const hull_sexpr_doc *doc,
                                 const hull_sexpr_form *pc)
{
	static const char *const places[] = { "/usr/share/ovmf/OVMF.fd", "/usr/share/qemu/OVMF.fd",
	                                      "/usr/share/OVMF/OVMF.fd", NULL };
	char *firmware = hull_access_value(doc, pc, "firmware");
	int   uefi = firmware != NULL && strcmp(firmware, "uefi") == 0;

	free(firmware);
	if (!uefi)
		return NULL;
	for (const char *const *place = places; *place != NULL; place++)
		if (access(*place, R_OK) == 0) {
			m->firmware = strdup(*place);
			return m->firmware != NULL ? NULL : "out of memory";
		}
	return "(firmware uefi) needs OVMF, and none is installed: Debian's package is ovmf";
}

/* (network nat) and the ports on the machine's address: NULL, or why it
 * cannot be had. */
static const char *read_network(hull_machine *m, const hull_sexpr_doc *doc,
                                const hull_sexpr_form *machine)
{
	hull_sexpr_form top;
	hull_sexpr_form entry;
	hull_sexpr_form item;
	char           *network = hull_access_value(doc, machine, "network");
	char           *address = NULL;

	m->network = network != NULL && strcmp(network, "nat") == 0;
	free(network);
	if (hull_sexpr_first(doc, &top))
		address = hull_access_value(doc, &top, "address");
	if (address != NULL && strlen(address) < sizeof m->address)
		strcpy(m->address, address);
	free(address);
	if (hull_access_find(doc, machine, "segments", &entry)) {
		hull_sexpr_child(doc, &entry, &item);
		while (hull_sexpr_next(doc, &item, &item)) {
			if (m->segment_count == PC_MAX_SEGMENTS)
				return "there are too many segments";
			m->segment_fd[m->segment_count] = -1;
			if ((m->segment[m->segment_count++] = hull_access_segment_name(doc, &item)) == NULL)
				return "a segment's name is not text";
		}
		if (m->address[0] == '\0')
			return "the plan gives its segments no address to draw a MAC from";
	}
	if (!hull_access_find(doc, machine, "ports", &entry))
		return NULL;
	if (!m->network)
		return "a pc machine's ports need its network: (network nat)";
	if (m->address[0] == '\0')
		return "the plan gives its ports no address";
	hull_sexpr_child(doc, &entry, &item);
	while (hull_sexpr_next(doc, &item, &item)) {
		char         *host_port = hull_access_value(doc, &item, "host");
		char         *guest     = hull_access_value(doc, &item, "guest");
		char         *proto     = hull_access_value(doc, &item, "proto");
		unsigned long h = host_port != NULL ? strtoul(host_port, NULL, 10) : 0u;
		unsigned long g = guest != NULL ? strtoul(guest, NULL, 10) : 0u;
		int           udp = proto != NULL && strcmp(proto, "udp") == 0;

		free(host_port);
		free(guest);
		free(proto);
		if (m->port_count == PC_MAX_PORTS)
			return "there are too many ports";
		if (h < 1u || h > 65535u || g < 1u || g > 65535u)
			return "a port is a host and a guest port of 1 to 65535";
		m->port_udp[m->port_count]     = udp;
		m->port_host[m->port_count]    = (unsigned)h;
		m->port_guest[m->port_count++] = (unsigned)g;
	}
	return NULL;
}

/* What the plan asks, read into `m`: NULL, or why it cannot be run. */
static const char *read_plan(hull_machine *m, const hull_sexpr_doc *doc,
                             const hull_sexpr_form *machine)
{
	hull_sexpr_form entry;
	hull_sexpr_form top;
	const char     *wrong;
	char           *source = hull_access_value(doc, machine, "source");
	char           *mutable_text = hull_access_value(doc, machine, "mutable");
	char           *display = hull_access_value(doc, machine, "display");
	int             wanted = 0;

	m->direct = mutable_text != NULL && strcmp(mutable_text, "direct") == 0;
	free(mutable_text);
	if (source != NULL && strncmp(source, "media:", 6u) == 0)
		wanted = strcmp(source + 6, "cdrom") == 0 ? 'c' : strcmp(source + 6, "disk") == 0 ? 'd' :
		         strcmp(source + 6, "floppy") == 0 ? 'f' : 0;
	else if (source != NULL && strncmp(source, "image:", 6u) == 0)
		wanted = 'd';
	m->boot = wanted == 'c' ? 'd' : wanted == 'd' ? 'c' : 'a';
	if (wanted == 0) {
		free(source);
		free(display);
		return "a pc machine's source is the drive it boots: \"media:cdrom\", \"media:disk\" or "
		       "\"media:floppy\"; or an \"image:\" of a disk";
	}
	if (display != NULL && strcmp(display, "window") == 0) {
		hull_sexpr_form screen;

		if (!hull_sexpr_first(doc, &top) || !hull_access_find(doc, &top, "screen", &screen) ||
		    (m->display = hull_access_value(doc, &screen, "display")) == NULL) {
			free(source);
			free(display);
			return "(display window) needs an X display, and the run gave none: DISPLAY is not "
			       "set where hull ran";
		}
		m->xauthority = hull_access_value(doc, &screen, "xauthority");
	}
	m->vnc = display != NULL && strcmp(display, "vnc") == 0;
	free(display);
	/* An image: source is the first disk, before any (media ...). */
	if (source[0] == 'i') {
		char *path = hull_access_path(doc, source + 6);

		wrong = path != NULL ? add_image(m, 'd', path) : "out of memory";
		free(path);
		if (wrong != NULL) {
			free(source);
			return wrong;
		}
	}
	free(source);
	if ((wrong = read_network(m, doc, machine)) != NULL)
		return wrong;
	m->memory = hull_access_value(doc, machine, "memory");
	m->cpus   = hull_access_value(doc, machine, "cpus");
	m->name   = hull_access_value(doc, machine, "hostname");
	if (hull_access_find(doc, machine, "tuning", &entry)) {
		hull_sexpr_form pc;

		if (hull_access_find(doc, &entry, "pc", &pc)) {
			m->machine_type = hull_access_value(doc, &pc, "machine");
			m->vga          = hull_access_value(doc, &pc, "vga");
			m->accel        = hull_access_value(doc, &pc, "accel");
			m->nic          = hull_access_value(doc, &pc, "nic");
			if ((wrong = read_firmware(m, doc, &pc)) != NULL)
				return wrong;
		}
	}
	if (m->vnc && m->address[0] == '\0')
		return "the plan gives (display vnc) no address";
	if ((wrong = read_media(m, doc, machine)) != NULL)
		return wrong;
	for (unsigned i = 0u; i < m->media_count; i++)
		if (m->media[i].type == wanted)
			return NULL;
	return "the drive the machine boots has no medium in (media ...)";
}

/* `text`, a path, with its commas doubled as qemu's options ask, after
 * `prefix`, in a new string. */
static char *option(const char *prefix, const char *text)
{
	size_t n = strlen(prefix) + 2u * strlen(text) + 1u;
	char  *out = malloc(n);
	char  *at = out;

	if (out == NULL)
		return NULL;
	at = stpcpy(at, prefix);
	for (; *text != '\0'; text++) {
		*at++ = *text;
		if (*text == ',')
			*at++ = ',';
	}
	*at = '\0';
	return out;
}

/* In the child: the drives and the rest, and exec. Returns to fail. */
static void exec_qemu(const hull_machine *m)
{
	char    *argv[PC_MAX_ARGS];
	char     machine[128];
	char     smp[64];
	char     vnc[48];
	unsigned n = 0u;
	unsigned ide = 0u;
	unsigned floppy = 0u;
	char     boot[16];
	const char *accel = m->accel != NULL ? m->accel : "auto";

	if (strcmp(accel, "auto") == 0)
		accel = access("/dev/kvm", R_OK | W_OK) == 0 ? "kvm" : "tcg";
	snprintf(machine, sizeof machine, "%s,accel=%s", m->machine_type != NULL ? m->machine_type : "pc",
	         accel);
	snprintf(boot, sizeof boot, "order=%c", m->boot);
	argv[n++] = "qemu-system-x86_64";
	argv[n++] = "-nodefaults";
	argv[n++] = "-no-user-config";
	argv[n++] = "-machine";
	argv[n++] = machine;
	argv[n++] = "-serial";
	argv[n++] = "stdio";
	argv[n++] = "-vga";
	argv[n++] = m->vga != NULL ? m->vga : "std";
	/* VNC's display 0 is port 5900, at the machine's own address. */
	snprintf(vnc, sizeof vnc, "vnc=%s:0", m->address);
	argv[n++] = "-display";
	argv[n++] = m->vnc ? vnc : m->display != NULL ? "sdl" : "none";
	argv[n++] = "-boot";
	argv[n++] = boot;
	if (m->firmware != NULL) {
		argv[n++] = "-bios";
		argv[n++] = m->firmware;
	}
	if (m->name != NULL) {
		argv[n++] = "-name";
		argv[n++] = option("", m->name);
	}
	if (m->memory != NULL) {
		argv[n++] = "-m";
		argv[n++] = m->memory;
	}
	if (m->cpus != NULL) {
		snprintf(smp, sizeof smp, "%s", m->cpus);
		argv[n++] = "-smp";
		argv[n++] = smp;
	}
	if (m->network) {
		size_t cap = 96u + (size_t)m->port_count * 64u;
		char  *nic = malloc(cap);
		size_t at;

		if (nic == NULL)
			return;
		at = (size_t)snprintf(nic, cap, "user,model=%s", m->nic != NULL ? m->nic : "ne2k_pci");
		for (unsigned i = 0u; i < m->port_count; i++)
			at += (size_t)snprintf(nic + at, cap - at, ",hostfwd=%s:%s:%u-:%u",
			                       m->port_udp[i] ? "udp" : "tcp", m->address, m->port_host[i],
			                       m->port_guest[i]);
		argv[n++] = "-nic";
		argv[n++] = nic;
	}
	/* A card on each wire, the machine's own to address. */
	for (unsigned i = 0u; i < m->segment_count; i++) {
		unsigned char mac[6];
		char         *netdev = malloc(64u);
		char         *device = malloc(96u);
		int           fd = fcntl(m->segment_fd[i], F_DUPFD, 3);

		if (netdev == NULL || device == NULL || fd < 0)
			return;
		hull_access_segment_mac(m->address, m->segment[i], i, mac);
		snprintf(netdev, 64u, "stream,id=s%u,addr.type=fd,addr.str=%d", i, fd);
		snprintf(device, 96u, "%s,netdev=s%u,mac=%02x:%02x:%02x:%02x:%02x:%02x",
		         m->nic == NULL ? "ne2k_pci" : strcmp(m->nic, "virtio") == 0 ? "virtio-net-pci" :
		         m->nic, i, mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
		argv[n++] = "-netdev";
		argv[n++] = netdev;
		argv[n++] = "-device";
		argv[n++] = device;
	}
	for (unsigned i = 0u; i < m->media_count; i++) {
		const medium *d = &m->media[i];
		char          file[64];
		char          head[160];
		const char   *path = d->path;
		char         *drive;

		/* A made image is ours and its writes go with it; an image:
		 * disk or floppy is the user's, kept under (mutable overlay)
		 * by a snapshot. */
		if (d->fd >= 0) {
			int fd = fcntl(d->fd, F_DUPFD, 3);

			if (fd < 0)
				return;
			snprintf(file, sizeof file, "/proc/self/fd/%d", fd);
			path = file;
		}
		snprintf(head, sizeof head, "%s,format=%s%s%s,file=",
		         d->type == 'f' ? "if=floppy" : d->type == 'c' ? "if=ide,media=cdrom" :
		         "if=ide,media=disk", d->path == NULL ? "raw" : d->format,
		         d->type == 'c' ? ",readonly=on" : "",
		         d->type != 'c' && d->path != NULL && !m->direct ? ",snapshot=on" : "");
		if ((drive = option(head, path)) == NULL)
			return;
		{
			size_t length = strlen(drive);
			char  *indexed = malloc(length + 16u);

			if (indexed == NULL)
				return;
			sprintf(indexed, "%s,index=%u", drive, d->type == 'f' ? floppy++ : ide++);
			argv[n++] = "-drive";
			argv[n++] = indexed;
		}
	}
	argv[n] = NULL;
	if (m->display != NULL) {
		setenv("DISPLAY", m->display, 1);
		if (m->xauthority != NULL)
			setenv("XAUTHORITY", m->xauthority, 1);
		/* What SDL and Mesa would otherwise leave behind them. */
		setenv("DBUS_SESSION_BUS_ADDRESS", "disabled:", 1);
		setenv("MESA_SHADER_CACHE_DISABLE", "true", 1);
	}
	setenv("HOME", "/nonexistent", 1);
	/* qemu's snapshots go here, unnamed once open. */
	if (m->cache[0] != '\0')
		setenv("TMPDIR", m->cache, 1);
	execvp(argv[0], argv);
}

/* What COM1 said, to the console where one is joined. */
static void to_console(hull_machine *m, const uint8_t *data, size_t n)
{
	pthread_mutex_lock(&m->lock);
	if (m->console != NULL && m->console->to >= 0 && write(m->console->to, data, n) < 0) {
		/* Full: a console's reader behind, and what it missed is gone. */
	}
	pthread_mutex_unlock(&m->lock);
}

/* The console ended, its thread reading to the pipe's end. */
static void end_console(hull_machine *m)
{
	pthread_mutex_lock(&m->lock);
	if (m->console != NULL && m->console->to >= 0) {
		close(m->console->to);
		m->console->to = -1;
	}
	pthread_mutex_unlock(&m->lock);
}

/* The two pipes handed on until both end, then the end said. */
static void *read_machine(void *arg)
{
	hull_machine *m = arg;
	struct pollfd fds[2] = { { m->out, POLLIN, 0 }, { m->err, POLLIN, 0 } };
	uint8_t       buffer[65536];
	int           status = 0;
	int           open_fds = 2;

	while (open_fds > 0) {
		if (poll(fds, 2u, -1) < 0) {
			if (errno == EINTR)
				continue;
			break;
		}
		for (unsigned i = 0u; i < 2u; i++) {
			ssize_t n;

			if (fds[i].fd < 0 || fds[i].revents == 0)
				continue;
			n = read(fds[i].fd, buffer, sizeof buffer);
			if (n > 0) {
				host->on_data(m->core, NULL, (int)i + 1, buffer, (size_t)n);
				if (i == 0u)
					to_console(m, buffer, (size_t)n);
			} else if (n == 0 || errno != EINTR) {
				fds[i].fd = -1;
				open_fds--;
			}
		}
	}
	while (waitpid(m->pid, &status, 0) < 0 && errno == EINTR)
		;
	pthread_mutex_lock(&m->lock);
	m->done = 1;
	pthread_cond_broadcast(&m->done_cond);
	pthread_mutex_unlock(&m->lock);
	end_console(m);
	if (WIFSIGNALED(status))
		host->on_exit(m->core, m, NULL, -1, WTERMSIG(status));
	else
		host->on_exit(m->core, m, NULL, WEXITSTATUS(status), 0);
	host->on_state(m->core, m, "stopped");
	return NULL;
}

static hull_status pc_create(hull_core *core, hull_buf plan, hull_machine **out, hull_error *error)
{
	static char     said[512];
	hull_sexpr_doc  doc;
	hull_sexpr_form machine;
	hull_machine   *m;
	const char     *reason;

	*out = NULL;
	if (!hull_access_machine(&doc, plan.data, plan.len, &machine))
		return fail(error, HULL_ERR_UNSUPPORTED, "the plan is not canonical");
	if ((m = calloc(1u, sizeof *m)) == NULL)
		return fail(error, HULL_ERR_INTERNAL, "out of memory");
	m->core = core;
	m->pid  = -1;
	m->out  = -1;
	m->err  = -1;
	m->input = -1;
	pthread_mutex_init(&m->lock, NULL);
	pthread_cond_init(&m->done_cond, NULL);
	if ((reason = read_plan(m, &doc, &machine)) == NULL && m->cache[0] == '\0' && !m->direct)
		reason = make_cache(m);
	if (reason != NULL) {
		/* The detail outlives the machine: the core reads it after. */
		snprintf(said, sizeof said, "%s", reason);
		release(m);
		return fail(error, HULL_ERR_RUNTIME, said);
	}
	*out = m;
	return HULL_OK;
}

static hull_status pc_start(hull_core *core, hull_machine *m, hull_error *error)
{
	int   out[2];
	int   err[2];
	int   in[2];
	pid_t parent = getpid();

	(void)core;
	/* A port on each wire, from the core's switch. */
	for (unsigned i = 0u; i < m->segment_count; i++) {
		static char detail[256];

		if (host->struct_size < offsetof(hull_host_ops, segment) + sizeof host->segment ||
		    host->segment == NULL)
			return fail(error, HULL_ERR_RUNTIME, "the core has no switch for a segment");
		m->segment_fd[i] = host->segment(m->core, m->segment[i], detail, sizeof detail);
		if (m->segment_fd[i] < 0)
			return fail(error, HULL_ERR_RUNTIME, detail);
	}
	if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, in) != 0)
		return fail(error, HULL_ERR_RUNTIME, "a socket for the machine could not be made");
	if (pipe2(out, O_CLOEXEC) != 0) {
		close(in[0]);
		close(in[1]);
		return fail(error, HULL_ERR_RUNTIME, "a pipe for the machine could not be made");
	}
	if (pipe2(err, O_CLOEXEC) != 0) {
		close(in[0]);
		close(in[1]);
		close(out[0]);
		close(out[1]);
		return fail(error, HULL_ERR_RUNTIME, "a pipe for the machine could not be made");
	}
	m->pid = fork();
	if (m->pid < 0) {
		close(in[0]);
		close(in[1]);
		close(out[0]);
		close(out[1]);
		close(err[0]);
		close(err[1]);
		return fail(error, HULL_ERR_RUNTIME, "qemu could not be started");
	}
	if (m->pid == 0) {
		if (dup2(in[1], 0) < 0 || dup2(out[1], 1) < 0 || dup2(err[1], 2) < 0 ||
		    setsid() < 0 || prctl(PR_SET_PDEATHSIG, SIGKILL, 0L, 0L, 0L) != 0 ||
		    getppid() != parent || prctl(PR_SET_NO_NEW_PRIVS, 1L, 0L, 0L, 0L) != 0)
			_exit(126);
		/* Nothing of the core's goes on; exec_qemu gives each made image
		 * a descriptor of its own that does. */
		close_range(3u, ~0u, CLOSE_RANGE_CLOEXEC);
		exec_qemu(m);
		dprintf(2, "qemu-system-x86_64, which a pc machine needs: %s\n", strerror(errno));
		_exit(127);
	}
	close(in[1]);
	close(out[1]);
	close(err[1]);
	m->input = in[0];
	for (unsigned i = 0u; i < m->segment_count; i++) {
		close(m->segment_fd[i]);
		m->segment_fd[i] = -1;
	}
	m->out = out[0];
	m->err = err[0];
	if (pthread_create(&m->reader, NULL, read_machine, m) != 0) {
		kill(m->pid, SIGKILL);
		waitpid(m->pid, NULL, 0);
		return fail(error, HULL_ERR_INTERNAL, "the reader thread could not start");
	}
	m->reading = 1;
	host->on_state(m->core, m, "running");
	return HULL_OK;
}

/* Until qemu is reaped or `ms` pass: 1 if it was. */
static int wait_done(hull_machine *m, int ms)
{
	struct timespec until;
	int             done;

	clock_gettime(CLOCK_REALTIME, &until);
	until.tv_sec  += ms / 1000;
	until.tv_nsec += (long)(ms % 1000) * 1000000L;
	if (until.tv_nsec >= 1000000000L) {
		until.tv_sec++;
		until.tv_nsec -= 1000000000L;
	}
	pthread_mutex_lock(&m->lock);
	while (!m->done && pthread_cond_timedwait(&m->done_cond, &m->lock, &until) == 0)
		;
	done = m->done;
	pthread_mutex_unlock(&m->lock);
	return done;
}

/* TERM, then KILL after `timeout_ms`. The reader reaps it, so the pid is
 * signalled only while it is not done, under the lock. */
static hull_status pc_stop(hull_core *core, hull_machine *m, int timeout_ms, hull_error *error)
{
	(void)core;
	(void)error;
	pthread_mutex_lock(&m->lock);
	if (!m->done && m->pid > 0) {
		if (m->paused)
			kill(m->pid, SIGCONT);
		kill(m->pid, SIGTERM);
	}
	pthread_mutex_unlock(&m->lock);
	if (!wait_done(m, timeout_ms > 0 ? timeout_ms : 1)) {
		pthread_mutex_lock(&m->lock);
		if (!m->done && m->pid > 0)
			kill(m->pid, SIGKILL);
		pthread_mutex_unlock(&m->lock);
		wait_done(m, 5000);
	}
	return HULL_OK;
}

static hull_status pc_destroy(hull_core *core, hull_machine *m, hull_error *error)
{
	if (m == NULL)
		return HULL_OK;
	if (m->reading) {
		pc_stop(core, m, 0, error);
		pthread_join(m->reader, NULL);
	} else if (m->pid > 0) {
		kill(m->pid, SIGKILL);
		waitpid(m->pid, NULL, 0);
	}
	release(m);
	return HULL_OK;
}

static hull_status pc_signal(hull_core *core, hull_machine *m, hull_stream *stream, int signum,
                             hull_error *error)
{
	(void)core;
	(void)error;
	/* A signal to the console detaches it; the machine goes on. */
	if (stream != NULL) {
		end_console(m);
		return HULL_OK;
	}
	pthread_mutex_lock(&m->lock);
	if (!m->done && m->pid > 0)
		kill(m->pid, signum);
	pthread_mutex_unlock(&m->lock);
	return HULL_OK;
}

/* The console's thread: the pipe, to its end, then the stream's end. */
static void *deliver_console(void *arg)
{
	hull_stream *st = arg;
	uint8_t      buffer[4096];
	ssize_t      n;

	while ((n = read(st->from, buffer, sizeof buffer)) > 0 || (n < 0 && errno == EINTR))
		if (n > 0)
			host->on_data(st->core, st, 1, buffer, (size_t)n);
	host->on_exit(st->core, st->machine, st, 0, 0);
	return NULL;
}

static hull_status pc_exec(hull_core *core, hull_machine *m, hull_buf spec, hull_stream **out,
                           hull_error *error)
{
	hull_sexpr_doc  doc;
	hull_sexpr_form message;
	hull_sexpr_form argv;
	hull_sexpr_form word;
	hull_stream    *st;
	char           *first = NULL;
	int             ends[2];
	int             asked;

	if (hull_sexpr_open(&doc, spec.data, spec.len, HULL_SEXPR_CANONICAL, NULL) == HULL_SEXPR_OK &&
	    hull_sexpr_first(&doc, &message) && hull_access_find(&doc, &message, "argv", &argv)) {
		hull_sexpr_child(&doc, &argv, &word);
		if (hull_sexpr_next(&doc, &word, &word))
			first = hull_access_text(&doc, &word);
	}
	asked = first != NULL && strcmp(first, "console") == 0;
	free(first);
	if (!asked)
		return fail(error, HULL_ERR_UNSUPPORTED, "nothing of hull's runs in a pc machine to run a "
		            "command: `console` is its serial port, the one thing there is");
	if ((st = calloc(1u, sizeof *st)) == NULL)
		return fail(error, HULL_ERR_INTERNAL, "out of memory");
	if (pipe2(ends, O_CLOEXEC) != 0 || fcntl(ends[1], F_SETFL, O_NONBLOCK) != 0) {
		free(st);
		return fail(error, HULL_ERR_RUNTIME, "a pipe for the console could not be made");
	}
	st->machine = m;
	st->core    = core;
	st->from    = ends[0];
	st->to      = ends[1];
	pthread_mutex_lock(&m->lock);
	if (m->done || m->console != NULL) {
		int gone = m->done;

		pthread_mutex_unlock(&m->lock);
		close(ends[0]);
		close(ends[1]);
		free(st);
		return gone ? fail(error, HULL_ERR_GONE, "the machine is not running") :
		       fail(error, HULL_ERR_BUSY, "the serial port has a console joined to it already");
	}
	m->console = st;
	pthread_mutex_unlock(&m->lock);
	if (pthread_create(&st->thread, NULL, deliver_console, st) != 0) {
		end_console(m);
		pthread_mutex_lock(&m->lock);
		m->console = NULL;
		pthread_mutex_unlock(&m->lock);
		close(ends[0]);
		free(st);
		return fail(error, HULL_ERR_INTERNAL, "the console's thread could not start");
	}
	*out = st;
	return HULL_OK;
}

/* Input for COM1: it goes to qemu's stdin, which the port reads. */
static hull_status pc_stream_write(hull_core *core, hull_stream *st, const uint8_t *data,
                                   size_t length, hull_error *error)
{
	(void)core;
	while (length > 0u) {
		ssize_t n = send(st->machine->input, data, length, MSG_NOSIGNAL);

		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0)
			return fail(error, HULL_ERR_GONE, "the machine is not running");
		data += n;
		length -= (size_t)n;
	}
	return HULL_OK;
}

/* A client's input ending ends nothing: what it typed may still answer. */
static hull_status pc_stream_eof(hull_core *core, hull_stream *st, hull_error *error)
{
	(void)core;
	(void)st;
	(void)error;
	return HULL_OK;
}

static hull_status pc_stream_close(hull_core *core, hull_stream *st, hull_error *error)
{
	hull_machine *m = st->machine;

	(void)core;
	(void)error;
	end_console(m);
	pthread_join(st->thread, NULL);
	pthread_mutex_lock(&m->lock);
	m->console = NULL;
	pthread_mutex_unlock(&m->lock);
	close(st->from);
	free(st);
	return HULL_OK;
}

static hull_status pc_pause(hull_core *core, hull_machine *m, int paused, hull_error *error)
{
	int gone;

	(void)core;
	pthread_mutex_lock(&m->lock);
	gone = m->done || m->pid <= 0 || kill(m->pid, paused ? SIGSTOP : SIGCONT) != 0;
	if (!gone)
		m->paused = paused;
	pthread_mutex_unlock(&m->lock);
	return gone ? fail(error, HULL_ERR_GONE, "the machine is not running") : HULL_OK;
}

static hull_status pc_stats(hull_core *core, hull_machine *m, hull_buf *out, hull_error *error)
{
	static unsigned char encoded[128];
	hull_usage           usage;
	int                  ok;

	(void)core;
	memset(&usage, 0, sizeof usage);
	pthread_mutex_lock(&m->lock);
	ok = !m->done && m->pid > 0 && hull_usage_add(m->pid, &usage) == 0;
	pthread_mutex_unlock(&m->lock);
	if (!ok)
		return fail(error, HULL_ERR_GONE, "the machine is not running");
	out->data = encoded;
	out->len  = hull_usage_encode(&usage, encoded, sizeof encoded);
	return out->len == 0u ? fail(error, HULL_ERR_INTERNAL, "stats do not fit") : HULL_OK;
}

static const hull_cap_entry entries[] = {
	{ "source.media.overlay", HULL_SUP_NATIVE,
	  "the firmware boots the drive named; a disk's or floppy's writes are a snapshot thrown "
	  "away with the machine", NULL },
	{ "source.media.direct",  HULL_SUP_NATIVE,
	  "the firmware boots the drive named; a disk's or floppy's writes land in its image: file",
	  NULL },
	{ "source.image.overlay", HULL_SUP_NATIVE,
	  "attached: the image is the first disk and boots, its writes a snapshot thrown away "
	  "with the machine", NULL },
	{ "source.image.direct",  HULL_SUP_NATIVE,
	  "attached: the image is the first disk and boots, its writes landing in it", NULL },
	{ "projection.bind",    HULL_SUP_NATIVE,   "nothing projected: the media are the machine's",
	  NULL },
	{ "writeback.none",     HULL_SUP_NATIVE,   NULL, NULL },
	{ "media",              HULL_SUP_NATIVE,
	  "a drive each: disks and CD-ROMs on IDE, four between them, and two floppy drives", NULL },
	{ "media.dir",          HULL_SUP_EMULATED,
	  "the collection reassembled, its hash held, into an unnamed file its writes go with; a "
	  "CD-ROM's tree alone made an ISO; a raw dump's audio tracks and quirks not given, qemu "
	  "reading a disc by its 2048-byte sectors", NULL },
	{ "media.image",        HULL_SUP_NATIVE,
	  "raw, qcow2, vmdk, vhd, vhdx or vdi, by its first bytes", NULL },
	{ "media.tar",          HULL_SUP_REJECTED, "not yet: a tar: collection", NULL },
	{ "media.pack",         HULL_SUP_REJECTED, "not yet: a pack: collection", NULL },
	{ "network.none",       HULL_SUP_NATIVE,   "a PC with no network card", NULL },
	{ "network.nat",        HULL_SUP_NATIVE,
	  "qemu's own user network, a card of (tuning (pc (nic ...))), ne2k_pci by default, DHCP "
	  "at 10.0.2.2; none unless asked for", NULL },
	{ "segments",           HULL_SUP_NATIVE,
	  "a card on each, of (tuning (pc (nic ...))), beside any (network nat), its address the "
	  "guest's own to set, nothing of hull's running there to give it one", NULL },
	{ "impair",             HULL_SUP_NATIVE,   "in the core's switch", NULL },
	{ "tamper",             HULL_SUP_NATIVE,   "in the core's switch", NULL },
	{ "ports",              HULL_SUP_NATIVE,
	  "qemu's forwards from the machine's address, with (network nat), to whatever the guest's "
	  "own stack answers", NULL },
	{ "display.window",     HULL_SUP_NATIVE,
	  "qemu's SDL window on the X display the run was given; no sound", NULL },
	{ "display.vnc",        HULL_SUP_NATIVE,
	  "the screen, keyboard and mouse by VNC at the machine's address, port 5900, with no "
	  "password: any local user can connect, as to its ports", NULL },
	{ "memory",             HULL_SUP_NATIVE,   NULL, NULL },
	{ "cpus",               HULL_SUP_NATIVE,   NULL, NULL },
	{ "pc.firmware.bios", HULL_SUP_NATIVE, "qemu's SeaBIOS", NULL },
	{ "pc.firmware.uefi", HULL_SUP_NATIVE, "OVMF, with no variable store", NULL },
	{ "pc.machine.pc", HULL_SUP_NATIVE, NULL, NULL },
	{ "pc.machine.q35", HULL_SUP_NATIVE, NULL, NULL },
	{ "pc.machine.isapc", HULL_SUP_NATIVE, NULL, NULL },
	{ "pc.vga.std", HULL_SUP_NATIVE, NULL, NULL },
	{ "pc.vga.cirrus", HULL_SUP_NATIVE, NULL, NULL },
	{ "pc.vga.vmware", HULL_SUP_NATIVE, NULL, NULL },
	{ "pc.vga.none", HULL_SUP_NATIVE, NULL, NULL },
	{ "pc.accel.auto", HULL_SUP_NATIVE, NULL, NULL },
	{ "pc.accel.kvm", HULL_SUP_NATIVE, NULL, NULL },
	{ "pc.accel.tcg", HULL_SUP_NATIVE, NULL, NULL },
	{ "pc.nic.ne2k_pci", HULL_SUP_NATIVE, NULL, NULL },
	{ "pc.nic.ne2k_isa", HULL_SUP_NATIVE, NULL, NULL },
	{ "pc.nic.pcnet", HULL_SUP_NATIVE, NULL, NULL },
	{ "pc.nic.rtl8139", HULL_SUP_NATIVE, NULL, NULL },
	{ "pc.nic.e1000", HULL_SUP_NATIVE, NULL, NULL },
	{ "pc.nic.virtio", HULL_SUP_NATIVE, NULL, NULL },
	{ "security.no_new_privs", HULL_SUP_NATIVE, "set on the qemu process", NULL },
	{ "on_disconnect.stop", HULL_SUP_NATIVE,   NULL, NULL },
	{ "on_disconnect.keep", HULL_SUP_REJECTED,
	  "not yet: qemu is a child of the core, and dies with it", NULL },
	{ "hostname",           HULL_SUP_EMULATED, "the name qemu's window is given; the guest's own "
	  "name is the guest's", NULL },

	{ NULL,                 HULL_SUP_NATIVE,   NULL, NULL },
};

static const char *const classes[] = { "pc", NULL };
static const char *const media[]   = { "cdrom", "floppy", "disk", NULL };

static const hull_capabilities caps = {
	.struct_size    = sizeof (hull_capabilities),
	.engine_name    = "qemu-pc",
	.engine_version = "0.1",
	.classes        = classes,
	.entries        = entries,
	.media_types    = media,
	.migration      = HULL_MIG_PINNED,
	.arches         = NULL,
	.requires_root  = 0,
};

static const hull_engine_ops ops = {
	.struct_size = sizeof (hull_engine_ops),
	.create      = pc_create,
	.start       = pc_start,
	.stop        = pc_stop,
	.destroy     = pc_destroy,
	.signal      = pc_signal,
	.exec         = pc_exec,
	.stream_write = pc_stream_write,
	.stream_eof   = pc_stream_eof,
	.stream_close = pc_stream_close,
	.stats       = pc_stats,
	.pause       = pc_pause,
};

static const hull_engine engine = {
	.struct_size = sizeof (hull_engine),
	.abi_major   = HULL_ABI_MAJOR,
	.abi_minor   = HULL_ABI_MINOR,
	.caps        = &caps,
	.ops         = &ops,
};

const hull_engine *hull_engine_qemu_pc(const hull_host_ops *host_ops)
{
	if (host_ops != NULL)
		host = host_ops;
	return &engine;
}
