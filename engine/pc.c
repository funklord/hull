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
 * written. A dir: collection is reassembled, on the machine's own thread
 * and not the core's loop, into an unnamed file, whose hash media.c holds
 * to the collection's, its writes going with the fd, so a killed core
 * leaves nothing to sweep.
 *
 * `hull exec -t ID -- console` is the serial bridge: a stream joined to
 * COM1, input written to the port and what the port says copied to it,
 * so a system with a console there can be driven with nothing of hull's
 * in the guest. It is a terminal and no more: no exit status of what is
 * typed, no sftp, no forwarding. One at a time. What the port says goes
 * through a pipe of the console's, dropped when full, for a slow client
 * must not hold the machine's own output, or its guest, back.
 *
 * Port 22 is the door (section 8.1): `ssh pc.hull`, by way of `hull
 * proxy-ssh`, reaches the guest's own sshd where a port forwards to one,
 * and under (ssh true) otherwise the host's OpenSSH, `sshd -i` for each
 * connection with the machine's host key and the user's keys, its one
 * command the console above. Hull writes no ssh server: where a host has
 * no natural sshd the door is refused, saying so, the holder's rule being
 * that a connector is made only for a host that could not have one.
 *
 * `hull media change` swaps a floppy or CD under a running machine by
 * qemu's QMP, on a socket qemu inherits: a set of disks is how much of
 * what this class runs was shipped.
 *
 * The guest powering off is the machine's end, status 0, and a stop
 * pulls the plug by SIGTERM, there being no agent to ask and no telling
 * whether a 1990s system answers ACPI.
 */

#define _GNU_SOURCE

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
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
#define PC_MAX_ARGS  80u
#define PC_MAX_SEGMENTS 4u
#define PC_MAX_PORTS 32u

typedef struct medium {
	int   type;          /* 'c' cdrom, 'd' disk, 'f' floppy */
	char *path;          /* an image: path, or NULL */
	int   fd;            /* a collection reassembled, or -1 */
	char *collection;    /* its directory, until then */
	int   exact;         /* `hull media import`'s, or a tree alone */
	char  format[8];     /* qemu's word for an image's container */
} medium;

struct hull_stream {
	hull_machine *machine;
	hull_core    *core;
	int           from;     /* the console's end of its pipe */
	int           to;       /* the reader's, or -1 once the console has ended */
	int           socket;   /* a dial's connection, or -1 for the console */
	pid_t         child;    /* a door's sshd, or 0 */
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
	char           *sound;          /* (tuning (pc (sound ...))), or NULL */
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
	int             monitor;        /* qemu's QMP, a socket, or -1 before it starts */
	int             monitor_peer;   /* its other end, for qemu to take */
	int             monitor_ready;  /* the greeting read and capabilities said */
	pthread_mutex_t monitor_lock;
	hull_stream    *console;        /* the one joined to COM1, or NULL */
	char           *ssh_dir;        /* (ssh true): the core's material, door.conf beside it */
	char            sshd[64];       /* and the host's sshd */
	pthread_t       reader;
	int             reading;
	pthread_mutex_t lock;
	pthread_cond_t  done_cond;
	int             done;
	int             paused;
	int             stopping;       /* asked to stop before qemu was started */
	char            why[800];       /* the thread's own word for its failure */
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
		free(m->media[i].collection);
		if (m->media[i].fd >= 0)
			close(m->media[i].fd);
	}
	for (unsigned i = 0u; i < m->segment_count; i++) {
		free(m->segment[i]);
		if (m->segment_fd[i] >= 0)
			close(m->segment_fd[i]);
	}
	free(m->ssh_dir);
	free(m->memory);
	free(m->cpus);
	free(m->machine_type);
	free(m->vga);
	free(m->accel);
	free(m->sound);
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
	if (m->monitor >= 0)
		close(m->monitor);
	pthread_mutex_destroy(&m->monitor_lock);
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

/* A collection's medium checked, to be made as the machine starts: NULL,
 * or why not. A cdrom's may be a tree alone, an ISO then made of it. */
