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
 * leaves nothing to sweep; under (mutable direct) a disk's or floppy's
 * is taken apart into the collection again when the machine powers off.
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
#include <dirent.h>
#include <ftw.h>
#include <limits.h>
#include <ctype.h>
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
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "access.h"
#include "builtin.h"
#include "esp.h"
#include "iso.h"
#include "media.h"
#include "nbd.h"
#include "usage.h"
#include "display.h"
#include "hostdev.h"
#include "hw.h"
#include "86box.h"
#include "hostnet.h"
#include "vbox.h"

#define PC_MAX_MEDIA 6u    /* four on IDE, two floppy drives */
#define PC_MAX_ARGS  160u  /* a USB stick takes four, an IDE medium two, a card four */
#define PC_MAX_CARDS 4u    /* (devices (pci ...)), within PC_MAX_ARGS */
#define SECURE_CODE  "/usr/share/OVMF/OVMF_CODE_4M.secboot.fd"
#define SECURE_VARS  "/usr/share/OVMF/OVMF_VARS_4M.ms.fd"
#define PC_MAX_SEGMENTS 4u
#define PC_MAX_PORTS 32u

typedef struct medium {
	int   type;          /* 'c' cdrom, 'd' disk, 'f' floppy */
	char *path;          /* an image: path, or NULL */
	int   fd;            /* a collection reassembled, or -1 */
	char *collection;    /* its directory, until then */
	int   exact;         /* `hull media import`'s, or a tree alone */
	int   write_back;    /* (mutable direct): taken apart into the collection again */
	int   nbd;           /* a disk collection served by NBD, qemu's end; 0 for none */
	hull_media_map  *map;      /* a served collection's, kept where its writes are */
	hull_nbd_writes  writes;   /* under (mutable direct): the blocks the guest wrote */
	pthread_t        server;   /* and the thread serving it, joined before they are read */
	int              serving;  /* that thread is running, to be joined */
	uint64_t         size;     /* (size ...): the disk served this large, 0 for its own */
	char  format[8];     /* qemu's word for an image's container */
} medium;

struct hull_stream {
	hull_machine *machine;
	hull_core    *core;
	int           from;     /* the console's end of its pipe */
	int           to;       /* the reader's, or -1 once the console has ended */
	int           socket;   /* a dial's connection, or -1 for the console */
	int           second;   /* joined to a board's second UART, `console 2` */
	pid_t         child;    /* a door's sshd, or 0 */
	pthread_t     thread;
};