static const char *add_collection(hull_machine *m, medium *d, const char *collection)
{
	char        files[4200];
	struct stat st;

	d->exact = hull_media_exact(collection);
	if (!d->exact && d->type != 'c')
		return "a floppy's or disk's collection is one `hull media import` made of its image";
	snprintf(files, sizeof files, "%s/files", collection);
	if (stat(files, &st) != 0 || !S_ISDIR(st.st_mode))
		return "a medium is not there: a dir: collection holds its disc's tree in files/";
	if (m->cache[0] == '\0') {
		const char *wrong = make_cache(m);

		if (wrong != NULL)
			return wrong;
	}
	if ((d->collection = strdup(collection)) == NULL)
		return "out of memory";
	strcpy(d->format, "raw");
	return NULL;
}

/* A collection reassembled into an unnamed file of the cache's, on the
 * machine's own thread, as a disk's gigabytes would hold every other
 * machine of the core up on its loop: 0, or -1 with m->why. */
static int make_image(hull_machine *m, medium *d)
{
	char detail[512];
	char files[4200];

	snprintf(files, sizeof files, "%s/files", d->collection);
	if ((d->fd = open(m->cache, O_TMPFILE | O_RDWR | O_CLOEXEC, 0600)) < 0) {
		snprintf(m->why, sizeof m->why, "an image cannot be made in the cache directory: %s\n",
		         strerror(errno));
		return -1;
	}
	if ((d->exact ? hull_media_write(d->collection, d->fd, detail, sizeof detail) :
	     hull_iso_write(files, "", d->fd, detail, sizeof detail)) != 0) {
		snprintf(m->why, sizeof m->why, "a medium's image cannot be made: %s\n", detail);
		return -1;
	}
	return 0;
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
			wrong = add_collection(m, d, path);
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

/* (ssh true): the host's OpenSSH found and door.conf written beside the
 * core's material: NULL, or why not. */
static const char *read_door(hull_machine *m, const hull_sexpr_doc *doc)
{
	static const char *const places[] = { "/usr/sbin/sshd", "/usr/local/sbin/sshd", "/sbin/sshd",
	                                      NULL };
	hull_sexpr_form top;
	const char     *runtime = getenv("XDG_RUNTIME_DIR");
	const char     *id;
	char            self[4096];
	char            path[4400];
	ssize_t         n = readlink("/proc/self/exe", self, sizeof self - 1u);
	FILE           *conf;
	int             fd;

	if (!hull_sexpr_first(doc, &top) ||
	    (m->ssh_dir = hull_access_value(doc, &top, "ssh_dir")) == NULL)
		return NULL;
	for (const char *const *place = places; *place != NULL && m->sshd[0] == '\0'; place++)
		if (access(*place, X_OK) == 0)
			snprintf(m->sshd, sizeof m->sshd, "%s", *place);
	if (m->sshd[0] == '\0')
		return "(ssh true) on a pc machine is the host's OpenSSH in front of its serial port, and "
		       "this host has no sshd: its openssh-server package gives one";
	if (n <= 0 || runtime == NULL || strpbrk(runtime, "\"\\\n") != NULL)
		return "(ssh true): the door's command cannot be written";
	self[n] = '\0';
	if (strpbrk(self, "\"\\\n") != NULL)
		return "(ssh true): hull's path cannot go in the door's command";
	id = strrchr(m->ssh_dir, '/');
	id = id != NULL ? id + 1 : m->ssh_dir;
	snprintf(path, sizeof path, "%s/door.conf", m->ssh_dir);
	if ((fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0400)) < 0 ||
	    (conf = fdopen(fd, "w")) == NULL) {
		if (fd >= 0)
			close(fd);
		return "(ssh true): the door's configuration cannot be written";
	}
	/* The one command, with nothing else the host's sshd could give:
	 * this is a door to a serial port, not a login to the host. */
	fprintf(conf, "HostKey %s/host_key\n"
	        "AuthorizedKeysFile %s/authorized_keys\n"
	        "PidFile none\n"
	        "UsePAM no\n"
	        "StrictModes no\n"
	        "PubkeyAuthentication yes\n"
	        "PasswordAuthentication no\n"
	        "KbdInteractiveAuthentication no\n"
	        "AllowTcpForwarding no\n"
	        "AllowStreamLocalForwarding no\n"
	        "AllowAgentForwarding no\n"
	        "X11Forwarding no\n"
	        "PermitTunnel no\n"
	        "PermitUserRC no\n"
	        "PermitUserEnvironment no\n"
	        "ForceCommand exec env XDG_RUNTIME_DIR=\"%s\" \"%s\" exec -t %s -- console\n",
	        m->ssh_dir, m->ssh_dir, runtime, self, id);
	return fclose(conf) == 0 ? NULL : "(ssh true): the door's configuration cannot be written";
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
	if ((wrong = read_network(m, doc, machine)) != NULL || (wrong = read_door(m, doc)) != NULL)
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
			m->sound        = hull_access_value(doc, &pc, "sound");
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
	char     monitor[48];
	int      monitor_fd;
	unsigned n = 0u;
	unsigned ide = 0u;
	unsigned floppy = 0u;
	unsigned cdrom = 0u;
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
	/* The monitor, for changing a medium: a descriptor, so no path to
	 * clean up and nothing a second client could reach. */
	if ((monitor_fd = fcntl(m->monitor_peer, F_DUPFD, 3)) < 0)
		return;
	snprintf(monitor, sizeof monitor, "socket,id=monitor,fd=%d", monitor_fd);
	argv[n++] = "-chardev";
	argv[n++] = monitor;
	argv[n++] = "-mon";
	argv[n++] = "chardev=monitor,mode=control";
	argv[n++] = "-vga";
	argv[n++] = m->vga != NULL ? m->vga : "std";
	/* VNC's display 0 is port 5900, at the machine's own address. */
	snprintf(vnc, sizeof vnc, "vnc=%s:0", m->address);
	argv[n++] = "-display";
	argv[n++] = m->vnc ? vnc : m->display != NULL ? "sdl" : "none";
	argv[n++] = "-boot";
	argv[n++] = boot;
	/* A card, heard where there is a window, SDL's audio beside its
	 * video, and playing to nothing where there is not: a system still
	 * finds the card it was installed with. */
	if (m->sound != NULL && strcmp(m->sound, "none") != 0) {
		static char card[48];

		argv[n++] = "-audiodev";
		argv[n++] = m->display != NULL ? "sdl,id=sound" : "none,id=sound";
		if (strcmp(m->sound, "hda") == 0) {
			argv[n++] = "-device";
			argv[n++] = "intel-hda";
			snprintf(card, sizeof card, "hda-output,audiodev=sound");
		} else {
			snprintf(card, sizeof card, "%s,audiodev=sound", m->sound);
		}
		argv[n++] = "-device";
		argv[n++] = card;
	}
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
			char  *indexed = malloc(length + 40u);

			if (indexed == NULL)
				return;
			/* What change_medium() calls it, for a removable one. */
			if (d->type == 'f')
				sprintf(indexed, "%s,index=%u,id=floppy%u", drive, floppy, floppy);
			else if (d->type == 'c')
				sprintf(indexed, "%s,index=%u,id=cdrom%u", drive, ide, cdrom);
			else
				sprintf(indexed, "%s,index=%u", drive, ide);
			floppy += d->type == 'f';
			cdrom += d->type == 'c';
			ide += d->type != 'f';
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

/* The machine's end said. */
static void finish(hull_machine *m, int code, int signum)
{
	pthread_mutex_lock(&m->lock);
	m->done = 1;
	pthread_cond_broadcast(&m->done_cond);
	pthread_mutex_unlock(&m->lock);
	end_console(m);
	host->on_exit(m->core, m, NULL, code, signum);
	host->on_state(m->core, m, "stopped");
}

/* The two pipes handed on until both end, then the end said. */
static void read_machine(hull_machine *m)
{
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
	finish(m, WIFSIGNALED(status) ? -1 : WEXITSTATUS(status),
	       WIFSIGNALED(status) ? WTERMSIG(status) : 0);
}

/* Each of the `n` descriptors that is open, closed. */
static void close_all(int *fds, unsigned n)
{
	for (unsigned i = 0u; i < n; i++)
		if (fds[i] >= 0)
			close(fds[i]);
}

/* qemu started, its stdio this machine's: 0, or -1 with m->why, nothing
 * of it left open, the core living on past a failure. Forked from the
 * thread that lives as long as it, PR_SET_PDEATHSIG going by the thread
 * and not the process. */
static int launch(hull_machine *m)
{
	int   fds[8] = { -1, -1, -1, -1, -1, -1, -1, -1 };
	int  *in = fds, *monitor = fds + 2, *out = fds + 4, *err = fds + 6;
	pid_t parent = getpid();
	pid_t child;

	if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, in) != 0 ||
	    socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, monitor) != 0) {
		snprintf(m->why, sizeof m->why, "a socket for the machine could not be made\n");
		close_all(fds, 8u);
		return -1;
	}
	if (pipe2(out, O_CLOEXEC) != 0 || pipe2(err, O_CLOEXEC) != 0) {
		snprintf(m->why, sizeof m->why, "a pipe for the machine could not be made\n");
		close_all(fds, 8u);
		return -1;
	}
	m->monitor_peer = monitor[1];
	child = fork();
	if (child < 0) {
		snprintf(m->why, sizeof m->why, "qemu could not be started\n");
		m->monitor_peer = -1;
		close_all(fds, 8u);
		return -1;
	}
	if (child == 0) {
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
	close(monitor[1]);
	for (unsigned i = 0u; i < m->segment_count; i++) {
		close(m->segment_fd[i]);
		m->segment_fd[i] = -1;
	}
	pthread_mutex_lock(&m->lock);
	m->pid   = child;
	m->input = in[0];
	m->monitor = monitor[0];
	m->out   = out[0];
	m->err   = err[0];
	pthread_mutex_unlock(&m->lock);
	return 0;
}

/* The machine's thread: its images, qemu, and what it says to its end. */
static void *run_machine(void *arg)
{
	hull_machine *m = arg;
	int           failed = 0;
	int           stopped;

	for (unsigned i = 0u; i < m->media_count && !failed; i++)
		if (m->media[i].collection != NULL)
			failed = make_image(m, &m->media[i]) != 0;
	pthread_mutex_lock(&m->lock);
	stopped = m->stopping;
	pthread_mutex_unlock(&m->lock);
	if (stopped) {
		finish(m, -1, SIGTERM);
		return NULL;
	}
	if (failed || launch(m) != 0) {
		host->on_data(m->core, NULL, 2, (const uint8_t *)m->why, strlen(m->why));
		finish(m, 125, 0);
		return NULL;
	}
	read_machine(m);
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
	m->monitor = -1;
	pthread_mutex_init(&m->lock, NULL);
	pthread_mutex_init(&m->monitor_lock, NULL);
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
	if (pthread_create(&m->reader, NULL, run_machine, m) != 0)
		return fail(error, HULL_ERR_INTERNAL, "the machine's thread could not start");
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
	m->stopping = 1;
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
	}
	release(m);
	return HULL_OK;
}

static hull_status pc_signal(hull_core *core, hull_machine *m, hull_stream *stream, int signum,
                             hull_error *error)
{
	(void)core;
	(void)error;
	/* A signal to a stream ends it; the machine goes on. */
	if (stream != NULL) {
		if (stream->socket >= 0)
			shutdown(stream->socket, SHUT_RDWR);
		else
			end_console(m);
		return HULL_OK;
	}
	pthread_mutex_lock(&m->lock);
	if (!m->done && m->pid > 0)
		kill(m->pid, signum);
	pthread_mutex_unlock(&m->lock);
	return HULL_OK;
}