struct hull_machine {
	hull_core      *core;
	medium          media[PC_MAX_MEDIA];
	unsigned        media_count;
	char            boot;           /* -boot's letter */
	int             vbox_efi;       /* virtualbox-pc's: (firmware uefi), VirtualBox's EFI */
	pid_t           vbox_child;     /* and the process its work is named for */
	int             direct;         /* (mutable direct) */
	int             resume;         /* its disks hold its saved state, hull-saved */
	char           *memory;         /* (memory ...), or NULL for qemu's own */
	char           *cpus;
	char           *machine_type;
	char           *vga;
	int             gpu;            /* (tuning (pc (gpu ...))): 0 2d, 1 virgl, 2 venus */
	char           *cpu;        /* (tuning (pc (cpu ...))), or NULL for qemu's own */
	char           *accel;
	char           *sound;          /* (tuning (pc (sound ...))), or NULL */
	int             tablet;         /* (tuning (pc (pointer tablet))): a USB tablet */
	char           *firmware;       /* OVMF's path, for (firmware uefi), or NULL for SeaBIOS */
	int             secure;         /* (firmware uefi-secure): OVMF's secure boot, Microsoft's keys */
	int             tpm;            /* (tpm true): a TPM 2.0 of swtpm's */
	char           *state;          /* (state DIR): its firmware variables and TPM, or NULL */
	char            run_state[4300];  /* where they are this run: DIR, or a copy in the cache */
	int             run_state_made;   /* the copy, removed at the end */
	pid_t           swtpm;
	long            boot_key;       /* (boot_key N): Enter, each second for N, from the start */
	pthread_t       presser;
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
	char           *lora;           /* (tuning (pc (lora NAME))): the air its radio is on */
	char           *smc;            /* (tuning (pc (smc PATH))): the file holding Apple's SMC key */
	char            osk[65];        /* and the key, read from it as the machine starts */
	int             lora_fd;        /* the air's port on the core's switch */
	unsigned        segment_count;
	char           *display;        /* the run's X display, or NULL */
	char           *xauthority;
	int             vnc;            /* (display vnc): the screen at the machine's address */
	int             local;          /* (display local): by qemu's D-Bus display alone */
	int             local_display;  /* and qemu's D-Bus display, by display.qmp */
	int             box;            /* the 86box engine's: 86Box runs it, not qemu */
	char           *box_machine;    /* (tuning (86box (machine ...) ...)), 86Box's names */
	char           *box_cpu;
	char           *box_video;
	char           *box_sound;
	char            display_link[4400]; /* hull/display/ADDRESS, linked to it, or "" */
	char            cache[4096];    /* where qemu's snapshots and made images are */
	hull_device     device[HULL_MAX_DEVICES]; /* (devices (pci ...)), found at start */
	unsigned        device_count;
	pid_t           pid;
	int             out;            /* the first serial port */
	int             err;            /* qemu's stderr */
	int             input;          /* COM1's input: qemu's stdin, a socket */
	int             monitor;        /* qemu's QMP, a socket, or -1 before it starts */
	int             monitor_peer;   /* its other end, for qemu to take */
	int             monitor_ready;  /* the greeting read and capabilities said */
	char            monitor_why[512];  /* qemu's refusal, kept past the call for the core */
	pthread_mutex_t monitor_lock;
	hull_stream    *console;        /* the one joined to COM1, or NULL */
	int             serial2;        /* a qemu board's second UART, a socket, or -1 */
	int             serial2_peer;   /* its other end, for qemu to take */
	hull_stream    *console2;       /* the one joined to it, or NULL */
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

/* A microcontroller board, on qemu or s51, run as a pc machine is: booted
 * from its firmware, the image: source, its first UART the machine's
 * serial port, so its output is the machine's and `hull exec ID --
 * console` writes to it. The boards hal9001 tests on are among them. */
typedef struct board {
	const char *name;
	const char *emulator;
	const char *load;    /* how it takes the firmware: -bios on the AVR, -kernel on ARM */
	const char *chip;    /* qemu's machine, where it is not the board's name */
	unsigned    flash;   /* an ESP32's flash in MB; 0, the smallest of 4, 8 and 16 to hold it */
	unsigned    boot;    /* an ESP32's: where its ROM reads the bootloader in flash */
	const char *nic;     /* its Ethernet, qemu's model, where it has one */
} board;

/* An ESP32: Espressif's qemu (engine/esp.h), the firmware a whole flash
 * image, as `esptool.py merge_bin` makes, given to the chip's SPI flash. */
#define ESP_LOAD "flash"
#define ESP32(name, chip, flash) \
	{ name, "qemu-system-xtensa", ESP_LOAD, chip, flash, 0x1000u, "open_eth" }
#define ESP_XT(name, chip, flash) \
	{ name, "qemu-system-xtensa", ESP_LOAD, chip, flash, 0u, "open_eth" }
#define ESP_RV(name, chip, flash, boot) \
	{ name, "qemu-system-riscv32", ESP_LOAD, chip, flash, boot, "open_eth" }

static const board boards[] = {
	{ "arduino-uno",          "qemu-system-avr", "-bios", NULL, 0u, 0u, NULL },
	{ "arduino-duemilanove",  "qemu-system-avr", "-bios", NULL, 0u, 0u, NULL },
	{ "arduino-mega",         "qemu-system-avr", "-bios", NULL, 0u, 0u, NULL },
	{ "arduino-mega-2560-v3", "qemu-system-avr", "-bios", NULL, 0u, 0u, NULL },
	{ "stm32vldiscovery",     "qemu-system-arm", "-kernel", NULL, 0u, 0u, NULL },
	{ "netduino2",            "qemu-system-arm", "-kernel", NULL, 0u, 0u, NULL },
	{ "netduinoplus2",        "qemu-system-arm", "-kernel", NULL, 0u, 0u, NULL },
	{ "olimex-stm32-h405",    "qemu-system-arm", "-kernel", NULL, 0u, 0u, NULL },
	{ "lm3s6965evb",          "qemu-system-arm", "-kernel", NULL, 0u, 0u, "stellaris_enet" },
	{ "lm3s811evb",           "qemu-system-arm", "-kernel", NULL, 0u, 0u, NULL },
	{ "microbit",             "qemu-system-arm", "-kernel", NULL, 0u, 0u, NULL },
	{ "mps2-an385",           "qemu-system-arm", "-kernel", NULL, 0u, 0u, "lan9118" },
	{ "b-l475e-iot01a",       "qemu-system-arm", "-kernel", NULL, 0u, 0u, NULL },
	/* An 8051, by SDCC's simulator, s51: its 8052 core, which the AT89
	 * parts share, at the 11.0592 MHz crystal a UART's rates divide. */
	{ "at89c51ed2",           "ucsim_51",        "-g", NULL, 0u, 0u, NULL },
	/* Espressif's chips, by their own names, and boards built on them,
	 * each with the flash it ships with. qemu has none of a board's other
	 * parts: a Heltec's OLED and LoRa radio are not there. */
	ESP32("esp32", "esp32", 0u),
	ESP32("esp32-devkitc", "esp32", 4u),
	ESP32("esp32-cam", "esp32", 4u),
	ESP32("heltec-wifi-lora-32", "esp32", 4u),
	ESP32("heltec-wifi-lora-32-v2", "esp32", 8u),
	ESP32("ttgo-lora32", "esp32", 4u),
	ESP32("m5stack-core", "esp32", 16u),
	ESP_XT("esp32s3", "esp32s3", 0u),
	ESP_XT("esp32-s3-devkitc-1", "esp32s3", 8u),
	ESP_XT("heltec-wifi-lora-32-v3", "esp32s3", 8u),
	ESP_XT("xiao-esp32s3", "esp32s3", 8u),
	ESP_XT("lilygo-t-display-s3", "esp32s3", 16u),
	ESP_XT("lilygo-t3-s3", "esp32s3", 4u),
	ESP_RV("esp32c3", "esp32c3", 0u, 0u),
	ESP_RV("esp32-c3-devkitm-1", "esp32c3", 4u, 0u),
	ESP_RV("xiao-esp32c3", "esp32c3", 4u, 0u),
	ESP_RV("esp32c5", "esp32c5", 0u, 0x2000u),
	ESP_RV("esp32c6", "esp32c6", 0u, 0u),
	ESP_RV("esp32-c6-devkitc-1", "esp32c6", 8u, 0u),
	ESP_RV("xiao-esp32c6", "esp32c6", 4u, 0u),
	ESP_RV("esp32c61", "esp32c61", 0u, 0u),
	ESP_RV("esp32h2", "esp32h2", 0u, 0u),
	ESP_RV("esp32-h2-devkitm-1", "esp32h2", 4u, 0u),
	ESP_RV("esp32p4", "esp32p4", 0u, 0x2000u),
	ESP_RV("esp32-p4-function-ev-board", "esp32p4", 16u, 0x2000u),
};

static int is_esp(const board *b)
{
	return b != NULL && strcmp(b->load, ESP_LOAD) == 0;
}

/* The boards with an SX1276 on VSPI, NSS on GPIO 18, NRESET on 14 and DIO0
 * on 26: a Heltec WiFi LoRa 32 (V1 and V2) and a TTGO LoRa32. A V3's
 * radio is an SX1262, which is not modelled. */
static int has_sx1276(const board *b)
{
	static const char *const names[] = { "heltec-wifi-lora-32", "heltec-wifi-lora-32-v2",
	                                     "ttgo-lora32", "lilygo-t3-s3",
	                                     "heltec-wifi-lora-32-v3" };

	for (unsigned i = 0u; b != NULL && i < sizeof names / sizeof names[0]; i++)
		if (strcmp(b->name, names[i]) == 0)
			return 1;
	return 0;
}

/* The board `name` names, or NULL for a PC's machine type. */
static const board *board_named(const char *name)
{
	for (size_t i = 0u; name != NULL && i < sizeof boards / sizeof boards[0]; i++)
		if (strcmp(boards[i].name, name) == 0)
			return &boards[i];
	return NULL;
}

/* Whether the machine is a board on qemu, which is given a second serial
 * port for its second UART; s51's 8051 and a PC are given one. */
static int has_second_uart(const hull_machine *m)
{
	const board *b = m->box ? NULL : board_named(m->machine_type);

	return b != NULL && strncmp(b->emulator, "qemu-", 5u) == 0;
}

/* Whether the machine is a PowerPC Mac -- mac99, a G4 as Mac OS X 10.x
 * ran on, or g3beige -- on qemu-system-ppc under TCG, OpenBIOS its
 * firmware, rather than a PC. */
static int is_mac(const char *machine_type)
{
	return machine_type != NULL &&
	       (strcmp(machine_type, "mac99") == 0 || strcmp(machine_type, "g3beige") == 0);
}

/* qemu-system-ppc, Debian's where installed, else the package `make
 * qemu-ppc` unpacks into the user's cache, as a ppc64le vm finds it
 * (engine/qemu.c). 0, or -1 where neither is there. */
static int mac_emulator(char *out, size_t cap)
{
	const char *cache = getenv("XDG_CACHE_HOME");
	const char *home = getenv("HOME");

	snprintf(out, cap, "/usr/bin/qemu-system-ppc");
	if (access(out, X_OK) == 0)
		return 0;
	if (cache != NULL && cache[0] == '/')
		snprintf(out, cap, "%s/hull/qemu-system-ppc/usr/bin/qemu-system-ppc", cache);
	else if (home != NULL && home[0] == '/')
		snprintf(out, cap, "%s/.cache/hull/qemu-system-ppc/usr/bin/qemu-system-ppc", home);
	return access(out, X_OK) == 0 ? 0 : -1;
}

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
		if (m->media[i].nbd > 0)
			close(m->media[i].nbd);
		if (m->media[i].serving)
			pthread_join(m->media[i].server, NULL);
		free(m->media[i].writes.written);
		hull_media_map_close(m->media[i].map != NULL ? m->media[i].map : m->media[i].writes.map);
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
	free(m->cpu);
	free(m->state);
	free(m->accel);
	free(m->sound);
	free(m->firmware);
	free(m->nic);
	free(m->lora);
	free(m->smc);
	if (m->lora_fd >= 0)
		close(m->lora_fd);
	hull_hostdev_unclaim(m);
	free(m->box_machine);
	free(m->box_cpu);
	free(m->box_video);
	free(m->box_sound);
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
	if (m->serial2 >= 0)
		close(m->serial2);
	if (m->serial2_peer >= 0)
		close(m->serial2_peer);
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

/* A file of this run's directory. */
/* A file of the run directory, into a buffer of the calling thread's own:
 * each machine prepares its state on its own thread, and two starting at
 * once wrote each other's paths into one static buffer, an ESP32's flash
 * then copied for, or booted from, another machine's directory. */
static const char *run_file(const hull_machine *m, const char *name)
{
	static __thread char path[4400];

	snprintf(path, sizeof path, "%s/%s", m->run_state, name);
	return path;
}

/* A file of the machine's state: its (state DIR) where it writes in place,
 * (mutable direct), as its disk is written; else the run's copy, which
 * goes with it as a disk's snapshot does. */
static const char *state_file(const hull_machine *m, const char *name)
{
	static __thread char path[4400];

	snprintf(path, sizeof path, "%s/%s", m->state != NULL && m->direct ? m->state : m->run_state,
	         name);
	return path;
}

static int copy_file(const char *from, const char *to)
{
	char    buffer[65536];
	ssize_t n;
	int     in = open(from, O_RDONLY | O_CLOEXEC);
	int     out = in >= 0 ? open(to, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600) : -1;
	int     good = in >= 0 && out >= 0;

	while (good && (n = read(in, buffer, sizeof buffer)) > 0)
		good = write(out, buffer, (size_t)n) == n;
	if (in >= 0)
		close(in);
	if (out >= 0 && close(out) != 0)
		good = 0;
	return good ? 0 : -1;
}

/* Espressif's qemu for board `b`, the fetched one's path into `out`. 0, or
 * -1 where it is not fetched. */
static int esp_emulator(const board *b, char *out, size_t cap)
{
	char dir[4096];

	if (hull_esp_dir(dir, sizeof dir) != 0)
		return -1;
	snprintf(out, cap, "%.4096s/qemu/bin/%s", dir, b->emulator);
	return access(out, X_OK);
}

/* The qemu an ESP32 with a LoRa radio runs on: Espressif's, built from its
 * source with hull's engine/esp-lora.patch by `make esp-qemu-lora`, named
 * by HULL_ESP_LORA_QEMU. Its ROMs are the fetched release's. 0, or -1
 * where it is not there. */
static int esp_lora_emulator(char *out, size_t cap)
{
	const char *named = getenv("HULL_ESP_LORA_QEMU");

	if (named == NULL || named[0] != '/')
		return -1;
	snprintf(out, cap, "%s", named);
	return access(out, X_OK);
}

/* The same build's RISC-V qemu, for an ESP32-C3's WiFi: beside the
 * Xtensa one HULL_ESP_LORA_QEMU names, as make esp-qemu-lora puts it. */
static int esp_lora_riscv(char *out, size_t cap)
{
	char *slash;

	if (esp_lora_emulator(out, cap) != 0)
		return -1;
	if ((slash = strrchr(out, '/')) == NULL ||
	    (size_t)(slash - out) + sizeof "/qemu-system-riscv32" > cap)
		return -1;
	strcpy(slash, "/qemu-system-riscv32");
	return access(out, X_OK);
}

/* Whether the board's chip has lcgamboa's WiFi: the ESP32's, and the C3's. */
static int esp_wifi_chip(const board *b)
{
	return b != NULL && b->chip != NULL &&
	       (strcmp(b->chip, "esp32") == 0 || strcmp(b->chip, "esp32c3") == 0);
}

/* qemu for a board with WiFi: the C3's RISC-V build or the ESP32's. */
static int esp_wifi_emulator(const board *b, char *out, size_t cap)
{
	return strcmp(b->chip, "esp32c3") == 0 ? esp_lora_riscv(out, cap) : esp_lora_emulator(out, cap);
}

/*
 * An ESP32's flash for this run: the firmware, a whole flash image, copied
 * into the run directory and filled out with erased flash, 0xff, to the
 * board's size, or for a bare chip to the smallest of 4, 8 and 16 MB that
 * holds it -- qemu takes those sizes alone, and the firmware writes to its
 * flash, which must not be the user's file. Refused where the image is no
 * whole flash, its bootloader's 0xe9 not where the chip's ROM reads it: an
 * application's .bin alone, the commonest mistake. 0, or -1 with m->why.
 */
static int esp_flash(hull_machine *m, const board *b)
{
	/* The thread's own: two boards' flashes are made at once, and one
	 * buffer filled with 0xff for one board's padding erased the other's
	 * bootloader as it was copied, "invalid header: 0xffffffff". */
	static __thread uint8_t buffer[65536];
	unsigned       mb = b->flash;
	struct stat    is;
	uint8_t        magic = 0u;
	off_t          size;
	int            in;
	int            out;
	int            good = 0;

	if ((in = open(m->media[0].path, O_RDONLY | O_CLOEXEC)) < 0 || fstat(in, &is) != 0) {
		snprintf(m->why, sizeof m->why, "the firmware cannot be read: %s\n", strerror(errno));
		if (in >= 0)
			close(in);
		return -1;
	}
	if (pread(in, &magic, 1u, (off_t)b->boot) != 1 || magic != 0xe9u) {
		close(in);
		snprintf(m->why, sizeof m->why, "the firmware is not a whole flash image for %s: its "
		         "bootloader is read at 0x%x, which holds no image; esptool.py merge_bin makes "
		         "one from the bootloader, partition table and application\n", b->chip, b->boot);
		return -1;
	}
	if (mb == 0u)
		for (mb = 4u; mb < 16u && is.st_size > (off_t)mb << 20; mb *= 2u)
			;
	size = (off_t)mb << 20;
	if (is.st_size > size) {
		close(in);
		snprintf(m->why, sizeof m->why, "the firmware is larger than the %u MB flash of %s\n",
		         mb, b->name);
		return -1;
	}
	if ((out = open(run_file(m, "flash.bin"), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600)) >= 0) {
		ssize_t got;
		off_t   at = 0;

		while ((got = read(in, buffer, sizeof buffer)) > 0 && write(out, buffer, (size_t)got) == got)
			at += got;
		memset(buffer, 0xff, sizeof buffer);
		while (got == 0 && at < size) {
			size_t n = size - at < (off_t)sizeof buffer ? (size_t)(size - at) : sizeof buffer;

			if (write(out, buffer, n) != (ssize_t)n)
				break;
			at += (off_t)n;
		}
		good = got == 0 && at == size;
		if (close(out) != 0)
			good = 0;
	}
	close(in);
	if (!good) {
		snprintf(m->why, sizeof m->why, "the board's flash could not be made in the run "
		         "directory\n");
		return -1;
	}
	return 0;
}

/* The ESP32 ROM's CRC-8 of a MAC, which ESP-IDF checks the efuse's
 * against: Dallas's, reflected, 0x8c. */
static uint8_t esp_mac_crc(const uint8_t mac[6])
{
	uint8_t crc = 0u;

	for (unsigned i = 0u; i < 6u; i++) {
		crc ^= mac[i];
		for (unsigned bit = 0u; bit < 8u; bit++)
			crc = (crc & 1u) != 0u ? (uint8_t)((crc >> 1) ^ 0x8cu) : (uint8_t)(crc >> 1);
	}
	return crc;
}

/*
 * An ESP32's efuse for this run, efuse.bin: its factory MAC, which the
 * WiFi station takes, the machine's own as its Ethernet's is. Without
 * one Espressif's qemu gives a blank efuse, a MAC of zeros, which slirp
 * answers but which every board shares: two on one segment each sent
 * from 00:00:00:00:00:00, and the second's ARP reply went to itself.
 * An ESP32's 124 bytes are the efuse's read registers in order, BLK0's
 * seven words then BLK1 to 3; the MAC is BLK0's bits 32 to 79, its bytes
 * the other way round, the ROM's CRC-8 of it at 80. A C3's are qemu's
 * ESPEfuseBlocks, 336 bytes: BLK0's six words, then BLK1, whose first 48
 * bits are the MAC the same way round, no CRC; and the chip revision v0.3
 * and efuse block version 1.3 qemu sets where it is given no file, which
 * ESP-IDF 5 requires of a C3. 0, or -1 with m->why.
 */
static int esp_efuse(hull_machine *m, const board *b)
{
	uint8_t efuse[336] = { 0 };
	size_t  size = 124u;
	uint8_t mac[6];
	int     out;
	int     good;

	hull_access_segment_mac(m->address, m->segment_count > 0u ? m->segment[0] : "wifi", 0u, mac);
	if (strcmp(b->chip, "esp32c3") == 0) {
		uint32_t revision = 3u << 24 | 3u << 18;   /* BLK_VERSION_MINOR, WAFER_VER_MINOR_LO */

		size = sizeof efuse;
		for (unsigned i = 0u; i < 6u; i++)
			efuse[24u + i] = mac[5u - i];
		for (unsigned i = 0u; i < 4u; i++)
			efuse[36u + i] = (uint8_t)(revision >> (8u * i));
		efuse[64] = 1u;                              /* BLK_VERSION_MAJOR */
	} else {
		for (unsigned i = 0u; i < 6u; i++)
			efuse[4u + i] = mac[5u - i];
		efuse[10] = esp_mac_crc(mac);
	}
	if ((out = open(run_file(m, "efuse.bin"), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600)) < 0) {
		snprintf(m->why, sizeof m->why, "the board's efuse could not be made in the run directory\n");
		return -1;
	}
	good = write(out, efuse, size) == (ssize_t)size;
	if (close(out) != 0 || !good) {
		snprintf(m->why, sizeof m->why, "the board's efuse could not be made in the run directory\n");
		return -1;
	}
	return 0;
}

/* The files swtpm keeps in its state directory, by name, for copying and
 * removing them. */
static const char *const tpm_files[] = { "tpm2-00.permall", ".lock" };

/*
 * The firmware's variables and the TPM's state for this run, the machine's
 * identity as its disk is, (state DIR) holding them across runs: a run
 * directory in the cache, and in it, or in DIR under (mutable direct),
 * OVMF_VARS.fd -- the DIR's, else a copy of Microsoft's enrolled store --
 * and tpm/; then swtpm, started on them. 0, or -1 with m->why.
 */
static int prepare_state(hull_machine *m)
{
	const board *b = board_named(m->machine_type);
	int          s51 = b != NULL && strcmp(b->emulator, "ucsim_51") == 0;
	char         template[4200];
	char         from[4400];
	char         to[4400];

	if (is_esp(b) && esp_emulator(b, from, sizeof from) != 0) {
		snprintf(m->why, sizeof m->why, "%s runs on Espressif's qemu, which is not fetched: "
		         "hull esp fetch\n", b->name);
		return -1;
	}
	if (m->lora != NULL && esp_lora_emulator(from, sizeof from) != 0) {
		snprintf(m->why, sizeof m->why, "a LoRa radio needs Espressif's qemu built with hull's "
		         "SX1276, `make esp-qemu-lora`, and HULL_ESP_LORA_QEMU naming it\n");
		return -1;
	}
	if (m->nic != NULL && strcmp(m->nic, "wifi") == 0 && b != NULL && esp_wifi_emulator(b, from, sizeof from) != 0) {
		snprintf(m->why, sizeof m->why, "an ESP32's WiFi needs Espressif's qemu built with hull's "
		         "patch, `make esp-qemu-lora`, and HULL_ESP_LORA_QEMU naming it\n");
		return -1;
	}
	/* A screen by VNC has its D-Bus display's monitor here too. */
	if (!m->secure && !m->tpm && !s51 && !is_esp(b) && !m->vnc && !m->local && !m->box)
		return 0;
	if (m->cache[0] == '\0' && make_cache(m) != NULL) {
		snprintf(m->why, sizeof m->why, "the cache directory for the machine's state cannot be made\n");
		return -1;
	}
	snprintf(template, sizeof template, "%s/pc-XXXXXX", m->cache);
	if (mkdtemp(template) == NULL) {
		snprintf(m->why, sizeof m->why, "the machine's run directory cannot be made\n");
		return -1;
	}
	if (m->tpm && strlen(template) > 90u) {
		rmdir(template);
		snprintf(m->why, sizeof m->why, "the machine's run directory cannot be made: the "
		         "cache's path is too long for a socket in it\n");
		return -1;
	}
	snprintf(m->run_state, sizeof m->run_state, "%s", template);
	m->run_state_made = 1;
	/* s51 reads Intel hex only from a file named .ihx, .hex or .ihex, and
	 * from any other name loads nothing and says nothing: the firmware is
	 * linked into the run directory by a name it reads. */
	if (s51) {
		if (m->media[0].path[0] != '/' ||
		    symlink(m->media[0].path, run_file(m, "firmware.ihx")) != 0) {
			snprintf(m->why, sizeof m->why, "the firmware could not be linked for s51\n");
			return -1;
		}
		return 0;
	}
	if (is_esp(b)) {
		if (esp_flash(m, b) != 0)
			return -1;
		return m->nic != NULL && strcmp(m->nic, "wifi") == 0 ? esp_efuse(m, b) : 0;
	}
	if (m->state != NULL && m->direct && mkdir(m->state, 0700) != 0 && errno != EEXIST) {
		snprintf(m->why, sizeof m->why, "%s: %s\n", m->state, strerror(errno));
		return -1;
	}
	if (mkdir(state_file(m, "tpm"), 0700) != 0 && errno != EEXIST) {
		snprintf(m->why, sizeof m->why, "the TPM's state directory: %s\n", strerror(errno));
		return -1;
	}
	/* The run's copy of what (state DIR) holds, where it is not used in
	 * place: the variables, and swtpm's files. */
	if (m->state != NULL && !m->direct) {
		snprintf(from, sizeof from, "%s/OVMF_VARS.fd", m->state);
		if (access(from, R_OK) == 0 && copy_file(from, run_file(m, "OVMF_VARS.fd")) != 0) {
			snprintf(m->why, sizeof m->why, "%.600s could not be copied\n", from);
			return -1;
		}
		for (unsigned i = 0u; i < sizeof tpm_files / sizeof tpm_files[0]; i++) {
			snprintf(from, sizeof from, "%s/tpm/%s", m->state, tpm_files[i]);
			snprintf(to, sizeof to, "%s/tpm/%s", m->run_state, tpm_files[i]);
			if (access(from, R_OK) == 0 && copy_file(from, to) != 0) {
				snprintf(m->why, sizeof m->why, "%.600s could not be copied\n", from);
				return -1;
			}
		}
	}
	if (m->secure) {
		snprintf(to, sizeof to, "%s", state_file(m, "OVMF_VARS.fd"));
		if (access(to, F_OK) != 0 && copy_file(SECURE_VARS, to) != 0) {
			snprintf(m->why, sizeof m->why, "the variables could not be made from %s\n",
			         SECURE_VARS);
			return -1;
		}
	}
	if (m->tpm) {
		char  dir[4500];
		char  ctrl[4500];
		pid_t child;

		snprintf(dir, sizeof dir, "dir=%s", state_file(m, "tpm"));
		snprintf(ctrl, sizeof ctrl, "type=unixio,path=%s", run_file(m, "swtpm.sock"));
		if ((child = fork()) < 0) {
			snprintf(m->why, sizeof m->why, "swtpm could not be started\n");
			return -1;
		}
		if (child == 0) {
			const char *argv[] = { "swtpm", "socket", "--tpm2", "--tpmstate", dir, "--ctrl", ctrl,
			                       "--terminate", NULL };
			int         null = open("/dev/null", O_RDWR | O_CLOEXEC);

			if (null < 0 || dup2(null, 0) < 0 || dup2(null, 1) < 0 ||
			    prctl(PR_SET_PDEATHSIG, SIGKILL, 0L, 0L, 0L) != 0)
				_exit(126);
			close_range(3u, ~0u, CLOSE_RANGE_CLOEXEC);
			execvp(argv[0], (char *const *)argv);
			dprintf(2, "swtpm, which (tpm true) needs: %s\n", strerror(errno));
			_exit(127);
		}
		m->swtpm = child;
		/* Its socket made before qemu dials it: five seconds at most. */
		for (int tries = 0; access(run_file(m, "swtpm.sock"), F_OK) != 0; tries++) {
			struct timespec pause = { 0, 50000000L };

			if (tries >= 100 || waitpid(child, NULL, WNOHANG) == child) {
				m->swtpm = 0;
				snprintf(m->why, sizeof m->why, "swtpm did not start: is it installed "
				         "(Debian's swtpm)?\n");
				return -1;
			}
			nanosleep(&pause, NULL);
		}
	}
	return 0;
}

/* The run's directory gone, by name, and swtpm with it: it ends when qemu
 * goes, --terminate, and is reaped here. */
static int open_up(const char *path, const struct stat *st, int type, struct FTW *ftw);
static int take_away(const char *path, const struct stat *st, int type, struct FTW *ftw);

static void release_state(hull_machine *m)
{
	char path[4400];

	/* virtualbox-pc's work, which a machine killed at the end of its grace
	 * could not take away itself: by the name exec_vbox gave it, under
	 * the cache's VirtualBox runs and nowhere else. */
	if (m->box == 2 && m->vbox_child > 0) {
		char dir[4096];

		if (hull_vbox_dir(dir, sizeof dir) == 0 &&
		    snprintf(path, sizeof path, "%s-run/pc-%ld", dir, (long)m->vbox_child) <
		    (int)sizeof path && access(path, F_OK) == 0) {
			nftw(path, open_up, 16, FTW_PHYS);
			nftw(path, take_away, 16, FTW_PHYS | FTW_DEPTH);
		}
		m->vbox_child = 0;
	}

	if (m->swtpm > 0) {
		kill(m->swtpm, SIGTERM);
		waitpid(m->swtpm, NULL, 0);
		m->swtpm = 0;
	}
	if (!m->run_state_made)
		return;
	unlink(run_file(m, "swtpm.sock"));
	unlink(run_file(m, "OVMF_VARS.fd"));
	unlink(run_file(m, "firmware.ihx"));
	unlink(run_file(m, "flash.bin"));
	unlink(run_file(m, "efuse.bin"));
	unlink(run_file(m, "display.qmp"));
	/* 86Box's, where it ran from the run's directory: its settings, its
	 * log and its CMOS, nvr/ITS-MACHINE.nvr. */
	unlink(run_file(m, "86box.cfg"));
	unlink(run_file(m, "86box.log"));
	{
		DIR *nvr = opendir(run_file(m, "nvr"));

		if (nvr != NULL) {
			struct dirent *entry;
			int            at = dirfd(nvr);

			while ((entry = readdir(nvr)) != NULL)
				if (entry->d_name[0] != '.')
					unlinkat(at, entry->d_name, 0);
			closedir(nvr);
			rmdir(run_file(m, "nvr"));
		}
	}
	for (unsigned i = 0u; i < sizeof tpm_files / sizeof tpm_files[0]; i++) {
		snprintf(path, sizeof path, "%s/tpm/%s", m->run_state, tpm_files[i]);
		unlink(path);
	}
	rmdir(run_file(m, "tpm"));
	rmdir(m->run_state);
	m->run_state_made = 0;
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
		/* VMware's own disk is often a descriptor, text naming its
		 * extents beside it, which qemu follows. */
		else if (memcmp(buffer, "KDMV", 4u) == 0 ||
		         memcmp(buffer, "# Disk DescriptorFile", 21u) == 0)
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

/* Whether `path` is under the user's store, as a tar: or pack:
 * collection's checkout is. */
static int in_store(const char *path)
{
	const char *data = getenv("XDG_DATA_HOME");
	const char *home = getenv("HOME");
	char        root[4200];
	char        real_root[PATH_MAX];
	char        real_path[PATH_MAX];
	size_t      n;

	if (data != NULL && data[0] == '/')
		snprintf(root, sizeof root, "%s/hull/store", data);
	else if (home != NULL)
		snprintf(root, sizeof root, "%s/.local/share/hull/store", home);
	else
		return 0;
	if (realpath(root, real_root) == NULL || realpath(path, real_path) == NULL)
		return 0;
	n = strlen(real_root);
	return strncmp(real_path, real_root, n) == 0 && (real_path[n] == '/' || real_path[n] == '\0');
}

/* For nftw: a directory made the user's to empty, then all removed. */
static int open_up(const char *path, const struct stat *st, int kind, struct FTW *at)
{
	(void)at;
	if (kind == FTW_D)
		chmod(path, (st->st_mode & 07777) | 0700);
	return 0;
}

static int take_away(const char *path, const struct stat *st, int kind, struct FTW *at)
{
	(void)st;
	(void)at;
	return (kind == FTW_DP ? rmdir(path) : unlink(path)) == 0 ? 0 : -1;
}

/* The disk's image as the machine left it taken apart into its collection
 * again (`hull media import`), the new collection swapped in whole and the
 * old removed, so a reader sees one or the other; where the import fails,
 * the image is kept beside the collection as COLLECTION.unsaved.img, the
 * writes not lost. What it did, said on the machine's stderr. */
static void write_back(hull_machine *m, medium *d)
{
	hull_media_report report = { 0 };
	char              image[64];
	char              fresh[4300];
	char              said[4800];
	char              detail[4400];

	/* A served disk: its server joined, qemu gone; nothing written is
	 * nothing to take apart, and otherwise the overlay is made the whole
	 * disk -- each block the guest did not write filled from the files,
	 * where it is not zero -- and taken apart as an image would be. */
	if (d->serving) {
		uint8_t  block[HULL_NBD_BLOCK];
		uint64_t bytes = 0u;
		struct stat st;

		close(d->nbd);
		d->nbd = 0;
		pthread_join(d->server, NULL);
		d->serving = 0;
		if (d->writes.count == 0u) {
			if (d->writes.taken != 0u)
				snprintf(said, sizeof said, "hull: %s: its writes taken in %llu times as the "
				         "machine ran, nothing left at power-off\n", d->collection,
				         (unsigned long long)d->writes.taken);
			else
				snprintf(said, sizeof said, "hull: %s: nothing written, nothing to write back\n",
				         d->collection);
			host->on_data(m->core, NULL, 2, (const uint8_t *)said, strlen(said));
			return;
		}
		if (fstat(d->fd, &st) == 0)
			bytes = (uint64_t)st.st_size;
		/* Taken as the server takes them as the machine runs: only what
		 * was written read again. Where that fails, the whole disk is
		 * made and taken apart as an image, and kept where that fails. */
		uint64_t blocks = d->writes.count;

		if (bytes != 0u &&
		    hull_nbd_take(&d->writes, bytes, &report, detail, sizeof detail) == 0) {
			snprintf(said, sizeof said, "hull: %s written back: %llu blocks written, %llu files, "
			         "%llu bytes not understood\n", d->collection, (unsigned long long)blocks,
			         report.files, report.patch_bytes);
			host->on_data(m->core, NULL, 2, (const uint8_t *)said, strlen(said));
			return;
		}
		for (uint64_t at = 0u; at < bytes; at += HULL_NBD_BLOCK) {
			uint64_t index = at / HULL_NBD_BLOCK;
			size_t   n = bytes - at < HULL_NBD_BLOCK ? (size_t)(bytes - at) : HULL_NBD_BLOCK;
			static const uint8_t zero[HULL_NBD_BLOCK];

			if (((d->writes.written[index / 8u] >> (index % 8u)) & 1u) ||
			    at >= hull_media_map_size(d->writes.map))
				continue;
			if (at + n > hull_media_map_size(d->writes.map))
				n = (size_t)(hull_media_map_size(d->writes.map) - at);
			if (hull_media_map_read(d->writes.map, at, block, n) != 0 ||
			    (memcmp(block, zero, n) != 0 &&
			     pwrite(d->fd, block, n, (off_t)at) != (ssize_t)n)) {
				snprintf(said, sizeof said, "hull: %s was not written back: its files could not "
				         "be read with the writes; they are lost\n", d->collection);
				host->on_data(m->core, NULL, 2, (const uint8_t *)said, strlen(said));
				return;
			}
		}
	}
	snprintf(image, sizeof image, "/proc/self/fd/%d", d->fd);
	snprintf(fresh, sizeof fresh, "%s.hull-new", d->collection);
	if (fdatasync(d->fd) != 0 ||
	    hull_media_import(image, fresh, &report, detail, sizeof detail) != 0 ||
	    renameat2(AT_FDCWD, fresh, AT_FDCWD, d->collection, RENAME_EXCHANGE) != 0) {
		char  unsaved[4300];
		int   out;
		off_t at = 0;
		struct stat st;

		if (detail[0] == '\0')
			snprintf(detail, sizeof detail, "%s", strerror(errno));
		snprintf(unsaved, sizeof unsaved, "%s.unsaved.img", d->collection);
		out = open(unsaved, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
		if (out >= 0 && fstat(d->fd, &st) == 0) {
			while (at < st.st_size && copy_file_range(d->fd, &at, out, NULL,
			                                          (size_t)(st.st_size - at), 0) > 0)
				;
		}
		snprintf(said, sizeof said, "hull: %s was not written back (%s): %s\n", d->collection,
		         detail, out >= 0 && at == st.st_size ? "its writes are in the image beside it, "
		         ".unsaved.img" : "and its writes are lost");
		if (out >= 0)
			close(out);
		host->on_data(m->core, NULL, 2, (const uint8_t *)said, strlen(said));
		return;
	}
	/* The old collection, now at the new one's name, goes. */
	nftw(fresh, open_up, 16, FTW_PHYS);
	nftw(fresh, take_away, 16, FTW_PHYS | FTW_DEPTH);
	if (d->writes.count != 0u)
		snprintf(said, sizeof said, "hull: %s written back: %llu blocks written, %llu files, %llu "
		         "bytes not understood\n", d->collection, (unsigned long long)d->writes.count,
		         report.files, report.patch_bytes);
	else
		snprintf(said, sizeof said, "hull: %s written back: %llu files, %llu bytes not "
		         "understood\n", d->collection, report.files, report.patch_bytes);
	host->on_data(m->core, NULL, 2, (const uint8_t *)said, strlen(said));
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

/* A disk's collection under (mutable overlay) served where its files lie,
 * by NBD on a socketpair (engine/nbd.c), held to its hash first by reading
 * it through, and no image made of it: qemu's snapshot takes the writes,
 * as for an image: disk. 1 where served, 0 where the collection is not one
 * to serve so, -1 with m->why. */
static int serve_collection(hull_machine *m, medium *d)
{
	char            detail[512];
	uint64_t        bytes;
	hull_media_map *map;
	int             pair[2];

	if (d->type != 'd' || !d->exact)
		return 0;
	if ((map = hull_media_map_open(d->collection, &bytes, detail, sizeof detail)) == NULL ||
	    hull_media_map_verify(map, detail, sizeof detail) != 0) {
		hull_media_map_close(map);
		snprintf(m->why, sizeof m->why, "a medium's disk cannot be served: %s\n", detail);
		return -1;
	}
	/* Grown, never shrunk: a disk smaller than its files would cut them. */
	if (d->size != 0u && d->size < bytes) {
		hull_media_map_close(map);
		snprintf(m->why, sizeof m->why, "a medium's (size ...) is smaller than its collection's "
		         "disk, %llu bytes: a disk is grown, not cut\n", (unsigned long long)bytes);
		return -1;
	}
	if (d->size > bytes)
		bytes = d->size;
	if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, pair) != 0) {
		hull_media_map_close(map);
		snprintf(m->why, sizeof m->why, "a medium's disk cannot be served: %s\n", strerror(errno));
		return -1;
	}
	/* Under (mutable direct) the guest's writes are kept, block by block,
	 * in a sparse overlay of the cache's, unnamed, and go back into the
	 * collection at power-off (write_back); else qemu's snapshot has them
	 * and the server is read-only. */
	if (d->write_back) {
		size_t bits = (size_t)((bytes + HULL_NBD_BLOCK - 1u) / HULL_NBD_BLOCK + 7u) / 8u;

		d->writes.collection = d->collection;  /* live: taken in at each pause */
		if ((d->writes.overlay = open(m->cache, O_TMPFILE | O_RDWR | O_CLOEXEC, 0600)) < 0 ||
		    ftruncate(d->writes.overlay, (off_t)bytes) != 0 ||
		    (d->writes.written = calloc(1u, bits != 0u ? bits : 1u)) == NULL ||
		    hull_nbd_serve_writes(map, bytes, pair[1], &d->writes, &d->server) != 0) {
			snprintf(m->why, sizeof m->why, "a medium's disk cannot be served: %s\n",
			         strerror(errno));
			if (d->writes.overlay >= 0)
				close(d->writes.overlay);
			hull_media_map_close(map);
			close(pair[0]);
			close(pair[1]);
			return -1;
		}
		d->serving = 1;
		d->fd = d->writes.overlay;
	} else if (hull_nbd_serve(map, bytes, pair[1]) != 0) {
		hull_media_map_close(map);
		close(pair[0]);
		close(pair[1]);
		snprintf(m->why, sizeof m->why, "a medium's disk cannot be served: %s\n", strerror(errno));
		return -1;
	}
	d->nbd = pair[0];
	return 1;
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
		                   strcmp(type, "disk") == 0 ? 'd' : strcmp(type, "floppy") == 0 ? 'f' :
		                   strcmp(type, "usb") == 0 ? 'u' : 0;
		const char *wrong = NULL;

		/* Any other word had been taken for a floppy, silently. */
		if (kind == 0)
			wrong = "a medium's type is cdrom, disk, floppy or usb";
		else if (path == NULL)
			wrong = "a pc machine's medium is a dir: collection or an image:";
		else if (image)
			wrong = add_image(m, kind, path);
		else if (m->media_count == PC_MAX_MEDIA)
			wrong = "there are too many media: four on IDE and two floppy drives are a PC's";
		else {
			d->type = kind;
			d->fd   = -1;
			m->media_count++;
			wrong = add_collection(m, d, path);
			/* (mutable direct): a disk's or floppy's writes go back into
			 * its collection when the machine powers off -- not a store's
			 * checkout, which a tar: or pack: collection is. */
			if (wrong == NULL && kind != 'c' && kind != 'u' && m->direct) {
				if (in_store(path))
					wrong = "(mutable direct) writes a collection back, and a tar: or pack: "
					        "one is the store's: give it as a dir: collection";
				else
					d->write_back = 1;
			}
		}
		/* (size ...): a disk collection served larger than it is, zeros
		 * after it, so a guest has room to grow into (section 3.13). */
		if (wrong == NULL) {
			char *size = hull_access_value(doc, &item, "size");

			if (size != NULL) {
				char              *end;
				unsigned long long n = strtoull(size, &end, 10);
				unsigned           shift = *end == 'K' ? 10u : *end == 'M' ? 20u :
				                           *end == 'G' ? 30u : *end == 'T' ? 40u : 0u;

				if (kind != 'd' || image)
					wrong = "a medium's size is a disk collection's: the disk served that large";
				else if (n > (UINT64_MAX >> shift) || (n << shift) / HULL_NBD_BLOCK == 0u)
					wrong = "a medium's size is too large or too small";
				else
					d->size = (uint64_t)n << shift;
				free(size);
			}
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
	int   secure = firmware != NULL && strcmp(firmware, "uefi-secure") == 0;

	free(firmware);
	/* VirtualBox's own EFI, nothing of the host's. */
	if (m->box == 2) {
		m->vbox_efi = uefi || secure;
		return secure ? "VirtualBox's PC is given UEFI without Secure Boot here: (firmware uefi)" :
		       NULL;
	}
	/* Secure boot, as Windows 11 asks: OVMF's secure-boot build, which
	 * needs SMM and so q35, and a variable store enrolled with Microsoft's
	 * keys, the machine's own copy (prepare_state()). */
	if (secure) {
		if (m->machine_type == NULL || strcmp(m->machine_type, "q35") != 0)
			return "(firmware uefi-secure) is q35's, SMM being what keeps its variables: "
			       "(machine q35)";
		if (access(SECURE_CODE, R_OK) != 0 || access(SECURE_VARS, R_OK) != 0)
			return "(firmware uefi-secure) needs OVMF's secure-boot build and Microsoft's keys, "
			       SECURE_CODE " and " SECURE_VARS ": Debian's package is ovmf";
		m->secure = 1;
		return NULL;
	}
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
	/* (devices (pci ...)): a card of the host's given whole by VFIO, as a
	 * vm's are (section 4.13) -- a GPU being Windows' one route to native
	 * 3D (section 5.2). A PC's alone, a board having no PCI bus. */
	{
		const char *why = hull_access_devices(doc, machine, m->device, &m->device_count);

		if (why != NULL) {
			free(source);
			free(display);
			return why;
		}
		for (unsigned i = 0u; i < m->device_count; i++)
			if (m->device[i].kind != HULL_DEVICE_PCI || m->device[i].root) {
				free(source);
				free(display);
				return "a pc machine takes a card of the host's by (devices (pci ADDRESS)); "
				       "its USB, serial and parallel ports are not given it yet";
			}
		if (m->device_count > PC_MAX_CARDS) {
			free(source);
			free(display);
			return "a pc machine takes four cards of the host's at most";
		}
		if (m->device_count > 0u && board_named(m->machine_type) != NULL) {
			free(source);
			free(display);
			return "a board has no PCI bus for (devices (pci ...))";
		}
	}
	m->vnc = display != NULL && strcmp(display, "vnc") == 0;
	m->local = display != NULL && strcmp(display, "local") == 0;
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
		hull_sexpr_form box;

		if (m->box && hull_access_find(doc, &entry, "86box", &box)) {
			m->box_machine = hull_access_value(doc, &box, "machine");
			m->box_cpu     = hull_access_value(doc, &box, "cpu");
			m->box_video   = hull_access_value(doc, &box, "video");
			m->box_sound   = hull_access_value(doc, &box, "sound");
		}

		if (hull_access_find(doc, &entry, "pc", &pc)) {
			m->machine_type = hull_access_value(doc, &pc, "machine");
			m->vga          = hull_access_value(doc, &pc, "vga");
			{
				char *text = hull_access_value(doc, &pc, "gpu");

				if (text != NULL) {
					m->gpu = strcmp(text, "venus") == 0 ? 2 : strcmp(text, "virgl") == 0;
					free(text);
				}
			}
			m->cpu          = hull_access_value(doc, &pc, "cpu");
			m->tpm          = hull_access_true(doc, &pc, "tpm");
			{
				char *text = hull_access_value(doc, &pc, "boot_key");

				if (text != NULL) {
					m->boot_key = strtol(text, NULL, 10);
					free(text);
					if (m->boot_key < 1 || m->boot_key > 3600)
						return "pc's boot_key is seconds, at most an hour's";
				}
			}
			{
				char *smc = hull_access_value(doc, &pc, "smc");

				if (smc != NULL) {
					m->smc = hull_access_path(doc, smc);
					free(smc);
					if (m->smc == NULL)
						return "the SMC key's path cannot be read";
				}
			}
			{
				char *state = hull_access_value(doc, &pc, "state");

				if (state != NULL) {
					m->state = hull_access_path(doc, state);
					free(state);
					if (m->state == NULL)
						return "the state's path cannot be read";
				}
			}
			m->accel        = hull_access_value(doc, &pc, "accel");
			m->nic          = hull_access_value(doc, &pc, "nic");
			m->sound        = hull_access_value(doc, &pc, "sound");
			m->lora         = hull_access_value(doc, &pc, "lora");
			{
				char *pointer = hull_access_value(doc, &pc, "pointer");

				m->tablet = pointer != NULL && strcmp(pointer, "tablet") == 0;
				free(pointer);
			}
			if ((wrong = read_firmware(m, doc, &pc)) != NULL)
				return wrong;
		}
	}
	/* A LoRa radio, on the boards that carry an SX1276 as a Heltec's is
	 * wired, and a name for its air. */
	if (m->lora != NULL && !has_sx1276(board_named(m->machine_type)))
		return "a LoRa radio is on heltec-wifi-lora-32, heltec-wifi-lora-32-v2, ttgo-lora32, "
		       "lilygo-t3-s3 (an SX1276 each) and heltec-wifi-lora-32-v3 (an SX1262) alone";
	if (m->lora != NULL && (strlen(m->lora) > 64u || strchr(m->lora, '/') != NULL))
		return "a LoRa air's name is at most 64 characters and has no /";
	/* VirtualBox's PC, in a qemu vm of its own: its media image files it
	 * opens -- VDI, VMDK, VHD, a raw disk by a descriptor -- by absolute
	 * path, its COM1 the run's console. A qcow2 it cannot open, qemu's
	 * version 3 being past VirtualBox's reader. */
	if (m->box == 2) {
		if ((wrong = read_media(m, doc, machine)) != NULL)
			return wrong;
		for (unsigned i = 0u; i < m->media_count; i++) {
			uint8_t head[4] = { 0 };
			int     fd;

			if (m->media[i].path == NULL || m->media[i].path[0] != '/')
				return "VirtualBox takes a medium as an image: file; a collection is not "
				       "reassembled for it";
			if (m->media[i].type == 'u')
				return "VirtualBox is given no USB stick here: a disc carries what a stick would";
			if (m->media[i].type == 'd' && (fd = open(m->media[i].path, O_RDONLY | O_CLOEXEC)) >= 0) {
				ssize_t got = read(fd, head, sizeof head);

				close(fd);
				if (got == (ssize_t)sizeof head && memcmp(head, "QFI\xfb", 4u) == 0)
					return "VirtualBox cannot open a qcow2: `qemu-img convert -O vdi` makes "
					       "a VDI of it, which both run";
			}
		}
		return NULL;
	}
	/* 86Box's machine: a PC of a period, in its window, its media images
	 * by path, its COM1 the run's console. */
	if (m->box) {
		if (m->display == NULL)
			return "86Box runs a machine in its window: (display window)";
		if (m->box_machine == NULL)
			return "86Box needs its machine named: (tuning (86box (machine \"NAME\"))), "
			       "a name of 86Box's, as roms/machines has it";
		if ((wrong = read_media(m, doc, machine)) != NULL)
			return wrong;
		for (unsigned i = 0u; i < m->media_count; i++)
			if (m->media[i].path == NULL)
				return "86Box takes a medium as an image: file; a collection is not "
				       "reassembled for it yet";
		return NULL;
	}
	/* A board: its firmware, the image: source, and none of a PC's parts. */
	if (board_named(m->machine_type) != NULL) {
		if (m->media_count != 1u || m->media[0].path == NULL)
			return "a board boots its firmware, an image: source: (source \"image:FIRMWARE\")";
		if (hull_access_find(doc, machine, "media", &entry))
			return "a board has no drives: its firmware is its image: source alone";
		/* A board with Ethernet has one card, its chip's -- an ESP32's
		 * open_eth, the LM3S6965's own MAC, the MPS2's LAN9118: on (network nat), with its
		 * ports, or on one segment. Any other board's serial port is all
		 * it has. */
		if (board_named(m->machine_type)->nic != NULL) {
			if ((m->network ? 1u : 0u) + m->segment_count > 1u)
				return "a board has one Ethernet: (network nat) or one segment, not both";
			/* An ESP32's WiFi, its own MAC to an access point in qemu,
			 * in place of its Ethernet, on the same network. */
			if (m->nic != NULL && (strcmp(m->nic, "wifi") != 0 ||
			                       !esp_wifi_chip(board_named(m->machine_type))))
				return "a board's Ethernet is its chip's: no (nic …) but an ESP32's or a C3's (nic wifi)";
		} else if (m->network || m->segment_count != 0u || m->port_count != 0u) {
			return "a board's serial port is its one way in: no network, segments or ports";
		}
		if (m->vnc || m->display != NULL || m->vga != NULL || m->sound != NULL || m->gpu != 0 ||
		    m->tablet)
			return "a board has no screen or sound of qemu's: its serial port is what it says";
		if (m->smc != NULL)
			return "a board has no Apple SMC: (smc PATH) is a Mac's, on a q35 PC";
		if (m->memory != NULL || m->cpus != NULL || m->cpu != NULL)
			return "a board's memory and CPU are its microcontroller's";
		if (m->tpm || m->firmware != NULL || m->secure || m->state != NULL || m->boot_key != 0)
			return "a board's firmware is its image: no BIOS, TPM, state or boot key";
		return NULL;
	}
	if (m->vnc && m->address[0] == '\0')
		return "the plan gives (display vnc) no address";
	/* A PowerPC Mac: OpenBIOS its firmware, its parts a G4's or a G3's,
	 * none of a PC's. */
	if (is_mac(m->machine_type)) {
		if (m->cpu != NULL || m->smc != NULL || m->tpm || m->firmware != NULL || m->secure)
			return "a PowerPC Mac's processor and firmware are its own, OpenBIOS: no (cpu ...), "
			       "(smc ...), (tpm ...) or (firmware ...)";
		if (m->sound != NULL || m->gpu != 0)
			return "a PowerPC Mac here has no sound card or 3D of qemu's";
	}
	/* (gpu virgl|venus): the display card is virtio-vga-gl, a VGA card
	 * for the firmware and the boot, and the host's GPU behind it for a
	 * guest with Mesa's virgl or Venus driver, as on a vm (section 5). */
	if (m->gpu != 0 && m->display == NULL && !m->vnc && !m->local)
		return "(gpu virgl) and (gpu venus) are a screen's 3D: the machine needs "
		       "(display window) or (display vnc), or (display local)";
	if (m->gpu != 0 && m->vga != NULL && strcmp(m->vga, "virtio") != 0)
		return "(gpu virgl) and (gpu venus) are virtio-vga's: (vga virtio), or no (vga ...)";
	if ((wrong = read_media(m, doc, machine)) != NULL)
		return wrong;
	for (unsigned i = 0u; m->box && i < m->media_count; i++)
		if (m->media[i].type == 'u')
			return "86Box is given no USB stick here: its media are IDE disks, a CD and floppies";
	for (unsigned i = 0u; is_mac(m->machine_type) && i < m->media_count; i++)
		if (m->media[i].type == 'f')
			return "a PowerPC Mac has no floppy drive: its media are IDE disks and CDs";
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

/* In the child: a board's emulator, exec'd. Returns to fail. On qemu, its
 * machine, its firmware, its first UART on stdio and the monitor beside it,
 * nothing of a PC's. */
static void exec_board(const hull_machine *m, const board *b)
{
	char  drive[4500];
	char  efuse[4500];
	char  emulator[4200];
	char  lora_emulator[4200];
	char  rom[4200];
	char  air[64];
	char  nic[96u + PC_MAX_PORTS * 64u];
	char  netdev[64];
	char  monitor[48];
	char  serial2[48];
	char *argv[40];
	int   n = 0;
	int   monitor_fd;
	int   wifi = m->nic != NULL && strcmp(m->nic, "wifi") == 0;
	const char *wifi_model = b != NULL && b->chip != NULL && strcmp(b->chip, "esp32c3") == 0 ? "esp32c3_wifi" : "esp32_wifi";

	/* s51: the UART on descriptors 3 and 4, the machine's stdin and
	 * stdout; its command console on a pipe whose writing end it holds
	 * itself, so that it never reads an end and quits, what it says
	 * thrown away. No monitor: what needs qemu's says so. */
	if (strcmp(b->emulator, "ucsim_51") == 0) {
		int   console[2];
		int   serial[2];
		int   null = open("/dev/null", O_WRONLY);
		int   saved;
		pid_t copier;

		/* The input is a socket, which s51 cannot open by a path: a pipe
		 * between, a copier of this process's own feeding it. */
		if (null < 0 || pipe(console) != 0 || pipe(serial) != 0 || (copier = fork()) < 0)
			return;
		if (copier == 0) {
			char    buffer[512];
			ssize_t got;

			/* Nothing of the machine's held but its input, so that the
			 * engine reads its output's end; gone with s51. */
			close(serial[0]);
			close(console[0]);
			close(console[1]);
			if (dup2(null, 1) != 1 || dup2(null, 2) != 2 ||
			    (serial[1] != 3 && dup2(serial[1], 3) != 3))
				_exit(1);
			serial[1] = 3;
			close_range(4u, ~0u, 0);
			if (prctl(PR_SET_PDEATHSIG, SIGKILL, 0L, 0L, 0L) != 0 || getppid() == 1)
				_exit(1);
			while ((got = read(0, buffer, sizeof buffer)) > 0 || (got < 0 && errno == EINTR))
				if (got > 0 && write(serial[1], buffer, (size_t)got) != got)
					break;
			_exit(0);
		}
		close(serial[1]);
		if (dup2(serial[0], 3) != 3 || dup2(1, 4) != 4 || dup2(console[0], 0) != 0 ||
		    dup2(null, 1) != 1)
			return;
		argv[n++] = "ucsim_51";
		argv[n++] = "-q";
		argv[n++] = "-t";
		argv[n++] = "C52";
		argv[n++] = "-X";
		argv[n++] = "11.0592M";
		argv[n++] = "-S";
		argv[n++] = "in=/dev/fd/3,out=/dev/fd/4";
		argv[n++] = "-g";
		argv[n++] = (char *)run_file(m, "firmware.ihx");
		argv[n] = NULL;
		setenv("HOME", "/nonexistent", 1);
		/* What s51 says on stderr is only that its serial files are no
		 * terminals, written as it starts, so into the firmware's first
		 * line under load: "re", two warnings, "ady". Thrown away, and
		 * stderr given back for this process's own word where it
		 * cannot be started. */
		if ((saved = fcntl(2, F_DUPFD_CLOEXEC, 5)) < 0 || dup2(null, 2) != 2)
			return;
		execvp(argv[0], argv);
		dup2(saved, 2);
		return;
	}
	if ((monitor_fd = fcntl(m->monitor_peer, F_DUPFD, 3)) < 0)
		return;
	snprintf(monitor, sizeof monitor, "socket,id=monitor,fd=%d", monitor_fd);
	argv[n++] = (char *)b->emulator;
	argv[n++] = "-nodefaults";
	argv[n++] = "-no-user-config";
	argv[n++] = "-machine";
	argv[n++] = (char *)(b->chip != NULL ? b->chip : b->name);
	if (m->lora != NULL) {
		int fd = fcntl(m->lora_fd, F_DUPFD, 3);

		if (fd < 0 || esp_lora_emulator(lora_emulator, sizeof lora_emulator) != 0 ||
		    hull_esp_dir(rom, sizeof rom) != 0)
			return;
		snprintf(air, sizeof air, "socket,id=air,fd=%d", fd);
		strncat(rom, "/qemu/share/qemu", sizeof rom - strlen(rom) - 1u);
		/* The chip's machine, its radio on the air: an ESP32's on VSPI, an
		 * S3's on SPI2 (engine/esp-lora.patch). */
		{
			static __thread char with_radio[64];

			snprintf(with_radio, sizeof with_radio, "%s,lora=air%s",
			         b->chip != NULL ? b->chip : b->name,
			         strcmp(b->name, "heltec-wifi-lora-32-v3") == 0 ? ",lora-chip=sx1262" : "");
			argv[n - 1] = with_radio;
		}
		argv[n++] = "-chardev";
		argv[n++] = air;
		argv[n++] = "-L";
		argv[n++] = rom;
	}
	argv[n++] = "-display";
	argv[n++] = "none";
	argv[n++] = "-serial";
	argv[n++] = "stdio";
	/* The second UART, `hull exec ID -- console 2`: a socket nobody reads
	 * until a console joins it, so what the board says there waits, as
	 * far as the socket holds, rather than being lost. A board with one
	 * UART leaves qemu's second serial port unused. */
	if (m->serial2_peer >= 0) {
		int fd = fcntl(m->serial2_peer, F_DUPFD, 3);

		if (fd < 0)
			return;
		snprintf(serial2, sizeof serial2, "socket,id=serial2,fd=%d", fd);
		argv[n++] = "-chardev";
		argv[n++] = serial2;
		argv[n++] = "-serial";
		argv[n++] = "chardev:serial2";
	}
	argv[n++] = "-chardev";
	argv[n++] = monitor;
	argv[n++] = "-mon";
	argv[n++] = "chardev=monitor,mode=control";
	if (is_esp(b)) {
		argv[n++] = "-drive";
		snprintf(drive, sizeof drive, "file=%.4400s,if=mtd,format=raw", run_file(m, "flash.bin"));
		argv[n++] = drive;
		if (esp_emulator(b, emulator, sizeof emulator) != 0)
			return;
		if (wifi && esp_wifi_emulator(b, lora_emulator, sizeof lora_emulator) != 0)
			return;
		argv[0] = m->lora != NULL || wifi ? lora_emulator : emulator;
		/* Its WiFi's MAC, from the efuse esp_efuse made. */
		if (wifi) {
			snprintf(efuse, sizeof efuse, "file=%.4400s,if=none,format=raw,id=efuse",
			         run_file(m, "efuse.bin"));
			argv[n++] = "-drive";
			argv[n++] = efuse;
			argv[n++] = "-global";
			argv[n++] = strcmp(b->chip, "esp32c3") == 0 ?
			            "driver=nvram.esp32c3.efuse,property=drive,value=efuse" :
			            "driver=nvram.esp32.efuse,property=drive,value=efuse";
		}
	} else {
		argv[n++] = (char *)b->load;
		argv[n++] = (char *)m->media[0].path;
	}
	/* Its card: slirp's NAT with the ports forwarded to it, as a PC's
	 * card is, or the segment's wire, by the old -net form, as the card
	 * is the machine's own and no -device. */
	if (b->nic != NULL && m->network) {
		size_t at = (size_t)snprintf(nic, sizeof nic, "user,model=%s",
		                             wifi ? wifi_model : b->nic);

		for (unsigned i = 0u; i < m->port_count && at < sizeof nic; i++)
			at += (size_t)snprintf(nic + at, sizeof nic - at, ",hostfwd=%s:%s:%u-:%u",
			                       m->port_udp[i] ? "udp" : "tcp", m->address,
			                       m->port_host[i], m->port_guest[i]);
		if (at >= sizeof nic)
			return;
		argv[n++] = "-nic";
		argv[n++] = nic;
	} else if (b->nic != NULL && m->segment_count == 1u) {
		unsigned char mac[6];
		int           fd = fcntl(m->segment_fd[0], F_DUPFD, 3);

		if (fd < 0)
			return;
		hull_access_segment_mac(m->address, m->segment[0], 0u, mac);
		snprintf(netdev, sizeof netdev, "stream,id=s0,addr.type=fd,addr.str=%d", fd);
		snprintf(nic, sizeof nic, "nic,model=%s,netdev=s0,"
		         "macaddr=%02x:%02x:%02x:%02x:%02x:%02x", wifi ? wifi_model : b->nic,
		         mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
		argv[n++] = "-netdev";
		argv[n++] = netdev;
		argv[n++] = "-net";
		argv[n++] = nic;
	}
	argv[n] = NULL;
	setenv("HOME", "/nonexistent", 1);
	execvp(argv[0], argv);
}

/* In the child: a PC's qemu, the drives and the rest, exec'd. Returns to
 * fail. */
static void exec_pc(const hull_machine *m)
{
	char    *argv[PC_MAX_ARGS];
	char     machine[128];
	char     smp[64];
	char     vnc[48];
	char     monitor[48];
	char     display_monitor[4500];
	int      monitor_fd;
	unsigned n = 0u;
	unsigned ide = 0u;
	unsigned floppy = 0u;
	unsigned cdrom = 0u;
	unsigned sticks = 0u;
	char     boot[16];
	const char *accel = m->accel != NULL ? m->accel : "auto";

	static __thread char mac_qemu[4200];
	const int            mac = is_mac(m->machine_type);

	if (strcmp(accel, "auto") == 0)
		accel = access("/dev/kvm", R_OK | W_OK) == 0 && !mac ? "kvm" : "tcg";
	/* A Mac's: its PMU's ADB keyboard and mouse, as Mac OS X expects. */
	snprintf(machine, sizeof machine, "%s%s,accel=%s%s",
	         m->machine_type != NULL ? m->machine_type : "pc",
	         mac && strcmp(m->machine_type, "mac99") == 0 ? ",via=pmu" : "", accel,
	         m->secure ? ",smm=on" : "");
	snprintf(boot, sizeof boot, "order=%c", m->boot);
	if (mac && mac_emulator(mac_qemu, sizeof mac_qemu) != 0)
		return;
	argv[n++] = mac ? mac_qemu : "qemu-system-x86_64";
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
	/* (cpu ...): a model a system needs, Windows 11's SSE4.2 and POPCNT
	 * that qemu's own qemu64 lacks, or an old one an old system expects;
	 * none, qemu's default, as a system was installed under before the
	 * key. host is KVM's alone: under TCG, max. */
	if (m->cpu != NULL) {
		argv[n++] = "-cpu";
		argv[n++] = strcmp(m->cpu, "host") == 0 ? (strcmp(accel, "kvm") == 0 ? "host" : "max") :
		            strcmp(m->cpu, "skylake") == 0 ? "Skylake-Client" :
		            /* macOS's, as the OSX-KVM projects give it: Intel's by
		             * vendor, its TSC invariant and its frequency told. */
		            strcmp(m->cpu, "penryn") != 0 ? m->cpu :
		            strcmp(accel, "kvm") == 0 ?
		            "Penryn,kvm=on,vendor=GenuineIntel,+invtsc,vmware-cpuid-freq=on,+ssse3,"
		            "+sse4.2,+popcnt" :
		            "Penryn,vendor=GenuineIntel,+ssse3,+sse4.2,+popcnt";
	}
	/* Apple's SMC, which macOS asks for its key before it boots, the key
	 * the user's (read in launch()); and its keyboard and pointer on an
	 * xHCI, macOS having no PS/2 driver and, since Big Sur, no UHCI. */
	if (m->smc != NULL) {
		static __thread char smc[96];

		snprintf(smc, sizeof smc, "isa-applesmc,osk=%s", m->osk);
		argv[n++] = "-device";
		argv[n++] = smc;
		argv[n++] = "-smbios";
		argv[n++] = "type=2";
		argv[n++] = "-device";
		argv[n++] = "qemu-xhci,id=apple";
		argv[n++] = "-device";
		argv[n++] = "usb-kbd,bus=apple.0";
		argv[n++] = "-device";
		argv[n++] = "usb-tablet,bus=apple.0";
	}
	/* 3D: virtio-vga-gl, VGA for the firmware and the boot, the guest's
	 * OpenGL (virgl) or Vulkan (Venus) rendered on the host's GPU. */
	argv[n++] = "-vga";
	argv[n++] = m->gpu != 0 ? "none" : m->vga != NULL ? m->vga : "std";
	if (m->gpu != 0) {
		argv[n++] = "-device";
		argv[n++] = m->gpu == 2 ? "virtio-vga-gl,venus=on,blob=on,hostmem=4G" : "virtio-vga-gl";
	}
	/* A card on q35 goes behind a PCIe root port of its own, as a card in
	 * a real slot is: a driver reading its link from the bridge above it,
	 * ib_qib's among them, oopsed on the root bus, where there is none
	 * (section 5.0). i440fx has no PCIe to put one on. */
	for (unsigned i = 0u; i < m->device_count; i++) {
		static __thread char port[PC_MAX_CARDS][96];
		static __thread char card[PC_MAX_CARDS][160];
		int q35 = m->machine_type != NULL && strstr(m->machine_type, "q35") != NULL;

		if (q35) {
			snprintf(port[i], sizeof port[i], "pcie-root-port,id=hullport%u,chassis=%u,slot=%u", i,
			         i + 1u, i + 1u);
			argv[n++] = "-device";
			argv[n++] = port[i];
		}
		snprintf(card[i], sizeof card[i], q35 ? "vfio-pci,host=%s,bus=hullport%u" : "vfio-pci,host=%s",
		         m->device[i].host.port, i);
		argv[n++] = "-device";
		argv[n++] = card[i];
	}
	/* VNC's display 0 is port 5900, at the machine's own address. */
	snprintf(vnc, sizeof vnc, "vnc=%s:0", m->address);
	/* A screen by VNC is qemu's D-Bus display as well, for hull-gui to
	 * show without VNC's copies (section 5.2): its monitor display.qmp in
	 * the run's directory, linked as hull/display/ADDRESS. */
	/* With 3D, the window renders by OpenGL, and VNC's frames are
	 * rendered headless by EGL on the host's render node; hull-gui's
	 * screen takes each frame as a dmabuf, which excludes VNC. */
	argv[n++] = "-display";
	argv[n++] = m->local_display ? (m->gpu != 0 ? "dbus,p2p=yes,gl=on" : "dbus,p2p=yes") :
	            m->gpu != 0 ? (m->vnc ? "egl-headless" : "sdl,gl=on") :
	            m->vnc ? vnc : m->display != NULL ? "sdl" : "none";
	if (m->gpu != 0 && !m->local_display && m->vnc) {
		snprintf(vnc, sizeof vnc, "%s:0", m->address);
		argv[n++] = "-vnc";
		argv[n++] = vnc;
	}
	if (m->local_display) {
		if (m->vnc) {
			snprintf(vnc, sizeof vnc, "%s:0", m->address);
			argv[n++] = "-vnc";
			argv[n++] = vnc;
		}
		snprintf(display_monitor, sizeof display_monitor,
		         "socket,id=display,path=%s,server=on,wait=off", run_file(m, "display.qmp"));
		argv[n++] = "-chardev";
		argv[n++] = display_monitor;
		argv[n++] = "-mon";
		argv[n++] = "chardev=display,mode=control";
	}
	/* A USB tablet: a pointer at absolute positions, as hull-gui's screen
	 * and VNC give them, where a PS/2 mouse moves by deltas alone and a
	 * guest's pointer drifts from the viewer's. On a UHCI of its own, the
	 * one every Windows from 98 and every Linux drives, on q35 too; a Mac's
	 * is on its xHCI, above. */
	if (m->tablet && m->smc == NULL) {
		argv[n++] = "-device";
		argv[n++] = mac ? "pci-ohci,id=pointer" : "piix3-usb-uhci,id=pointer";
		argv[n++] = "-device";
		argv[n++] = "usb-tablet,bus=pointer.0";
	}
	argv[n++] = "-boot";
	argv[n++] = boot;
	/* Its saved state, `hull stop --save`'s: the machine carries on from
	 * where it was, its memory and disks as they were then. */
	if (m->resume) {
		argv[n++] = "-loadvm";
		argv[n++] = "hull-saved";
	}
	/* A card, heard where there is a window, SDL's audio beside its
	 * video, or where hull-gui shows the screen, on the host's sound
	 * server or ALSA; and playing to nothing by VNC alone: a system still
	 * finds the card it was installed with. */
	if (m->sound != NULL && strcmp(m->sound, "none") != 0) {
		static char card[48];
		static __thread char audiodev[48];

		snprintf(audiodev, sizeof audiodev, "%s,id=sound",
		         m->display != NULL ? "sdl" : m->local ? hull_audio_backend() : "none");
		argv[n++] = "-audiodev";
		argv[n++] = audiodev;
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
	if (m->secure) {
		static char code[64 + sizeof SECURE_CODE];
		static char vars[4400];

		snprintf(code, sizeof code, "if=pflash,format=raw,unit=0,readonly=on,file=%s", SECURE_CODE);
		snprintf(vars, sizeof vars, "if=pflash,format=raw,unit=1,file=%s",
		         option("", state_file(m, "OVMF_VARS.fd")));
		argv[n++] = "-global";
		argv[n++] = "driver=cfi.pflash01,property=secure,value=on";
		argv[n++] = "-drive";
		argv[n++] = code;
		argv[n++] = "-drive";
		argv[n++] = vars;
	}
	/* A TPM 2.0, swtpm's, its control socket the run directory's: CRB on
	 * q35, as a PC of Windows 11's era has it, TIS on the older machine. */
	if (m->tpm) {
		static char chardev[4400];

		snprintf(chardev, sizeof chardev, "socket,id=chrtpm,path=%s", run_file(m, "swtpm.sock"));
		argv[n++] = "-chardev";
		argv[n++] = chardev;
		argv[n++] = "-tpmdev";
		argv[n++] = "emulator,id=tpm0,chardev=chrtpm";
		argv[n++] = "-device";
		argv[n++] = m->machine_type != NULL && strcmp(m->machine_type, "q35") == 0 ?
		            "tpm-crb,tpmdev=tpm0" : "tpm-tis,tpmdev=tpm0";
	}
	/* Its threads named too, "CPU 0/KVM" and the rest: a machine that
	 * has stopped shows from /proc, as the user and touching nothing,
	 * whether its vCPUs spin or sleep (project.md section 5.0). */
	if (m->name != NULL) {
		char *named = option("", m->name);
		char *full = named == NULL ? NULL : malloc(strlen(named) + sizeof ",debug-threads=on");

		if (full != NULL)
			strcpy(stpcpy(full, named), ",debug-threads=on");
		free(named);
		argv[n++] = "-name";
		argv[n++] = full;
	}
	if (m->memory != NULL || m->gpu == 2) {
		argv[n++] = "-m";
		argv[n++] = m->memory != NULL ? m->memory : "128M";
	}
	/* Venus's blobs are the guest's memory mapped by the host, so it is
	 * a shared memfd, as on a vm. */
	if (m->gpu == 2) {
		static __thread char shared_memory[96];

		snprintf(shared_memory, sizeof shared_memory, "memory-backend-memfd,id=mem,size=%s,share=on",
		         m->memory != NULL ? m->memory : "128M");
		argv[n++] = "-object";
		argv[n++] = shared_memory;
		argv[n++] = "-machine";
		argv[n++] = "memory-backend=mem";
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
		at = (size_t)snprintf(nic, cap, "user,model=%s", m->nic != NULL ? m->nic :
		                      mac && strcmp(m->machine_type, "mac99") == 0 ? "sungem" : "ne2k_pci");
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
		/* A disk or stick passes the guest's TRIM to its image, and
		 * zeroes written become holes: attached without, a guest's
		 * image only grows, whatever it frees (fmake's Windows 11
		 * reached 24G and could not shrink, 2026-11-18). */
		snprintf(head, sizeof head, "%s,format=%s%s%s,file=",
		         d->type == 'f' ? "if=floppy" : d->type == 'c' ? "if=ide,media=cdrom" :
		         d->type == 'u' ? "if=none" : "if=ide,media=disk", d->path == NULL ? "raw" : d->format,
		         d->type == 'c' ? ",readonly=on" :
		         d->type != 'f' ? ",discard=unmap,detect-zeroes=unmap" : "",
		         d->type != 'c' && (d->path != NULL || d->nbd > 0) && !m->direct ? ",snapshot=on" :
		         "");
		/* A served collection: qemu's NBD client on its end of the pair,
		 * by number, in place of a file. */
		if (d->nbd > 0) {
			int  fd = fcntl(d->nbd, F_DUPFD, 3);
			char served[96];

			if (fd < 0)
				return;
			head[strlen(head) - strlen("file=")] = '\0';
			snprintf(served, sizeof served, "file.driver=nbd,file.server.type=fd,file.server.str=%d",
			         fd);
			if ((drive = malloc(strlen(head) + strlen(served) + 1u)) == NULL)
				return;
			strcpy(stpcpy(drive, head), served);
		} else if ((drive = option(head, path)) == NULL)
			return;
		{
			size_t length = strlen(drive);
			char  *indexed = malloc(length + 40u);

			if (indexed == NULL)
				return;
			/* A USB stick: usb-storage on an xHCI of its own, made
			 * removable, which is the media Windows Setup searches for
			 * its answer file. */
			if (d->type == 'u') {
				sprintf(indexed, "%s,id=stick%u", drive, sticks);
				argv[n++] = "-drive";
				argv[n++] = indexed;
				if (sticks == 0u) {
					argv[n++] = "-device";
					argv[n++] = "qemu-xhci,id=sticks";
				}
				if ((indexed = malloc(64u)) == NULL)
					return;
				snprintf(indexed, 64u, "usb-storage,bus=sticks.0,drive=stick%u,removable=on", sticks);
				sticks++;
				argv[n++] = "-device";
				argv[n++] = indexed;
				continue;
			}
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

/* In the child: the machine's emulator, a board's or a PC's. */
/* A floppy image's drive, 86Box's word for it, by the image's size. */
static const char *box_floppy_type(const char *path)
{
	struct stat st;

	if (stat(path, &st) != 0)
		return "35_2hd";
	switch (st.st_size) {
	case 163840: case 184320: case 327680: case 368640: return "525_2dd";
	case 1228800:                                       return "525_2hd";
	case 737280:                                        return "35_2dd";
	case 2949120:                                       return "35_2ed";
	default:                                            return "35_2hd";
	}
}

/* In the child: 86Box's settings for the machine, 86box.cfg in its
 * directory -- (state DIR) where given, so its CMOS settings, nvr/, are
 * kept as a PC's are, else the run's -- then 86Box, its COM1 on its own
 * stdio as qemu's is, in its window on the run's display. Returns to
 * fail. */
static void exec_86box(const hull_machine *m)
{
	const char *vm = m->state != NULL ? m->state : m->run_state;
	char        path[4400];
	char        app[4300];
	char        roms[4400];
	unsigned    floppy = 0u;
	unsigned    disk = 0u;
	unsigned    cdrom = 0u;
	FILE       *cfg;

	if (hull_86box_dir(app, sizeof app) != 0)
		return;
	snprintf(roms, sizeof roms, "%s/roms", app);
	strncat(app, "/squashfs-root/AppRun", sizeof app - strlen(app) - 1u);
	snprintf(path, sizeof path, "%s/86box.cfg", vm);
	if ((cfg = fopen(path, "w")) == NULL)
		return;
	fprintf(cfg, "[General]\nconfirm_exit = 0\nconfirm_reset = 0\nconfirm_save = 0\n\n");
	fprintf(cfg, "[Machine]\nmachine = %s\n", m->box_machine);
	if (m->box_cpu != NULL)
		fprintf(cfg, "cpu_family = %s\n", m->box_cpu);
	/* In KiB, a period's PC having 512K or 640K: hull_size_mib's MiB would
	 * round them to nothing. */
	if (m->memory != NULL) {
		char         *end;
		unsigned long kib = strtoul(m->memory, &end, 10);

		kib *= *end == 'G' ? 1024ul * 1024ul : *end == 'M' ? 1024ul : *end == 'K' ? 1ul :
		       1ul / 1024ul;
		if (kib > 0ul)
			fprintf(cfg, "mem_size = %lu\n", kib);
	}
	if (m->box_video != NULL)
		fprintf(cfg, "\n[Video]\ngfxcard = %s\n", m->box_video);
	if (m->box_sound != NULL)
		fprintf(cfg, "\n[Sound]\nsndcard = %s\n", m->box_sound);
	fprintf(cfg, "\n[Ports (COM & LPT)]\nserial1_enabled = 1\nserial1_device = stdio\n"
	        "\n[Virtual Console (COM) #1]\nmode = 0\n");
	fprintf(cfg, "\n[Floppy and CD-ROM drives]\n");
	for (unsigned i = 0u; i < m->media_count; i++) {
		const medium *d = &m->media[i];

		if (d->type == 'f' && floppy < 2u) {
			floppy++;
			fprintf(cfg, "fdd_%02u_type = %s\nfdd_%02u_fn = %s\n", floppy,
			        box_floppy_type(d->path), floppy, d->path);
		} else if (d->type == 'c' && cdrom < 1u) {
			cdrom++;
			fprintf(cfg, "cdrom_01_parameters = 1, atapi\ncdrom_01_ide_channel = 1:0\n"
			        "cdrom_01_image_path = %s\n", d->path);
		}
	}
	for (unsigned i = 0u; i < m->media_count; i++) {
		const medium *d = &m->media[i];
		struct stat   st;

		/* A disk's geometry from its size, 63 sectors and 16 heads, as
		 * an IDE disk of the period is addressed. */
		if (d->type == 'd' && disk < 2u && stat(d->path, &st) == 0) {
			if (disk == 0u)
				fprintf(cfg, "\n[Hard disks]\n");
			disk++;
			fprintf(cfg, "hdd_%02u_parameters = 63, 16, %llu, 0, ide\nhdd_%02u_fn = %s\n"
			        "hdd_%02u_ide_channel = 0:%u\n", disk,
			        (unsigned long long)st.st_size / (63ull * 16ull * 512ull), disk, d->path,
			        disk, disk - 1u);
		}
	}
	if (fclose(cfg) != 0)
		return;
	/* COM1 on stdio reads stdin at its start, and a pipe with nothing in
	 * it holds the machine there, never started (measured: 0 Hz until
	 * stdin was /dev/null). So its COM1 is output alone: what the guest
	 * writes reaches the run; what is typed goes to its window. */
	{
		int null = open("/dev/null", O_RDONLY | O_CLOEXEC);

		if (null < 0 || dup2(null, 0) < 0)
			return;
	}
	setenv("DISPLAY", m->display, 1);
	if (m->xauthority != NULL)
		setenv("XAUTHORITY", m->xauthority, 1);
	setenv("QT_QPA_PLATFORM", "xcb", 1);
	snprintf(path, sizeof path, "%s/86box.log", m->run_state);
	execl(app, app, "-P", vm, "-R", roms, "-N", "-L", path, (char *)NULL);
}

/* `hull virtualbox pc`, the outer vm and VirtualBox in it, COM1 its
 * stdout. */
static void exec_vbox(const hull_machine *m)
{
	static char self[PATH_MAX];
	static char agent[PATH_MAX + 16];
	static char work[4200];
	static char memory[32];
	static char cpus[32];
	char       *argv[16u + 2u * PC_MAX_MEDIA];
	unsigned    n = 0u;
	ssize_t     length = readlink("/proc/self/exe", self, sizeof self - 1u);
	const char *slash;
	char        boot = 'd';

	if (length <= 0)
		return;
	self[length] = '\0';
	slash = strrchr(self, '/');
	snprintf(agent, sizeof agent, "%.*s/hull-agent", (int)(slash != NULL ? slash - self : 0), self);
	{
		char dir[4096];

		if (hull_vbox_dir(dir, sizeof dir) != 0)
			return;
		snprintf(work, sizeof work, "%s-run/pc-%ld", dir, (long)getpid());
	}
	/* The spec's size in MiB, VirtualBox's unit: a number, then K, M, G
	 * or T, which the plan has checked. */
	{
		char              *end = NULL;
		unsigned long long size = m->memory != NULL ? strtoull(m->memory, &end, 10) : 1024ull;

		if (end != NULL)
			size = *end == 'G' ? size << 10 : *end == 'T' ? size << 20 :
			       *end == 'K' ? size >> 10 : *end == 'M' ? size : size >> 20;
		snprintf(memory, sizeof memory, "%llu", size);
	}
	snprintf(cpus, sizeof cpus, "%s", m->cpus != NULL ? m->cpus : "1");
	boot = m->boot == 'd' ? 'c' : m->boot == 'a' ? 'f' : 'd';
	argv[n++] = self;
	argv[n++] = "virtualbox";
	argv[n++] = "pc";
	argv[n++] = work;
	argv[n++] = agent;
	argv[n++] = memory;
	argv[n++] = cpus;
	argv[n++] = m->network ? "nat" : "none";
	argv[n++] = m->vbox_efi ? "efi" : "bios";
	argv[n++] = m->nic == NULL || strcmp(m->nic, "e1000") == 0 ? "82540EM" :
	            strcmp(m->nic, "pcnet") == 0 ? "Am79C973" : "virtio";
	argv[n++] = m->machine_type != NULL && strcmp(m->machine_type, "q35") == 0 ? "ich9" : "piix3";
	argv[n++] = m->tpm ? "yes" : "no";
	argv[n++] = boot == 'c' ? "dvd" : boot == 'f' ? "floppy" : "disk";
	/* Its screen: the outer vm's, VirtualBox's window drawn on it. In a
	 * window of the outer qemu's on the display the run was given, or for
	 * hull-gui, its monitor linked as a pc's own display.qmp would be. */
	if (m->local) {
		argv[n++] = "local";
		argv[n++] = (char *)run_file(m, "display.qmp");
	} else if (m->display != NULL) {
		argv[n++] = "window";
		argv[n++] = "-";
		setenv("DISPLAY", m->display, 1);
		if (m->xauthority != NULL)
			setenv("XAUTHORITY", m->xauthority, 1);
	} else {
		argv[n++] = "none";
		argv[n++] = "-";
	}
	argv[n++] = m->direct ? "direct" : "overlay";
	for (unsigned i = 0u; i < m->media_count; i++) {
		argv[n++] = m->media[i].type == 'c' ? "cdrom" : m->media[i].type == 'f' ? "floppy" :
		            "disk";
		argv[n++] = m->media[i].path;
	}
	argv[n] = NULL;
	execv(self, argv);
}

static void exec_qemu(const hull_machine *m)
{
	if (m->box == 2) {
		exec_vbox(m);
		return;
	}
	if (m->box) {
		exec_86box(m);
		return;
	}
	if (board_named(m->machine_type) != NULL)
		exec_board(m, board_named(m->machine_type));
	else
		exec_pc(m);
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
	/* Powered off by itself: a collection's writes go back into it, before
	 * the machine's end is said. Killed or failed, the image is dropped,
	 * as a crash would leave it untrustworthy. */
	if (code == 0 && signum == 0)
		for (unsigned i = 0u; i < m->media_count; i++)
			if (m->media[i].write_back && m->media[i].fd >= 0)
				write_back(m, &m->media[i]);
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
	/* What qemu forked and left, Venus's render server, ended while
	 * qemu is still a zombie and its session's id cannot be reused. */
	{
		siginfo_t info;

		while (waitid(P_PID, (id_t)m->pid, &info, WEXITED | WNOWAIT) < 0 && errno == EINTR)
			;
		hull_display_end_session(m->pid);
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
/* Apple's SMC key from the user's file: 64 printable characters, a line
 * end after them allowed. Apple's, so never in hull's tree or its plans;
 * read as the machine starts, into m->osk. 0, or -1 with m->why. */
static int read_osk(hull_machine *m)
{
	char    key[80];
	FILE   *in = fopen(m->smc, "re");
	size_t  got = in != NULL ? fread(key, 1u, sizeof key - 1u, in) : 0u;

	if (in != NULL)
		fclose(in);
	while (got > 0u && (key[got - 1u] == '\n' || key[got - 1u] == '\r'))
		got--;
	key[got] = '\0';
	for (size_t i = 0u; i < got; i++)
		if (key[i] < 0x20 || key[i] > 0x7e || key[i] == ',')
			got = 0u;
	if (in == NULL || got != 64u) {
		snprintf(m->why, sizeof m->why, "(smc %s): Apple's SMC key is 64 characters, read from "
		         "this file, the user's own: %s\n", m->smc,
		         in == NULL ? strerror(errno) : "it holds no such key");
		return -1;
	}
	memcpy(m->osk, key, 65u);
	return 0;
}

static int launch(hull_machine *m)
{
	int   fds[8] = { -1, -1, -1, -1, -1, -1, -1, -1 };
	int  *in = fds, *monitor = fds + 2, *out = fds + 4, *err = fds + 6;
	pid_t parent = getpid();
	pid_t child;
	const char *venus;

	/* Venus's needs on the host, as on a vm (engine/display.c). */
	if (m->gpu == 2 && (venus = hull_display_venus_refusal()) != NULL) {
		snprintf(m->why, sizeof m->why, "%s\n", venus);
		return -1;
	}
	if (m->smc != NULL && read_osk(m) != 0)
		return -1;
	if (is_mac(m->machine_type)) {
		char emulator[4200];

		if (mac_emulator(emulator, sizeof emulator) != 0) {
			snprintf(m->why, sizeof m->why, "a PowerPC Mac runs under qemu-system-ppc, Debian's "
			         "qemu-system-ppc, which is not installed: `make qemu-ppc` unpacks it into "
			         "the user's cache\n");
			return -1;
		}
	}
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
	if (has_second_uart(m)) {
		int pair[2];

		if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, pair) != 0) {
			snprintf(m->why, sizeof m->why, "a socket for the machine could not be made\n");
			close_all(fds, 8u);
			return -1;
		}
		m->serial2      = pair[0];
		m->serial2_peer = pair[1];
	}
	/* Its cards as the host has them now, each bound to vfio-pci with its
	 * whole group and one machine's at a time; and the guest's memory,
	 * which VFIO pins for their DMA, within what the user may lock --
	 * refused saying so rather than left to fail in qemu's log. */
	for (unsigned i = 0u; i < m->device_count; i++)
		if (hull_hostdev_find(&m->device[i], m->why, sizeof m->why - 1u) != NULL) {
			strcat(m->why, "\n");
			close_all(fds, 8u);
			return -1;
		}
	if (m->device_count > 0u) {
		struct rlimit      locked;
		unsigned long long wanted = ((unsigned long long)(m->memory != NULL ?
		                             hull_size_mib(m->memory) : 128u) + 64ull) << 20;

		if (hull_hostdev_claim(m, m->device, m->device_count, m->why, sizeof m->why - 1u) != NULL) {
			strcat(m->why, "\n");
			close_all(fds, 8u);
			return -1;
		}
		if (getrlimit(RLIMIT_MEMLOCK, &locked) != 0)
			locked.rlim_cur = locked.rlim_max = 0;
		if (locked.rlim_cur != RLIM_INFINITY && locked.rlim_cur < wanted) {
			if (locked.rlim_max == RLIM_INFINITY || locked.rlim_max >= wanted) {
				locked.rlim_cur = wanted;
				setrlimit(RLIMIT_MEMLOCK, &locked);
			} else {
				snprintf(m->why, sizeof m->why, "a PCI device by VFIO pins the guest's %llu "
				         "MiB, and the user may lock %llu MiB: memlock in limits.conf, or "
				         "LimitMEMLOCK for the user's manager, raises it\n",
				         (wanted >> 20) - 64ull, (unsigned long long)(locked.rlim_max >> 20));
				hull_hostdev_unclaim(m);
				close_all(fds, 8u);
				return -1;
			}
		}
	}
	m->monitor_peer = monitor[1];
	m->local_display = ((m->vnc && m->gpu == 0) || m->local) && m->run_state_made &&
	                   hull_display_fits(run_file(m, "display.qmp"));
	if (m->local && !m->local_display) {
		snprintf(m->why, sizeof m->why, "(display local) needs its monitor's socket in the "
		         "machine's run directory, whose path is too long for one\n");
		m->monitor_peer = -1;
		close_all(fds, 8u);
		return -1;
	}
	child = fork();
	if (child < 0) {
		snprintf(m->why, sizeof m->why, "qemu could not be started\n");
		m->monitor_peer = -1;
		close_all(fds, 8u);
		return -1;
	}
	/* virtualbox-pc's work is named for the process it runs in. */
	if (child > 0 && m->box == 2)
		m->vbox_child = child;
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
	if (m->serial2_peer >= 0) {
		close(m->serial2_peer);
		m->serial2_peer = -1;
	}
	for (unsigned i = 0u; i < m->segment_count; i++) {
		close(m->segment_fd[i]);
		m->segment_fd[i] = -1;
	}
	if (m->lora_fd >= 0) {
		close(m->lora_fd);
		m->lora_fd = -1;
	}
	if (m->local_display)
		hull_display_link(run_file(m, "display.qmp"), m->address, m->display_link,
		                  sizeof m->display_link);
	pthread_mutex_lock(&m->lock);
	m->pid   = child;
	m->input = in[0];
	m->monitor = monitor[0];
	m->out   = out[0];
	m->err   = err[0];
	pthread_mutex_unlock(&m->lock);
	return 0;
}

static int         wait_done(hull_machine *m, int ms);
static const char *monitor_say(hull_machine *m, const char *command, char *why, size_t cap);

/* (boot_key N): Enter pressed by QMP once a second for the first N
 * seconds, which is what a person at the screen does for a firmware or
 * loader waiting on a key -- a UEFI disc's "Press any key to boot from CD
 * or DVD", Windows' among them, which otherwise times out into the next
 * boot entry with nobody there. Ends with the machine; a paused one is not
 * pressed. A key too early is lost, not stored, so it repeats rather than
 * guessing when the prompt is up, and the plan says for how long. */
static void *press_keys(void *arg)
{
	static const char command[] =
	        "{\"execute\":\"send-key\",\"arguments\":{\"keys\":"
	        "[{\"type\":\"qcode\",\"data\":\"ret\"}]}}\n";
	hull_machine *m = arg;

	for (long left = m->boot_key; left > 0; left--) {
		char why[256];
		int  paused;

		if (wait_done(m, 1000))
			break;
		pthread_mutex_lock(&m->lock);
		paused = m->paused;
		pthread_mutex_unlock(&m->lock);
		if (paused)
			continue;
		pthread_mutex_lock(&m->monitor_lock);
		(void)monitor_say(m, command, why, sizeof why);
		pthread_mutex_unlock(&m->monitor_lock);
	}
	return NULL;
}

/* The machine's thread: its images, qemu, and what it says to its end. */
static int monitor_answer_for(hull_machine *m, char *why, size_t cap, long left, char *said,
                              size_t said_cap);

/* Whether the machine's disks hold a state `hull stop --save` kept: a
 * snapshot hull-saved of the running machine, its memory in it, in the
 * first disk, a qcow2 under (mutable direct). A PC on qemu alone. */
static int saved_state(hull_machine *m)
{
	static char table[16384];
	const char *disk = NULL;

	if (!m->direct || m->box || is_mac(m->machine_type) || board_named(m->machine_type) != NULL)
		return 0;
	for (unsigned i = 0u; i < m->media_count && disk == NULL; i++)
		if (m->media[i].type == 'd' && m->media[i].path != NULL && m->media[i].collection == NULL)
			disk = m->media[i].path;
	if (disk == NULL)
		return 0;
	{
		const char *const argv[] = { "qemu-img", "snapshot", "-U", "-l", disk, NULL };

		memset(table, 0, sizeof table);
		if (hull_hostnet_run(argv, table, sizeof table) != 0)
			return 0;
	}
	for (const char *line = table; *line != '\0';) {
		const char *end = strchr(line, '\n');
		size_t      n = end != NULL ? (size_t)(end - line) : strlen(line);
		char        row[512];
		char        id[32];
		char        tag[128];
		char        size[32];

		snprintf(row, sizeof row, "%.*s", (int)(n < sizeof row ? n : sizeof row - 1u), line);
		line += n + (end != NULL);
		if (sscanf(row, "%31s %127s %31s", id, tag, size) == 3 && isdigit((unsigned char)id[0]) &&
		    strcmp(tag, "hull-saved") == 0)
			return strcmp(size, "0") != 0;
	}
	return 0;
}

/* The saved state deleted from every disk once the machine has resumed
 * from it, by the monitor's delvm: said where it fails, for the next
 * start would resume from it again. */
static void drop_saved_state(hull_machine *m)
{
	static const char command[] = "{\"execute\":\"human-monitor-command\",\"arguments\":"
	                              "{\"command-line\":\"delvm hull-saved\"}}\n";
	char              said[512] = "";
	const char       *wrong;

	pthread_mutex_lock(&m->monitor_lock);
	wrong = monitor_say(m, "{\"execute\":\"query-status\"}\n", m->monitor_why,
	                    sizeof m->monitor_why);
	if (wrong == NULL) {
		if (send(m->monitor, command, strlen(command), MSG_NOSIGNAL) < 0 ||
		    monitor_answer_for(m, m->monitor_why, sizeof m->monitor_why, 60000L, said,
		                       sizeof said) != 1)
			wrong = m->monitor_why;
		else if (said[0] != '\0')
			wrong = said;
	}
	pthread_mutex_unlock(&m->monitor_lock);
	if (wrong != NULL) {
		char line[700];
		int  length = snprintf(line, sizeof line, "its saved state could not be dropped: %s; "
		                       "`hull snapshot delete FILE hull-saved` does it\n", wrong);

		host->on_data(m->core, NULL, 2, (const uint8_t *)line, (size_t)length);
	}
}

static void *run_machine(void *arg)
{
	hull_machine *m = arg;
	int           failed = 0;
	int           stopped;
	int           pressing;

	for (unsigned i = 0u; i < m->media_count && !failed; i++)
		if (m->media[i].collection != NULL) {
			int served = serve_collection(m, &m->media[i]);

			failed = served < 0 || (served == 0 && make_image(m, &m->media[i]) != 0);
		}
	pthread_mutex_lock(&m->lock);
	stopped = m->stopping;
	pthread_mutex_unlock(&m->lock);
	if (stopped) {
		finish(m, -1, SIGTERM);
		return NULL;
	}
	if (!failed && (m->resume = saved_state(m)) != 0) {
		static const char said[] = "resuming from its saved state, hull-saved; `hull snapshot "
		                           "delete FILE hull-saved` discards it\n";

		host->on_data(m->core, NULL, 2, (const uint8_t *)said, sizeof said - 1u);
	}
	if (failed || prepare_state(m) != 0 || launch(m) != 0) {
		host->on_data(m->core, NULL, 2, (const uint8_t *)m->why, strlen(m->why));
		release_state(m);
		finish(m, 125, 0);
		return NULL;
	}
	/* Resumed, its saved state is spent, as VirtualBox's is at a start:
	 * the next start boots, or resumes from a later save. */
	if (m->resume)
		drop_saved_state(m);
	pressing = m->boot_key > 0 && pthread_create(&m->presser, NULL, press_keys, m) == 0;
	read_machine(m);
	if (pressing)
		pthread_join(m->presser, NULL);
	release_state(m);
	return NULL;
}

static hull_status create(hull_core *core, hull_buf plan, int box, hull_machine **out,
                          hull_error *error);

static hull_status pc_create(hull_core *core, hull_buf plan, hull_machine **out, hull_error *error)
{
	return create(core, plan, 0, out, error);
}

static hull_status vbox_create(hull_core *core, hull_buf plan, hull_machine **out,
                               hull_error *error)
{
	return create(core, plan, 2, out, error);
}

static hull_status box_create(hull_core *core, hull_buf plan, hull_machine **out, hull_error *error)
{
	return create(core, plan, 1, out, error);
}

static hull_status create(hull_core *core, hull_buf plan, int box, hull_machine **out,
                          hull_error *error)
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
	m->box  = box;
	m->pid  = -1;
	m->out  = -1;
	m->err  = -1;
	m->input = -1;
	m->monitor = -1;
	m->lora_fd = -1;
	m->serial2 = -1;
	m->serial2_peer = -1;
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
	/* The radio's air: a segment of the core's switch of its own, lora-NAME,
	 * which no Ethernet segment's name can be, carrying its packets as
	 * frames of type 0x88b5 to every other radio on it. */
	if (m->lora != NULL) {
		static char detail[256];
		char        key[80];

		if (host->struct_size < offsetof(hull_host_ops, segment) + sizeof host->segment ||
		    host->segment == NULL)
			return fail(error, HULL_ERR_RUNTIME, "the core has no switch for a LoRa air");
		snprintf(key, sizeof key, "lora-%s", m->lora);
		if ((m->lora_fd = host->segment(m->core, key, detail, sizeof detail)) < 0)
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

static const char *monitor_say(hull_machine *m, const char *command, char *why, size_t cap);

/* A PC's stop presses its power button, as VirtualBox's ACPI Shutdown
 * does: qemu quits at TERM without a word to the guest, which for an XP
 * or a Linux with its disk mounted is the plug pulled. The guest is given
 * `timeout_ms` to act on it, then TERM and KILL as before. A board, a
 * Mac, 86Box's PC, a paused machine and a stop with no grace -- the
 * core's own end -- take TERM at once, having no button qemu presses.
 * 1 where the guest powered off, -1 where it was pressed and the grace
 * ran out, 0 where it was not pressed. */
static int pressed_power(hull_machine *m, int timeout_ms)
{
	static const char command[] = "{\"execute\":\"system_powerdown\"}\n";
	char              why[256];
	int               pressed;

	if (timeout_ms <= 0 || m->box || is_mac(m->machine_type) ||
	    board_named(m->machine_type) != NULL)
		return 0;
	pthread_mutex_lock(&m->lock);
	pressed = !m->done && !m->paused && m->monitor >= 0;
	pthread_mutex_unlock(&m->lock);
	if (!pressed)
		return 0;
	pthread_mutex_lock(&m->monitor_lock);
	pressed = monitor_say(m, command, why, sizeof why) == NULL;
	pthread_mutex_unlock(&m->monitor_lock);
	if (!pressed)
		return 0;
	return wait_done(m, timeout_ms) ? 1 : -1;
}

/* TERM, then KILL after `timeout_ms`, a PC's power button pressed first.
 * The reader reaps it, so the pid is signalled only while it is not done,
 * under the lock. */
static hull_status pc_stop(hull_core *core, hull_machine *m, int timeout_ms, hull_error *error)
{
	int pressed;

	(void)core;
	(void)error;
	pthread_mutex_lock(&m->lock);
	m->stopping = 1;
	pthread_mutex_unlock(&m->lock);
	if ((pressed = pressed_power(m, timeout_ms)) == 1)
		return HULL_OK;
	pthread_mutex_lock(&m->lock);
	if (!m->done && m->pid > 0) {
		if (m->paused)
			kill(m->pid, SIGCONT);
		kill(m->pid, SIGTERM);
	}
	pthread_mutex_unlock(&m->lock);
	/* The grace spent on the button, qemu's TERM is a moment's. */
	if (!wait_done(m, pressed != 0 ? 2000 : timeout_ms > 0 ? timeout_ms : 1)) {
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
	hull_display_unlink(m->display_link, run_file(m, "display.qmp"));
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
static int monitor_answer_for(hull_machine *m, char *why, size_t cap, long left, char *said,
                              size_t said_cap);

static int monitor_answer(hull_machine *m, char *why, size_t cap)
{
	return monitor_answer_for(m, why, cap, 5000, NULL, 0u);
}

/* The same, waiting `left` ms, and a return's value, where it is a string
 * -- what a human-monitor-command printed -- into `said`. */
static int monitor_answer_for(hull_machine *m, char *why, size_t cap, long left, char *said,
                              size_t said_cap)
{
	char   line[2048];
	size_t have = 0u;

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
			if (strncmp(line, "{\"return\"", 9u) == 0) {
				const char *text = strstr(line, "\"return\": \"");

				if (said != NULL && said_cap > 0u)
					snprintf(said, said_cap, "%.*s",
					         text != NULL ? (int)strcspn(text + 11, "\"\\") : 0,
					         text != NULL ? text + 11 : "");
				return 1;
			}
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
/* A command said to a running machine's QMP: HULL_OK, or why not, qemu's
 * own words kept in the machine for the core to read after. */
static hull_status ask_monitor(hull_machine *m, const char *command, hull_error *error)
{
	const char *wrong;

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
	wrong = monitor_say(m, command, m->monitor_why, sizeof m->monitor_why);
	pthread_mutex_unlock(&m->monitor_lock);
	return wrong != NULL ? fail(error, HULL_ERR_RUNTIME, wrong) : HULL_OK;
}

static hull_status change_medium(hull_machine *m, const char *drive, const char *path,
                                 hull_error *error)
{
	char        command[8192];
	char        escaped[4200];
	char        id[16];
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
	return ask_monitor(m, command, error);
}

/* `hull key ID KEY...`: each word a chord of qemu's key names joined by
 * `-`, `ctrl-alt-delete`, pressed together and let go, in order, as by a
 * person at the machine's keyboard -- what a legacy installer with no
 * serial console is driven by. `hull type` is this with its text spelled
 * as keys by the CLI. */
static hull_status press(hull_machine *m, const char *keys, hull_error *error)
{
	char        command[1024];
	const char *word = keys;

	while (*word != '\0') {
		size_t   length = strcspn(word, " ");
		size_t   at;
		unsigned count = 0u;
		hull_status said;

		if (length == 0u) {
			word++;
			continue;
		}
		at = (size_t)snprintf(command, sizeof command,
		                      "{\"execute\":\"send-key\",\"arguments\":{\"keys\":[");
		for (size_t i = 0u; i < length; ) {
			size_t name = strcspn(word + i, "- ");

			if (name == 0u || name > 32u || count == 8u)
				return fail(error, HULL_ERR_RUNTIME, "a key is a name of qemu's, `ret` or `a`, "
				            "a chord of up to eight joined by -, `ctrl-alt-delete`");
			for (size_t c = 0u; c < name; c++)
				if (!((word[i + c] >= 'a' && word[i + c] <= 'z') ||
				      (word[i + c] >= '0' && word[i + c] <= '9') || word[i + c] == '_'))
					return fail(error, HULL_ERR_RUNTIME, "a key's name is lower case letters, "
					            "digits and _, as qemu's are: `ret`, `spc`, `shift`, `f1`");
			at += (size_t)snprintf(command + at, sizeof command - at,
			                       "%s{\"type\":\"qcode\",\"data\":\"%.*s\"}",
			                       count > 0u ? "," : "", (int)name, word + i);
			count++;
			i += name + (word[i + name] == '-');
		}
		snprintf(command + at, sizeof command - at, "]}}\n");
		if ((said = ask_monitor(m, command, error)) != HULL_OK)
			return said;
		word += length;
	}
	return HULL_OK;
}

/* `hull screenshot ID FILE`: the machine's screen as qemu draws it, a PNG
 * written by qemu at `path`, absolute -- what a test compares or a person
 * reads with no VNC client open. */
static hull_status screenshot(hull_machine *m, const char *path, hull_error *error)
{
	char   command[4400];
	char   escaped[4200];
	size_t at = 0u;

	if (path == NULL || path[0] != '/')
		return fail(error, HULL_ERR_RUNTIME, "a screenshot's path is absolute by here");
	for (const char *c = path; *c != '\0'; c++) {
		if (at + 3u > sizeof escaped)
			return fail(error, HULL_ERR_RUNTIME, "the screenshot's path is too long");
		if ((unsigned char)*c < 0x20u)
			return fail(error, HULL_ERR_RUNTIME, "the screenshot's path holds a control character");
		if (*c == '"' || *c == '\\')
			escaped[at++] = '\\';
		escaped[at++] = *c;
	}
	escaped[at] = '\0';
	snprintf(command, sizeof command, "{\"execute\":\"screendump\",\"arguments\":"
	         "{\"filename\":\"%s\",\"format\":\"png\"}}\n", escaped);
	return ask_monitor(m, command, error);
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

/* `console 2`'s thread: the board's second UART to the stream, until the
 * machine ends it or the stream's close says so on `from`; the socket is
 * the machine's and stays open for the next console. */
static void *deliver_second(void *arg)
{
	hull_stream *st = arg;
	uint8_t      buffer[4096];

	for (;;) {
		struct pollfd fds[2] = { { st->machine->serial2, POLLIN, 0 }, { st->from, POLLIN, 0 } };
		ssize_t       n;

		if (poll(fds, 2u, -1) < 0) {
			if (errno == EINTR)
				continue;
			break;
		}
		if (fds[1].revents != 0)
			break;
		if (fds[0].revents == 0)
			continue;
		n = read(st->machine->serial2, buffer, sizeof buffer);
		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0)
			break;
		host->on_data(st->core, st, 1, buffer, (size_t)n);
	}
	host->on_exit(st->core, st->machine, st, 0, 0);
	return NULL;
}

/* `hull exec ID -- console 2`: a stream joined to a board's second UART,
 * one at a time, the same terminal and no more as the first's. */
static hull_status join_second(hull_core *core, hull_machine *m, hull_stream **out,
                               hull_error *error)
{
	hull_stream *st;
	int          ends[2];

	if (!has_second_uart(m))
		return fail(error, HULL_ERR_UNSUPPORTED, "only a board on qemu has a second serial "
		            "port, `console 2`: this machine has its first alone");
	if ((st = calloc(1u, sizeof *st)) == NULL)
		return fail(error, HULL_ERR_INTERNAL, "out of memory");
	if (pipe2(ends, O_CLOEXEC) != 0) {
		free(st);
		return fail(error, HULL_ERR_RUNTIME, "a pipe for the console could not be made");
	}
	st->machine = m;
	st->core    = core;
	st->from    = ends[0];
	st->to      = ends[1];
	st->socket  = -1;
	st->second  = 1;
	pthread_mutex_lock(&m->lock);
	/* Its socket is made before qemu starts and is read once the input is
	 * set, under this lock, as the first port's is. */
	if (m->done || m->console2 != NULL || m->input < 0) {
		int gone = m->done;
		int early = m->input < 0;

		pthread_mutex_unlock(&m->lock);
		close(ends[0]);
		close(ends[1]);
		free(st);
		return gone ? fail(error, HULL_ERR_GONE, "the machine is not running") :
		       fail(error, HULL_ERR_BUSY, early ? "the machine is still starting" :
		            "the second serial port has a console joined to it already");
	}
	m->console2 = st;
	pthread_mutex_unlock(&m->lock);
	if (pthread_create(&st->thread, NULL, deliver_second, st) != 0) {
		pthread_mutex_lock(&m->lock);
		m->console2 = NULL;
		pthread_mutex_unlock(&m->lock);
		close(ends[0]);
		close(ends[1]);
		free(st);
		return fail(error, HULL_ERR_INTERNAL, "the console's thread could not start");
	}
	*out = st;
	return HULL_OK;
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

/* `hull snapshot take|restore|delete ID NAME` of a running PC: its disks
 * and its memory, as VirtualBox's Take Snapshot of a running machine
 * keeps it, by the monitor's savevm, loadvm and delvm, inside its qcow2s
 * where `qemu-img snapshot` finds them too. A machine kept as it was,
 * under (mutable overlay), writes into qemu's throwaway file, so a
 * snapshot there would go with it: refused. savevm stops the guest while
 * its memory is written, so it is given minutes. */
static hull_status live_snapshot(hull_machine *m, const char *verb, const char *name,
                                 hull_error *error)
{
	const char *hmp = strcmp(verb, "take") == 0 ? "savevm" :
	                  strcmp(verb, "restore") == 0 ? "loadvm" :
	                  strcmp(verb, "delete") == 0 ? "delvm" : NULL;
	char        command[256];
	char        said[512] = "";
	const char *wrong;

	if (hmp == NULL)
		return fail(error, HULL_ERR_UNSUPPORTED, "a snapshot is taken, restored or deleted");
	for (const char *c = name; *c != '\0'; c++)
		if (!isalnum((unsigned char)*c) && strchr("._-", *c) == NULL)
			return fail(error, HULL_ERR_RUNTIME, "a snapshot's name is a plain word");
	if (name[0] == '\0' || strlen(name) > 64u)
		return fail(error, HULL_ERR_RUNTIME, "a snapshot's name is a plain word");
	if (!m->direct)
		return fail(error, HULL_ERR_UNSUPPORTED, "a machine under (mutable overlay) writes into "
		            "qemu's throwaway file, which a snapshot would go with: (mutable direct) "
		            "keeps one");
	pthread_mutex_lock(&m->lock);
	if (m->done || m->monitor < 0 || m->paused) {
		int paused = m->paused && !m->done;

		pthread_mutex_unlock(&m->lock);
		return paused ? fail(error, HULL_ERR_BUSY, "the machine is paused") :
		       fail(error, HULL_ERR_GONE, "the machine is not running");
	}
	pthread_mutex_unlock(&m->lock);
	snprintf(command, sizeof command,
	         "{\"execute\":\"human-monitor-command\",\"arguments\":{\"command-line\":\"%s %s\"}}\n",
	         hmp, name);
	pthread_mutex_lock(&m->monitor_lock);
	wrong = monitor_say(m, "{\"execute\":\"query-status\"}\n", m->monitor_why,
	                    sizeof m->monitor_why);
	if (wrong == NULL) {
		if (send(m->monitor, command, strlen(command), MSG_NOSIGNAL) < 0)
			wrong = "the machine is not running";
		else if (monitor_answer_for(m, m->monitor_why, sizeof m->monitor_why, 600000L, said,
		                            sizeof said) != 1)
			wrong = m->monitor_why;
	}
	pthread_mutex_unlock(&m->monitor_lock);
	if (wrong != NULL)
		return fail(error, HULL_ERR_RUNTIME, wrong);
	/* The human monitor's own failure is a return of its words: an
	 * "Error:" line, or a device it could not snapshot. */
	if (said[0] != '\0') {
		snprintf(m->monitor_why, sizeof m->monitor_why, "%s", said);
		return fail(error, HULL_ERR_RUNTIME, m->monitor_why);
	}
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
	/* 86Box has no monitor of qemu's to ask. */
	if (m->box && words[0] != NULL && (strcmp(words[0], "change") == 0 ||
	                                   strcmp(words[0], "snapshot") == 0 ||
	                                   strcmp(words[0], "key") == 0 ||
	                                   strcmp(words[0], "screen") == 0)) {
		for (unsigned i = 0u; i < count; i++)
			free(words[i]);
		return fail(error, HULL_ERR_UNSUPPORTED, "an 86Box machine's media, keys and screen "
		            "are its window's");
	}
	/* `hull media change`, which asks and is done. */
	if (words[0] != NULL && strcmp(words[0], "change") == 0 && words[1] != NULL) {
		hull_status changed = change_medium(m, words[1], words[2], error);

		for (unsigned i = 0u; i < count; i++)
			free(words[i]);
		return changed == HULL_OK ? finished_stream(core, m, out, error) : changed;
	}
	/* `hull snapshot VERB ID NAME`, likewise. */
	if (words[0] != NULL && strcmp(words[0], "snapshot") == 0) {
		hull_status done = words[1] == NULL || words[2] == NULL ?
		                   fail(error, HULL_ERR_RUNTIME, "a snapshot is a verb and a name") :
		                   live_snapshot(m, words[1], words[2], error);

		for (unsigned i = 0u; i < count; i++)
			free(words[i]);
		return done == HULL_OK ? finished_stream(core, m, out, error) : done;
	}
	/* `hull key`, `hull type` and `hull screenshot`, likewise. */
	if (words[0] != NULL && (strcmp(words[0], "key") == 0 || strcmp(words[0], "screen") == 0)) {
		hull_status done = words[1] == NULL ?
		                   fail(error, HULL_ERR_RUNTIME, "nothing was given to press or write") :
		                   words[0][0] == 'k' ? press(m, words[1], error) :
		                   screenshot(m, words[1], error);

		for (unsigned i = 0u; i < count; i++)
			free(words[i]);
		return done == HULL_OK ? finished_stream(core, m, out, error) : done;
	}
	asked = words[0] != NULL && strcmp(words[0], "console") == 0;
	/* `console 2` is the second serial port, `console 1` the first's
	 * name in full; counted as the ports are, whatever the board calls
	 * them -- an MPS2's UART1, an STM32's USART2, a PC's COM2. */
	if (asked && words[1] != NULL && strcmp(words[1], "1") != 0) {
		int second = strcmp(words[1], "2") == 0 && words[2] == NULL;

		for (unsigned i = 0u; i < count; i++)
			free(words[i]);
		if (!second)
			return fail(error, HULL_ERR_UNSUPPORTED, "`console` takes the serial port's "
			            "number, 1 or 2, and nothing else");
		return join_second(core, m, out, error);
	}
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

/* After input for a qemu board's UART, its main loop woken by a word to
 * QMP. qemu stops watching the serial port's descriptor while the UART
 * cannot take a byte -- before the firmware has turned its receiver on --
 * and looks again only when something wakes the loop; the AVR's USART
 * does not, on its receiver turned on. So a firmware that polls, rather
 * than reading the data register once at its start, heard nothing typed
 * on qemu 10.0 with -nodefaults, which takes away the user network whose
 * timers woke the loop otherwise. Measured by test/board/echo_avr.c:
 * nothing received, and every byte once the monitor was spoken to. */
static void wake_board(hull_machine *m)
{
	static const char command[] = "{\"execute\":\"query-status\"}\n";
	const board      *b = board_named(m->machine_type);
	char              why[256];
	int               ready;

	if (b == NULL || strncmp(b->emulator, "qemu-", 5u) != 0)
		return;
	pthread_mutex_lock(&m->lock);
	ready = !m->done && m->monitor >= 0;
	pthread_mutex_unlock(&m->lock);
	if (!ready)
		return;
	pthread_mutex_lock(&m->monitor_lock);
	(void)monitor_say(m, command, why, sizeof why);
	pthread_mutex_unlock(&m->monitor_lock);
}

/* Input for COM1: it goes to qemu's stdin, which the port reads. */
static hull_status pc_stream_write(hull_core *core, hull_stream *st, const uint8_t *data,
                                   size_t length, hull_error *error)
{
	int to = st->socket >= 0 ? st->socket : st->second ? st->machine->serial2 :
	         st->machine->input;

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
	if (st->socket < 0)
		wake_board(st->machine);
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
	if (st->second) {
		close(st->to);
		pthread_join(st->thread, NULL);
		pthread_mutex_lock(&m->lock);
		m->console2 = NULL;
		pthread_mutex_unlock(&m->lock);
		close(st->from);
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

#define BOARD_NOTE "a microcontroller board, booted from its firmware, the image: source; " \
                   "its first UART the machine's serial port, its console"
#define ESP_NOTE "an Espressif chip on Espressif's qemu (hull esp fetch), booted from its " \
                 "firmware, the image: source, a whole flash image; its UART0 the machine's " \
                 "serial port, its console; no radio"

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
	  "the collection reassembled, its hash held, into an unnamed file its writes go with, or "
	  "under (mutable direct) taken apart into the collection again at power-off; a "
	  "CD-ROM's tree alone made an ISO; a raw dump's audio tracks and quirks not given, qemu "
	  "reading a disc by its 2048-byte sectors", NULL },
	{ "media.size",         HULL_SUP_EMULATED,
	  "a disk collection served that large by NBD, zeros after its files; the guest grows its "
	  "own partition into the room, and under (mutable direct) the collection takes the "
	  "larger disk back", NULL },
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
	{ "devices.pci",        HULL_SUP_NATIVE,
	  "vfio-pci: a card of the host's itself, a GPU among them, bound to vfio-pci with its whole "
	  "group; the guest's memory pinned for its DMA", NULL },
	{ "display.local",      HULL_SUP_NATIVE,
	  "the screen, keyboard and mouse in hull-gui by qemu's D-Bus display, this host only, "
	  "its frames shared", NULL },
	{ "memory",             HULL_SUP_NATIVE,   NULL, NULL },
	{ "stop_grace",         HULL_SUP_NATIVE,
	  "a PC's power button pressed, its guest given the grace to act on it, then TERM; a "
	  "board's or a Mac's TERM, then KILL after it", NULL },
	{ "cpus",               HULL_SUP_NATIVE,   NULL, NULL },
	{ "pc.pointer.ps2",    HULL_SUP_NATIVE, "a PS/2 mouse, moved by deltas: hull-gui's screen "
	  "captures it, Right Ctrl releasing", NULL },
	{ "pc.pointer.tablet", HULL_SUP_NATIVE, "a USB tablet on a UHCI of its own, the pointer where "
	  "the viewer's is, nothing captured", NULL },
	{ "pc.sound.none",  HULL_SUP_NATIVE, NULL, NULL },
	{ "pc.sound.sb16",  HULL_SUP_NATIVE, "a Sound Blaster 16", NULL },
	{ "pc.sound.adlib", HULL_SUP_NATIVE, "an AdLib's OPL2", NULL },
	{ "pc.sound.gus",   HULL_SUP_NATIVE, "a Gravis Ultrasound", NULL },
	{ "pc.sound.es1370", HULL_SUP_NATIVE, "an AudioPCI", NULL },
	{ "pc.sound.ac97",  HULL_SUP_NATIVE, "AC'97", NULL },
	{ "pc.sound.hda",   HULL_SUP_NATIVE, "Intel HD Audio", NULL },
	{ "pc.firmware.bios", HULL_SUP_NATIVE, "qemu's SeaBIOS", NULL },
	{ "pc.firmware.uefi", HULL_SUP_NATIVE, "OVMF, with no variable store", NULL },
	{ "pc.firmware.uefi-secure", HULL_SUP_NATIVE,
	  "OVMF's secure-boot build under SMM, its variables enrolled with Microsoft's keys, the "
	  "machine's own copy", NULL },
	{ "pc.tpm", HULL_SUP_EMULATED,
	  "a TPM 2.0 in software, swtpm's, CRB on q35 and TIS on pc", NULL },
	{ "pc.lora.path", HULL_SUP_EMULATED,
	  "an SX1276 LoRa radio on a Heltec WiFi LoRa 32 or a TTGO LoRa32, on the named air with "
	  "every other radio on it: no range, loss or collisions, its time on air kept, DIO0 read or "
	  "interrupting; on Espressif's qemu built with hull's patch (make esp-qemu-lora)", NULL },
	{ "pc.boot_key", HULL_SUP_NATIVE,
	  "Enter pressed by qemu's monitor once a second for the seconds given, from the start", NULL },
	{ "pc.state.path", HULL_SUP_NATIVE,
	  "its firmware's variables and its TPM's state, kept there across runs under (mutable "
	  "direct) as its disk is, and copied for the run under an overlay", NULL },
	{ "pc.machine.pc", HULL_SUP_NATIVE, NULL, NULL },
	{ "pc.machine.q35", HULL_SUP_NATIVE, NULL, NULL },
	{ "pc.machine.isapc", HULL_SUP_NATIVE, NULL, NULL },
	{ "pc.machine.mac99", HULL_SUP_EMULATED,
	  "a PowerPC G4 Mac, as Mac OS X 10.x ran on, OpenBIOS its firmware, its keyboard and mouse "
	  "on ADB, a sungem card unless (nic ...) says otherwise: qemu-system-ppc under TCG, `make "
	  "qemu-ppc`'s where Debian's is not installed", NULL },
	{ "pc.machine.g3beige", HULL_SUP_EMULATED,
	  "a PowerPC G3 Mac, Mac OS 9's and early OS X's, OpenBIOS its firmware: qemu-system-ppc under "
	  "TCG", NULL },
	{ "pc.machine.arduino-uno", HULL_SUP_NATIVE, BOARD_NOTE, NULL },
	{ "pc.machine.arduino-duemilanove", HULL_SUP_NATIVE, BOARD_NOTE, NULL },
	{ "pc.machine.arduino-mega", HULL_SUP_NATIVE, BOARD_NOTE, NULL },
	{ "pc.machine.arduino-mega-2560-v3", HULL_SUP_NATIVE, BOARD_NOTE, NULL },
	{ "pc.machine.stm32vldiscovery", HULL_SUP_NATIVE, BOARD_NOTE, NULL },
	{ "pc.machine.netduino2", HULL_SUP_NATIVE, BOARD_NOTE, NULL },
	{ "pc.machine.netduinoplus2", HULL_SUP_NATIVE, BOARD_NOTE, NULL },
	{ "pc.machine.olimex-stm32-h405", HULL_SUP_NATIVE, BOARD_NOTE, NULL },
	{ "pc.machine.lm3s6965evb", HULL_SUP_NATIVE, BOARD_NOTE, NULL },
	{ "pc.machine.lm3s811evb", HULL_SUP_NATIVE, BOARD_NOTE, NULL },
	{ "pc.machine.microbit", HULL_SUP_NATIVE, BOARD_NOTE, NULL },
	{ "pc.machine.at89c51ed2", HULL_SUP_NATIVE,
	  "an 8051 by SDCC's simulator, s51, its 8052 core at 11.0592 MHz, booted from its firmware, "
	  "the image: source, an Intel hex; its UART the machine's serial port, its console", NULL },
	{ "pc.machine.mps2-an385", HULL_SUP_NATIVE, BOARD_NOTE, NULL },
	{ "pc.machine.b-l475e-iot01a", HULL_SUP_NATIVE, BOARD_NOTE, NULL },
	{ "pc.machine.esp32", HULL_SUP_NATIVE, ESP_NOTE, NULL },
	{ "pc.machine.esp32-devkitc", HULL_SUP_NATIVE, ESP_NOTE, NULL },
	{ "pc.machine.esp32-cam", HULL_SUP_NATIVE, ESP_NOTE, NULL },
	{ "pc.machine.heltec-wifi-lora-32", HULL_SUP_NATIVE, ESP_NOTE, NULL },
	{ "pc.machine.heltec-wifi-lora-32-v2", HULL_SUP_NATIVE, ESP_NOTE, NULL },
	{ "pc.machine.ttgo-lora32", HULL_SUP_NATIVE, ESP_NOTE, NULL },
	{ "pc.machine.m5stack-core", HULL_SUP_NATIVE, ESP_NOTE, NULL },
	{ "pc.machine.esp32s3", HULL_SUP_NATIVE, ESP_NOTE, NULL },
	{ "pc.machine.esp32-s3-devkitc-1", HULL_SUP_NATIVE, ESP_NOTE, NULL },
	{ "pc.machine.heltec-wifi-lora-32-v3", HULL_SUP_NATIVE, ESP_NOTE, NULL },
	{ "pc.machine.xiao-esp32s3", HULL_SUP_NATIVE, ESP_NOTE, NULL },
	{ "pc.machine.lilygo-t-display-s3", HULL_SUP_NATIVE, ESP_NOTE, NULL },
	{ "pc.machine.lilygo-t3-s3", HULL_SUP_NATIVE,
	  "an ESP32-S3 with an SX1276 on SPI2 (NSS 7, NRESET 8, DIO0 9), its radio by (lora NAME)",
	  NULL },
	{ "pc.machine.esp32c3", HULL_SUP_NATIVE, ESP_NOTE, NULL },
	{ "pc.machine.esp32-c3-devkitm-1", HULL_SUP_NATIVE, ESP_NOTE, NULL },
	{ "pc.machine.xiao-esp32c3", HULL_SUP_NATIVE, ESP_NOTE, NULL },
	{ "pc.machine.esp32c5", HULL_SUP_NATIVE, ESP_NOTE, NULL },
	{ "pc.machine.esp32c6", HULL_SUP_NATIVE, ESP_NOTE, NULL },
	{ "pc.machine.esp32-c6-devkitc-1", HULL_SUP_NATIVE, ESP_NOTE, NULL },
	{ "pc.machine.xiao-esp32c6", HULL_SUP_NATIVE, ESP_NOTE, NULL },
	{ "pc.machine.esp32c61", HULL_SUP_NATIVE, ESP_NOTE, NULL },
	{ "pc.machine.esp32h2", HULL_SUP_NATIVE, ESP_NOTE, NULL },
	{ "pc.machine.esp32-h2-devkitm-1", HULL_SUP_NATIVE, ESP_NOTE, NULL },
	{ "pc.machine.esp32p4", HULL_SUP_NATIVE, ESP_NOTE, NULL },
	{ "pc.machine.esp32-p4-function-ev-board", HULL_SUP_NATIVE, ESP_NOTE, NULL },
	{ "pc.vga.std", HULL_SUP_NATIVE, NULL, NULL },
	{ "pc.vga.cirrus", HULL_SUP_NATIVE, NULL, NULL },
	{ "pc.vga.vmware", HULL_SUP_NATIVE, NULL, NULL },
	{ "pc.vga.virtio", HULL_SUP_NATIVE, NULL, NULL },
	{ "pc.vga.none", HULL_SUP_NATIVE, NULL, NULL },
	{ "pc.gpu.2d", HULL_SUP_NATIVE, NULL, NULL },
	{ "pc.gpu.virgl", HULL_SUP_NATIVE,
	  "virtio-vga-gl, OpenGL forwarded by virglrenderer to the host's GPU, shared; the guest "
	  "needs Mesa's virgl driver, which Windows has none of", NULL },
	{ "pc.gpu.venus", HULL_SUP_NATIVE,
	  "virtio-vga-gl with Venus, Vulkan forwarded to the host's GPU and driver; the guest needs "
	  "Mesa's Venus driver, and its memory is a shared memfd; the host /dev/udmabuf the user's "
	  "and virglrenderer's render server", NULL },
	{ "pc.accel.auto", HULL_SUP_NATIVE, NULL, NULL },
	{ "pc.accel.kvm", HULL_SUP_NATIVE, NULL, NULL },
	{ "pc.accel.tcg", HULL_SUP_NATIVE, NULL, NULL },
	{ "pc.nic.ne2k_pci", HULL_SUP_NATIVE, NULL, NULL },
	{ "pc.nic.ne2k_isa", HULL_SUP_NATIVE, NULL, NULL },
	{ "pc.nic.pcnet", HULL_SUP_NATIVE, NULL, NULL },
	{ "pc.nic.rtl8139", HULL_SUP_NATIVE, NULL, NULL },
	{ "pc.nic.e1000", HULL_SUP_NATIVE, NULL, NULL },
	{ "pc.nic.e1000e", HULL_SUP_NATIVE, NULL, NULL },
	{ "pc.nic.vmxnet3", HULL_SUP_NATIVE,
	  "VMware's paravirtual card, which macOS drives from 10.11 on", NULL },
	{ "pc.cpu.host", HULL_SUP_NATIVE, NULL, NULL },
	{ "pc.cpu.max", HULL_SUP_NATIVE, NULL, NULL },
	{ "pc.cpu.qemu64", HULL_SUP_NATIVE, NULL, NULL },
	{ "pc.cpu.486", HULL_SUP_NATIVE, NULL, NULL },
	{ "pc.cpu.pentium", HULL_SUP_NATIVE, NULL, NULL },
	{ "pc.cpu.pentium3", HULL_SUP_NATIVE, NULL, NULL },
	{ "pc.cpu.core2duo", HULL_SUP_NATIVE, NULL, NULL },
	{ "pc.cpu.skylake", HULL_SUP_NATIVE, NULL, NULL },
	{ "pc.cpu.penryn", HULL_SUP_NATIVE,
	  "Intel's Penryn as macOS is booted on it, GenuineIntel, its TSC invariant under KVM", NULL },
	{ "pc.smc.path", HULL_SUP_NATIVE,
	  "Apple's SMC, qemu's isa-applesmc, its key read from the file as the machine starts, "
	  "with a USB keyboard and tablet on an xHCI, macOS having no PS/2 or UHCI driver", NULL },
	{ "pc.nic.virtio", HULL_SUP_NATIVE, NULL, NULL },
	{ "pc.nic.wifi", HULL_SUP_NATIVE,
	  "an ESP32's WiFi, ESP-IDF's own driver against the chip's MAC, joined to an open access "
	  "point in qemu, PICSimLabWifi, bridged to the board's network; hull's patched qemu",
	  NULL },
	{ "security.no_new_privs", HULL_SUP_NATIVE, "set on the qemu process", NULL },
	{ "on_disconnect.stop", HULL_SUP_NATIVE,   NULL, NULL },
	{ "on_disconnect.keep", HULL_SUP_REJECTED,
	  "not yet: qemu is a child of the core, and dies with it", NULL },
	{ "hostname",           HULL_SUP_EMULATED, "the machine's name to hull, and its window's where "
	  "it has one; the guest's own name, a board's or a PC's, is its firmware's or system's", NULL },

	{ NULL,                 HULL_SUP_NATIVE,   NULL, NULL },
};

static const char *const classes[] = { "pc", NULL };
static const char *const media[]   = { "cdrom", "floppy", "disk", "usb", NULL };

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

/* The 86box engine: the same machine, run by 86Box (section 5.0). What
 * it declares is what it gives; the rest of a pc's is rejected at plan. */
static const hull_cap_entry box_entries[] = {
	{ "source.media.direct",  HULL_SUP_NATIVE,
	  "86Box's BIOS boots the drive its CMOS names; the media's writes land in their image: "
	  "files", NULL },
	{ "source.image.direct",  HULL_SUP_NATIVE,
	  "the image is the first IDE disk, its writes landing in it", NULL },
	{ "projection.bind",    HULL_SUP_NATIVE,   "nothing projected: the media are the machine's",
	  NULL },
	{ "writeback.none",     HULL_SUP_NATIVE,   NULL, NULL },
	{ "media",              HULL_SUP_NATIVE,   NULL, NULL },
	{ "media.image",        HULL_SUP_NATIVE,   "a floppy, disk or CD image by its path, or a USB "
	  "stick's, (type usb), removable usb-storage on an xHCI of its own", NULL },
	{ "network.none",       HULL_SUP_NATIVE,   NULL, NULL },
	{ "display.window",     HULL_SUP_NATIVE,
	  "86Box's own window on the X display the run was given", NULL },
	{ "memory",             HULL_SUP_NATIVE,   "the board's RAM, which 86Box rounds to it", NULL },
	{ "stop_grace",         HULL_SUP_NATIVE,   "86Box's TERM, then KILL after it", NULL },
	{ "86box.machine.path", HULL_SUP_NATIVE,
	  "an 86Box machine by its name, its ROM from the fetched set", NULL },
	{ "86box.cpu.path",     HULL_SUP_NATIVE,   "a CPU family of 86Box's, the board's first if none",
	  NULL },
	{ "86box.video.path",   HULL_SUP_NATIVE,   "a video card of 86Box's", NULL },
	{ "86box.sound.path",   HULL_SUP_NATIVE,   "a sound card of 86Box's", NULL },
	{ "pc.state.path",      HULL_SUP_NATIVE,
	  "86Box's directory for the machine: its settings and its CMOS, kept", NULL },
	{ "security.no_new_privs", HULL_SUP_NATIVE, NULL, NULL },
	{ "on_disconnect.stop", HULL_SUP_NATIVE,   NULL, NULL },
	{ "hostname",           HULL_SUP_NATIVE,   "the machine's name, its OS's own", NULL },
	{ NULL, 0, NULL, NULL },
};

static const hull_capabilities box_caps = {
	.struct_size    = sizeof (hull_capabilities),
	.engine_name    = "86box",
	.engine_version = HULL_86BOX_VERSION,
	.classes        = classes,
	.entries        = box_entries,
	.media_types    = media,
	.migration      = HULL_MIG_PINNED,
	.arches         = NULL,
	.requires_root  = 0,
};

static const hull_engine_ops box_ops = {
	.struct_size = sizeof (hull_engine_ops),
	.create      = box_create,
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

static const hull_engine box_engine = {
	.struct_size = sizeof (hull_engine),
	.abi_major   = HULL_ABI_MAJOR,
	.abi_minor   = HULL_ABI_MINOR,
	.caps        = &box_caps,
	.ops         = &box_ops,
};

/* The virtualbox-pc engine: a PC of its own media under VirtualBox, in a
 * qemu vm of its own as the virtualbox engine's machines are (section
 * 5.0), for what VirtualBox gives that qemu does not: a Windows guest's
 * Direct3D through its Guest Additions' driver. `(backend virtualbox)`
 * names it for a machine of media (plan/registry.c). Headless, its COM1
 * the run's console. */
static const hull_cap_entry vbox_pc_entries[] = {
	{ "source.media.overlay", HULL_SUP_NATIVE,
	  "VirtualBox's immutable disks: the writes in a differencing image the next start drops",
	  NULL },
	{ "source.media.direct",  HULL_SUP_NATIVE,
	  "VirtualBox's BIOS or EFI boots the drive named first; the disks' writes land in their "
	  "image: files", NULL },
	{ "projection.bind",    HULL_SUP_NATIVE,   "nothing projected: the media are the machine's",
	  NULL },
	{ "writeback.none",     HULL_SUP_NATIVE,   NULL, NULL },
	{ "media",              HULL_SUP_NATIVE,
	  "disks and discs on IDE, or SATA on a q35, and a floppy drive", NULL },
	{ "media.image",        HULL_SUP_NATIVE,
	  "VDI, VMDK or VHD as it is, a raw disk by a VMDK descriptor naming it; not a qcow2",
	  NULL },
	{ "network.none",       HULL_SUP_NATIVE,   NULL, NULL },
	{ "network.nat",        HULL_SUP_NATIVE,
	  "VirtualBox's NAT, inside the outer vm's own", NULL },
	{ "display.local",      HULL_SUP_EMULATED,
	  "VirtualBox's window on X in the outer vm, the outer vm's screen shown in hull-gui",
	  NULL },
	{ "display.window",     HULL_SUP_EMULATED,
	  "VirtualBox's window on X in the outer vm, the outer vm's screen in a window of its "
	  "qemu's", NULL },
	{ "memory",             HULL_SUP_NATIVE,   NULL, NULL },
	{ "cpus",               HULL_SUP_NATIVE,   NULL, NULL },
	{ "stop_grace",         HULL_SUP_NATIVE,
	  "VirtualBox's ACPI power button, its guest given the grace, then the outer vm ended",
	  NULL },
	{ "pc.firmware.bios",   HULL_SUP_NATIVE,   NULL, NULL },
	{ "pc.firmware.uefi",   HULL_SUP_NATIVE,   "VirtualBox's EFI", NULL },
	{ "pc.tpm",             HULL_SUP_NATIVE,   "VirtualBox's TPM 2.0", NULL },
	{ "pc.machine.pc",      HULL_SUP_NATIVE,   "VirtualBox's PIIX3, its media on IDE", NULL },
	{ "pc.machine.q35",     HULL_SUP_NATIVE,   "VirtualBox's ICH9, its media on SATA", NULL },
	{ "pc.nic.e1000",       HULL_SUP_NATIVE,   "VirtualBox's Intel PRO/1000 MT, 82540EM", NULL },
	{ "pc.nic.pcnet",       HULL_SUP_NATIVE,   "VirtualBox's PCnet-FAST III", NULL },
	{ "pc.nic.virtio",      HULL_SUP_NATIVE,   "VirtualBox's virtio-net", NULL },
	{ "security.no_new_privs", HULL_SUP_NATIVE, NULL, NULL },
	{ "on_disconnect.stop", HULL_SUP_NATIVE,   NULL, NULL },
	{ "hostname",           HULL_SUP_NATIVE,   "the machine's name, its OS's own", NULL },
	{ NULL, 0, NULL, NULL },
};

static const hull_capabilities vbox_pc_caps = {
	.struct_size    = sizeof (hull_capabilities),
	.engine_name    = "virtualbox-pc",
	.engine_version = HULL_VBOX_VERSION,
	.classes        = classes,
	.entries        = vbox_pc_entries,
	.media_types    = media,
	.migration      = HULL_MIG_PINNED,
	.arches         = NULL,
	.requires_root  = 0,
};

static const hull_engine_ops vbox_pc_ops = {
	.struct_size = sizeof (hull_engine_ops),
	.create      = vbox_create,
	.start       = pc_start,
	.stop        = pc_stop,
	.destroy     = pc_destroy,
	.signal      = pc_signal,
	.exec         = pc_exec,
	.stream_write = pc_stream_write,
	.stream_eof   = pc_stream_eof,
	.stream_close = pc_stream_close,
	.stats       = pc_stats,
};

static const hull_engine vbox_pc_engine = {
	.struct_size = sizeof (hull_engine),
	.abi_major   = HULL_ABI_MAJOR,
	.abi_minor   = HULL_ABI_MINOR,
	.caps        = &vbox_pc_caps,
	.ops         = &vbox_pc_ops,
};

const hull_engine *hull_engine_virtualbox_pc(const hull_host_ops *host_ops)
{
	if (host_ops != NULL)
		host = host_ops;
	return &vbox_pc_engine;
}

const hull_engine *hull_engine_86box(const hull_host_ops *host_ops)
{
	if (host_ops != NULL)
		host = host_ops;
	return &box_engine;
}

const hull_engine *hull_engine_qemu_pc(const hull_host_ops *host_ops)
{
	if (host_ops != NULL)
		host = host_ops;
	return &engine;
}