/* One line of QMP's answer read into `why`: 1 for a return, 0 for an error
 * whose words are in `why`, -1 for no answer within the time. Events and the
 * greeting pass by, an answer being the only line opening with these. */
static int monitor_answer(hull_machine *m, char *why, size_t cap)
{
	char   line[2048];
	size_t have = 0u;
	long   left = 5000;

	while (left > 0) {
		struct pollfd   p = { .fd = m->monitor, .events = POLLIN };
		struct timespec before;
		struct timespec after;
		char           *end;
		ssize_t         n;
		int             ready;

		clock_gettime(CLOCK_MONOTONIC, &before);
		ready = poll(&p, 1u, (int)left);
		clock_gettime(CLOCK_MONOTONIC, &after);
		left -= (after.tv_sec - before.tv_sec) * 1000L + (after.tv_nsec - before.tv_nsec) / 1000000L;
		if (ready < 0 && errno == EINTR)
			continue;
		if (ready <= 0)
			break;
		n = read(m->monitor, line + have, sizeof line - 1u - have);
		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0)
			break;
		have += (size_t)n;
		line[have] = '\0';
		while ((end = strchr(line, '\n')) != NULL) {
			const char *desc;

			*end = '\0';
			if (strncmp(line, "{\"return\"", 9u) == 0)
				return 1;
			if (strncmp(line, "{\"error\"", 8u) == 0) {
				desc = strstr(line, "\"desc\": \"");
				snprintf(why, cap, "%.*s", desc != NULL ? (int)strcspn(desc + 9, "\"") : 0,
				         desc != NULL ? desc + 9 : "");
				return 0;
			}
			have -= (size_t)(end + 1 - line);
			memmove(line, end + 1, have + 1u);
		}
		if (have == sizeof line - 1u)
			have = 0u;   /* a line longer than any answer: an event's */
	}
	snprintf(why, cap, "qemu's monitor did not answer");
	return -1;
}

/* A command said to QMP: NULL, or why it failed. */
static const char *monitor_say(hull_machine *m, const char *command, char *why, size_t cap)
{
	static const char capabilities[] = "{\"execute\":\"qmp_capabilities\"}\n";

	if (!m->monitor_ready) {
		if (send(m->monitor, capabilities, sizeof capabilities - 1u, MSG_NOSIGNAL) < 0 ||
		    monitor_answer(m, why, cap) != 1)
			return "qemu's monitor did not answer";
		m->monitor_ready = 1;
	}
	if (send(m->monitor, command, strlen(command), MSG_NOSIGNAL) < 0)
		return "the machine is not running";
	return monitor_answer(m, why, cap) == 1 ? NULL : why;
}

/* The medium of a removable drive changed, or with no `path` taken out: the
 * swap a floppy set or a second CD asks for. `drive` is its type, then its
 * number from the second on, `floppy`, `floppy2`, `cdrom`. Lasts as long as
 * the machine: what boots next is what the file says. Under (mutable overlay)
 * a floppy goes in write-protected, qemu's snapshot being taken only as it
 * starts, so what overlay promises, the file unchanged, holds. */
static hull_status change_medium(hull_machine *m, const char *drive, const char *path,
                                 hull_error *error)
{
	char        command[8192];
	char        escaped[4200];
	char        why[512] = "";
	char        id[16];
	const char *wrong;
	int         cdrom = strncmp(drive, "cdrom", 5u) == 0;
	const char *rest = drive + (cdrom ? 5 : 6);
	unsigned    wanted = 1u;
	unsigned    seen = 0u;
	size_t      at = 0u;
	medium      probe = { .path = (char *)path };

	if (!cdrom && strncmp(drive, "floppy", 6u) != 0)
		return fail(error, HULL_ERR_RUNTIME, "a drive whose medium changes is a floppy or a cdrom");
	if (*rest != '\0' && (rest[0] < '2' || rest[0] > '9' || rest[1] != '\0'))
		return fail(error, HULL_ERR_RUNTIME, "a drive is `floppy` or `cdrom`, then 2 and on for "
		            "the next of that type");
	if (*rest != '\0')
		wanted = (unsigned)(rest[0] - '0');
	for (unsigned i = 0u; i < m->media_count; i++)
		seen += m->media[i].type == (cdrom ? 'c' : 'f');
	if (wanted > seen)
		return fail(error, HULL_ERR_RUNTIME, "this machine has no such drive: a medium of that "
		            "type in (media ...) is what gives it one");
	snprintf(id, sizeof id, "%s%u", cdrom ? "cdrom" : "floppy", wanted - 1u);
	if (path == NULL) {
		snprintf(command, sizeof command, "{\"execute\":\"eject\",\"arguments\":"
		         "{\"device\":\"%s\",\"force\":true}}\n", id);
	} else {
		if (path[0] != '/')
			return fail(error, HULL_ERR_RUNTIME, "a medium's path is absolute by here");
		for (const char *c = path; *c != '\0'; c++) {
			if (at + 3u > sizeof escaped)
				return fail(error, HULL_ERR_RUNTIME, "the medium's path is too long");
			if ((unsigned char)*c < 0x20u)
				return fail(error, HULL_ERR_RUNTIME, "the medium's path holds a control character");
			if (*c == '"' || *c == '\\')
				escaped[at++] = '\\';
			escaped[at++] = *c;
		}
		escaped[at] = '\0';
		read_format(&probe);
		snprintf(command, sizeof command, "{\"execute\":\"blockdev-change-medium\",\"arguments\":"
		         "{\"device\":\"%s\",\"filename\":\"%s\",\"format\":\"%s\","
		         "\"read-only-mode\":\"%s\",\"force\":true}}\n", id, escaped, probe.format,
		         cdrom || !m->direct ? "read-only" : "read-write");
	}
	pthread_mutex_lock(&m->lock);
	if (m->done || m->monitor < 0 || m->paused) {
		int gone = m->done;
		int paused = m->paused;

		pthread_mutex_unlock(&m->lock);
		return gone ? fail(error, HULL_ERR_GONE, "the machine is not running") :
		       fail(error, HULL_ERR_BUSY, paused ? "the machine is paused" :
		            "the machine's media are still being made");
	}
	pthread_mutex_unlock(&m->lock);
	pthread_mutex_lock(&m->monitor_lock);
	wrong = monitor_say(m, command, why, sizeof why);
	pthread_mutex_unlock(&m->monitor_lock);
	return wrong != NULL ? fail(error, HULL_ERR_RUNTIME, wrong) : HULL_OK;
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

static void *deliver_socket(void *arg);

/* A stream with nothing to say: a socket whose far end has gone, so it ends
 * at once, status 0, by a dial's own path. */
static hull_status finished_stream(hull_core *core, hull_machine *m, hull_stream **out,
                                   hull_error *error)
{
	hull_stream *st = calloc(1u, sizeof *st);
	int          ends[2];

	if (st == NULL)
		return fail(error, HULL_ERR_INTERNAL, "out of memory");
	if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, ends) != 0) {
		free(st);
		return fail(error, HULL_ERR_RUNTIME, "a socket for the answer could not be made");
	}
	close(ends[1]);
	st->machine = m;
	st->core    = core;
	st->from    = -1;
	st->to      = -1;
	st->socket  = ends[0];
	if (pthread_create(&st->thread, NULL, deliver_socket, st) != 0) {
		close(ends[0]);
		free(st);
		return fail(error, HULL_ERR_INTERNAL, "the answer's thread could not start");
	}
	*out = st;
	return HULL_OK;
}

static hull_status pc_exec(hull_core *core, hull_machine *m, hull_buf spec, hull_stream **out,
                           hull_error *error)
{
	hull_sexpr_doc  doc;
	hull_sexpr_form message;
	hull_sexpr_form argv;
	hull_sexpr_form word;
	hull_stream    *st;
	char           *words[3] = { NULL, NULL, NULL };
	unsigned        count = 0u;
	int             ends[2];
	int             asked;

	if (hull_sexpr_open(&doc, spec.data, spec.len, HULL_SEXPR_CANONICAL, NULL) == HULL_SEXPR_OK &&
	    hull_sexpr_first(&doc, &message) && hull_access_find(&doc, &message, "argv", &argv)) {
		hull_sexpr_child(&doc, &argv, &word);
		while (count < 3u && hull_sexpr_next(&doc, &word, &word))
			words[count++] = hull_access_text(&doc, &word);
	}
	/* `hull media change`, which asks and is done. */
	if (words[0] != NULL && strcmp(words[0], "change") == 0 && words[1] != NULL) {
		hull_status changed = change_medium(m, words[1], words[2], error);

		for (unsigned i = 0u; i < count; i++)
			free(words[i]);
		return changed == HULL_OK ? finished_stream(core, m, out, error) : changed;
	}
	asked = words[0] != NULL && strcmp(words[0], "console") == 0;
	for (unsigned i = 0u; i < count; i++)
		free(words[i]);
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
	st->socket  = -1;
	pthread_mutex_lock(&m->lock);
	if (m->done || m->console != NULL || m->input < 0) {
		int gone = m->done;
		int early = m->input < 0;

		pthread_mutex_unlock(&m->lock);
		close(ends[0]);
		close(ends[1]);
		free(st);
		return gone ? fail(error, HULL_ERR_GONE, "the machine is not running") :
		       fail(error, HULL_ERR_BUSY, early ? "the machine's media are still being made" :
		            "the serial port has a console joined to it already");
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

/* A dial's connection, to its end, then the stream's. */
static void *deliver_socket(void *arg)
{
	hull_stream *st = arg;
	uint8_t      buffer[16384];
	ssize_t      n;

	while ((n = read(st->socket, buffer, sizeof buffer)) > 0 || (n < 0 && errno == EINTR))
		if (n > 0)
			host->on_data(st->core, st, 1, buffer, (size_t)n);
	host->on_exit(st->core, st->machine, st, 0, 0);
	return NULL;
}

/* A connection to the forward of guest port `port`: a descriptor, or -1. */
static int dial_forward(const hull_machine *m, unsigned port)
{
	for (unsigned i = 0u; i < m->port_count; i++) {
		struct sockaddr_in at;
		int                fd;

		if (m->port_guest[i] != port || m->port_udp[i])
			continue;
		memset(&at, 0, sizeof at);
		at.sin_family = AF_INET;
		at.sin_port   = htons((uint16_t)m->port_host[i]);
		if (inet_pton(AF_INET, m->address, &at.sin_addr) != 1 ||
		    (fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0)) < 0)
			return -1;
		if (connect(fd, (struct sockaddr *)&at, sizeof at) == 0)
			return fd;
		close(fd);
	}
	return -1;
}

/* The host's sshd on one end of a new socket, `sshd -i`, as inetd would
 * run it: the other end, or -1. The user's own, so it logs in the user
 * alone, and its configuration is the door's. */
static int open_door(const hull_machine *m, pid_t *child)
{
	int  pair[2];
	char conf[4400];

	snprintf(conf, sizeof conf, "%s/door.conf", m->ssh_dir);
	if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, pair) != 0)
		return -1;
	*child = fork();
	if (*child < 0) {
		close(pair[0]);
		close(pair[1]);
		return -1;
	}
	if (*child == 0) {
		int null = open("/dev/null", O_WRONLY);

		if (null < 0 || dup2(pair[1], 0) < 0 || dup2(pair[1], 1) < 0 || dup2(null, 2) < 0 ||
		    setsid() < 0)
			_exit(126);
		close_range(3u, ~0u, 0);
		execl(m->sshd, m->sshd, "-i", "-f", conf, (char *)NULL);
		_exit(127);
	}
	close(pair[1]);
	return pair[0];
}

/* One of the machine's ports, as a stream: a forward of qemu's where it
 * has one, the guest's own server being the natural one to reach, and
 * port 22 otherwise the door where the machine has one. */
static hull_status pc_dial(hull_core *core, hull_machine *m, uint16_t port, hull_stream **out,
                           hull_error *error)
{
	hull_stream *st;
	pid_t        child = 0;
	int          fd = dial_forward(m, port);

	if (fd < 0 && port == 22u && m->ssh_dir != NULL)
		fd = open_door(m, &child);
	if (fd < 0)
		return fail(error, HULL_ERR_GONE, port == 22u ?
		            "this machine has no door: (ssh true), or a port forwarded to its own sshd" :
		            "nothing is forwarded to that port of the machine");
	if ((st = calloc(1u, sizeof *st)) == NULL) {
		close(fd);
		return fail(error, HULL_ERR_INTERNAL, "out of memory");
	}
	st->machine = m;
	st->core    = core;
	st->socket  = fd;
	st->child   = child;
	st->from    = -1;
	st->to      = -1;
	if (pthread_create(&st->thread, NULL, deliver_socket, st) != 0) {
		close(fd);
		if (child > 0) {
			kill(child, SIGKILL);
			waitpid(child, NULL, 0);
		}
		free(st);
		return fail(error, HULL_ERR_INTERNAL, "the connection's thread could not start");
	}
	*out = st;
	return HULL_OK;
}

/* Input for COM1: it goes to qemu's stdin, which the port reads. */
static hull_status pc_stream_write(hull_core *core, hull_stream *st, const uint8_t *data,
                                   size_t length, hull_error *error)
{
	int to = st->socket >= 0 ? st->socket : st->machine->input;

	(void)core;
	while (length > 0u) {
		ssize_t n = send(to, data, length, MSG_NOSIGNAL);

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
	(void)error;
	if (st->socket >= 0)
		shutdown(st->socket, SHUT_WR);
	return HULL_OK;
}

static hull_status pc_stream_close(hull_core *core, hull_stream *st, hull_error *error)
{
	hull_machine *m = st->machine;

	(void)core;
	(void)error;
	if (st->socket >= 0) {
		shutdown(st->socket, SHUT_RDWR);
		pthread_join(st->thread, NULL);
		close(st->socket);
		if (st->child > 0) {
			kill(st->child, SIGTERM);
			waitpid(st->child, NULL, 0);
		}
		free(st);
		return HULL_OK;
	}
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
	{ "media.tar",          HULL_SUP_EMULATED,
	  "committed to the store as a tar: source is, its checkout taken as a dir: collection", NULL },
	{ "media.pack",         HULL_SUP_EMULATED,
	  "read into the store as a pack: source is, its checkout taken as a dir: collection", NULL },
	{ "network.none",       HULL_SUP_NATIVE,   "a PC with no network card", NULL },
	{ "network.nat",        HULL_SUP_NATIVE,
	  "qemu's own user network, a card of (tuning (pc (nic ...))), ne2k_pci by default, DHCP "
	  "at 10.0.2.2; none unless asked for", NULL },
	{ "ssh",                HULL_SUP_EMULATED,
	  "the host's OpenSSH in front of the machine's first serial port: a terminal on whatever "
	  "console the system has there, the machine's name and host key the same; no sftp, no "
	  "forwarding, no exit status of what is typed, one at a time, and the user logs in as "
	  "the host's user. A port forwarded to the guest's own sshd goes first, whole. Refused "
	  "where the host has no sshd", NULL },
	{ "segments",           HULL_SUP_NATIVE,
	  "a card on each, of (tuning (pc (nic ...))), beside any (network nat), its address the "
	  "guest's own to set, nothing of hull's running there to give it one", NULL },
	{ "impair",             HULL_SUP_NATIVE,   "in the core's switch", NULL },
	{ "tamper",             HULL_SUP_NATIVE,   "in the core's switch", NULL },
	{ "ports",              HULL_SUP_NATIVE,
	  "qemu's forwards from the machine's address, with (network nat), to whatever the guest's "
	  "own stack answers", NULL },
	{ "display.window",     HULL_SUP_NATIVE,
	  "qemu's SDL window on the X display the run was given", NULL },
	{ "display.vnc",        HULL_SUP_NATIVE,
	  "the screen, keyboard and mouse by VNC at the machine's address, port 5900, with no "
	  "password: any local user can connect, as to its ports", NULL },
	{ "memory",             HULL_SUP_NATIVE,   NULL, NULL },
	{ "cpus",               HULL_SUP_NATIVE,   NULL, NULL },
	{ "pc.sound.none",  HULL_SUP_NATIVE, NULL, NULL },
	{ "pc.sound.sb16",  HULL_SUP_NATIVE, "a Sound Blaster 16", NULL },
	{ "pc.sound.adlib", HULL_SUP_NATIVE, "an AdLib's OPL2", NULL },
	{ "pc.sound.gus",   HULL_SUP_NATIVE, "a Gravis Ultrasound", NULL },
	{ "pc.sound.es1370", HULL_SUP_NATIVE, "an AudioPCI", NULL },
	{ "pc.sound.ac97",  HULL_SUP_NATIVE, "AC'97", NULL },
	{ "pc.sound.hda",   HULL_SUP_NATIVE, "Intel HD Audio", NULL },
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
	.dial         = pc_dial,
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
