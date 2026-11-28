/*
 * vbox_host.c -- a virtualbox machine's outer vm (project.md section 12).
 *
 * VirtualBox needs its kernel module and refuses to run beside KVM, so it
 * runs in a qemu vm, whose kernel is the host's and takes the host's
 * vboxdrv.ko as it is. This process is the engine's handle on all of it:
 * the core starts it, its stdin and stdout are the agent's wire, and with
 * it goes everything below, each child dying with its parent.
 *
 * The outer vm is an ordinary hull machine, on a core of its own, in a
 * runtime directory under `work`: the engine's core is busy starting this
 * machine and could not serve it. The relay is a `hull exec` in it, so the
 * wire crosses nothing but the core's own socket and the vm's channel:
 * no port on the host's network for another user to reach first.
 */

#define _GNU_SOURCE

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <sys/socket.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <ftw.h>
#include <limits.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "sshd.h"
#include "vbox.h"
#include "vbox_host.h"

/* Seconds for the outer vm to reach its script. */
#define OUTER_LIMIT 120
/* What the outer vm holds beyond the guest's memory: VirtualBox's own
 * files in its overlay, about 230 MiB, and VirtualBox itself. */
#define OUTER_MEMORY 1536ul

/* Run in the outer vm as root, its root the host's tree under an overlay.
 * VirtualBox's hardening holds every directory above its files to root,
 * which a share of the host's tree shows as nobody, and its files to
 * root's with their setuid bits, which only its package has: the host's
 * are --x to others and cannot be read. Its arguments: the guest's
 * memory, its processors, and its network. */
static const char script[] =
    "#!/bin/sh\n"
    "memory=$1 cpus=$2 network=$3\n"
    "release=$(uname -r)\n"
    "insmod /lib/modules/$release/misc/vboxdrv.ko || {\n"
    "	echo \"no vboxdrv.ko for $release: the host's VirtualBox $4 builds it\"; exit 1; }\n"
    "cp -a /vbox/usr/lib/virtualbox /usr/lib/virtualbox.hull || exit 1\n"
    "rm -rf /usr/lib/virtualbox\n"
    "mv /usr/lib/virtualbox.hull /usr/lib/virtualbox || exit 1\n"
    "chown -R 0:0 /usr/lib/virtualbox\n"
    "chmod -R go-w /usr/lib/virtualbox\n"
    "for f in VBoxHeadless VBoxNetDHCP VBoxNetNAT VBoxNetAdpCtl; do\n"
    "	chmod 4511 /usr/lib/virtualbox/$f\n"
    "done\n"
    "chown 0:0 / /usr /usr/lib\n"
    "chmod 755 / /usr /usr/lib\n"
    "export HOME=/root VBOX_USER_HOME=/share/vbox\n"
    "mkdir -p /root $VBOX_USER_HOME\n"
    "manage=/usr/lib/virtualbox/VBoxManage\n"
    "$manage convertfromraw /share/inner.img /share/inner.vdi --format VDI >/dev/null || exit 1\n"
    "rm -f /share/inner.img\n"
    "$manage createvm --name hull --register --basefolder /share/vbox >/dev/null || exit 1\n"
    "nic='--nic1 none'\n"
    "[ \"$network\" = nat ] && nic='--nic1 nat --nic-type1 virtio'\n"
    "$manage modifyvm hull --memory $memory --cpus $cpus --ioapic on --firmware efi $nic \\\n"
    "	--audio-driver none --usb-ohci off --graphicscontroller vboxvga \\\n"
    "	--uart1 0x3f8 4 --uart-mode1 server /tmp/uart.sock \\\n"
    "	--uart2 0x2f8 3 --uart-mode2 file /share/console.log || exit 1\n"
    /* One port: thirty empty ones cost about 0.4 s each to probe, nested. */
    "ports=1\n"
    "[ -f /share/sshd.img ] && ports=2\n"
    "$manage storagectl hull --name sata --add sata --portcount $ports || exit 1\n"
    "$manage storageattach hull --storagectl sata --port 0 --device 0 --type hdd \\\n"
    "	--medium /share/inner.vdi || exit 1\n"
    "if [ -f /share/sshd.img ]; then\n"
    "	$manage convertfromraw /share/sshd.img /share/sshd.vdi --format VDI >/dev/null || exit 1\n"
    "	$manage storageattach hull --storagectl sata --port 1 --device 0 --type hdd \\\n"
    "		--medium /share/sshd.vdi || exit 1\n"
    "fi\n"
    ". /share/folders.sh\n"
    "touch /share/up\n"
    "exec /usr/lib/virtualbox/VBoxHeadless --startvm hull\n";

static void say(const char *what)
{
	fprintf(stderr, "hull: virtualbox: %s\n", what);
}

/* A child with stdin and stdout as given (-1, /dev/null and stderr), its
 * end tied to this process's. */
static pid_t start(const char *const *argv, int in, int out)
{
	pid_t pid = fork();

	if (pid != 0)
		return pid;
	prctl(PR_SET_PDEATHSIG, SIGKILL);
	if (in < 0)
		in = open("/dev/null", O_RDONLY);
	dup2(in, 0);
	dup2(out < 0 ? 2 : out, 1);
	execv(argv[0], (char *const *)argv);
	_exit(127);
}

static int finish(pid_t pid)
{
	int status = 1;

	while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
		;
	return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
}

/* `source` copied to `target`, mode 0755. 0, or -1. */
static int copy_file(const char *source, const char *target)
{
	char   buffer[65536];
	int    in = open(source, O_RDONLY | O_CLOEXEC);
	int    out = open(target, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0755);
	int    status = in >= 0 && out >= 0 ? 0 : -1;
	ssize_t got;

	while (status == 0 && (got = read(in, buffer, sizeof buffer)) > 0)
		if (write(out, buffer, (size_t)got) != got)
			status = -1;
	if (in >= 0)
		close(in);
	if (out >= 0 && close(out) != 0)
		status = -1;
	return status;
}

int hull_vbox_host(const char *work, const char *root, const char *agent, const char *memory,
                   const char *cpus, const char *network, const char *ssh,
                   char *const *volumes, unsigned volume_count)
{
	char        self[PATH_MAX];
	char        vbox[4096];
	char        sshd[4096] = "";
	char        share[4300];
	char        runtime[4300];
	char        path[4400];
	char        target[4400];
	const char *parent_end = strrchr(work, '/');
	ssize_t     n = readlink("/proc/self/exe", self, sizeof self - 1u);
	pid_t       outer;
	pid_t       link;
	time_t      deadline;
	FILE       *file;
	FILE       *folders;
	int         status;

	signal(SIGPIPE, SIG_IGN);
	prctl(PR_SET_PDEATHSIG, SIGKILL);
	if (n <= 0 || parent_end == NULL || hull_vbox_dir(vbox, sizeof vbox) != 0) {
		say("this process's own paths cannot be found");
		return 1;
	}
	self[n] = '\0';
	if (hull_sshd_dir(sshd, sizeof sshd) != 0)
		sshd[0] = '\0';
	snprintf(path, sizeof path, "%s/%s", vbox, HULL_VBOX_HEADLESS);
	if (access(path, X_OK) != 0) {
		say("no VirtualBox has been fetched: `hull virtualbox fetch` downloads it, once");
		return 1;
	}
	snprintf(path, sizeof path, "%.*s", (int)(parent_end - work), work);
	mkdir(path, 0700);
	snprintf(share, sizeof share, "%s/share", work);
	snprintf(runtime, sizeof runtime, "%s/run", work);
	if (mkdir(work, 0700) != 0 || mkdir(share, 0700) != 0 || mkdir(runtime, 0700) != 0) {
		say("its work directory cannot be made");
		return 1;
	}
	setenv("XDG_RUNTIME_DIR", runtime, 1);
	setenv("HULL_CORE_IDLE", "1", 1);

	/* The machine's disk. */
	snprintf(target, sizeof target, "%s/inner.img", share);
	{
		const char *const argv[] = { self, "export", "--bootable", root, target,
		                             "--host", "ttyS0", NULL };

		if (finish(start(argv, -1, -1)) != 0)
			return 1;
	}

	/* The fetched sshd, where the machine may want it: a disk of its own,
	 * nothing being shared into the guest. An image with its own sshd and
	 * a cache with none go on, and the agent says if that will not do. */
	snprintf(path, sizeof path, "%s/%s", sshd, HULL_SSHD_DROPBEAR);
	if (strcmp(ssh, "ssh") == 0 && access(path, R_OK) == 0) {
		const char *mke2fs = access("/usr/sbin/mke2fs", X_OK) == 0 ? "/usr/sbin/mke2fs" :
		                     "/sbin/mke2fs";

		snprintf(target, sizeof target, "%s/sshd.img", share);
		{
			const char *const argv[] = { mke2fs, "-q", "-F", "-t", "ext4", "-d", sshd,
			                             target, "32M", NULL };

			if (finish(start(argv, -1, -1)) != 0) {
				say("the sshd's disk could not be made");
				return 1;
			}
		}
	}

	/* The outer vm's files. */
	snprintf(path, sizeof path, "%s/hull-agent", share);
	if (copy_file(agent, path) != 0) {
		say("the agent cannot be copied");
		return 1;
	}
	snprintf(path, sizeof path, "%s/run.sh", share);
	if ((file = fopen(path, "w")) == NULL || fputs(script, file) < 0 || fclose(file) != 0) {
		say("the outer vm's script cannot be written");
		return 1;
	}
	snprintf(path, sizeof path, "%s/hull.conf", share);
	if ((file = fopen(path, "w")) == NULL) {
		say("the outer vm's spec cannot be written");
		return 1;
	}
	/* The host's own kernel, (profile full): the outer vm loads the host's
	 * vboxdrv.ko, built for it, which hull's own kernel -- a vm's default
	 * wherever `make kernel` has built it -- could never take. */
	fprintf(file, "(machine (backend vm) (source \"dir:/\") (mutable overlay)\n"
	        " (tuning (vm (profile full)))\n"
	        " (memory \"%luM\") (cpus %lu)\n"
	        " (volumes (volume (host \"%s\") (guest \"/vbox\") (mode ro))\n"
	        "          (volume (host \"%s\") (guest \"/share\"))\n",
	        strtoul(memory, NULL, 10) + OUTER_MEMORY, strtoul(cpus, NULL, 10) + 1ul, vbox, share);
	/* Each of the machine's volumes a share of the outer vm's, and that
	 * a shared folder of VirtualBox's, by the name the agent mounts. */
	snprintf(target, sizeof target, "%s/folders.sh", share);
	if ((folders = fopen(target, "w")) == NULL) {
		say("the outer vm's script cannot be written");
		return 1;
	}
	for (unsigned i = 0u; i < volume_count; i++) {
		const char *host = volumes[2u * i];
		int         readonly = strcmp(volumes[2u * i + 1u], "ro") == 0;

		if (strpbrk(host, "\"\\") != NULL) {
			say("a volume's path holds a quote or a backslash");
			return 1;
		}
		fprintf(file, "          (volume (host \"%s\") (guest \"/volume/v%u\")%s)\n", host, i,
		        readonly ? " (mode ro)" : "");
		fprintf(folders, "$manage sharedfolder add hull --name v%u --hostpath /volume/v%u%s || "
		        "exit 1\n", i, i, readonly ? " --readonly" : "");
	}
	fprintf(file, " )\n (command \"/bin/sh\" \"/share/run.sh\" \"%s\" \"%s\" \"%s\" \"%s\"))\n",
	        memory, cpus, network, HULL_VBOX_VERSION);
	if (fclose(folders) != 0 || fclose(file) != 0) {
		say("the outer vm's spec cannot be written");
		return 1;
	}

	/* The outer vm; its script says when VirtualBox is about to start. */
	{
		const char *const argv[] = { self, "run", "-f", path, NULL };

		if ((outer = start(argv, -1, -1)) < 0)
			return 1;
	}
	snprintf(target, sizeof target, "%s/up", share);
	deadline = time(NULL) + OUTER_LIMIT;
	while (access(target, F_OK) != 0) {
		struct timespec pause = { 0, 100000000L };

		if (time(NULL) >= deadline || waitpid(outer, NULL, WNOHANG) != 0) {
			say("the outer vm did not reach VirtualBox");
			return 1;
		}
		nanosleep(&pause, NULL);
	}

	/* The wire, until the guest powers off and VirtualBox closes it. */
	{
		const char *const argv[] = { self, "exec", "share", "--", "/share/hull-agent", "relay",
		                             "/tmp/uart.sock", NULL };

		if ((link = start(argv, 0, 1)) < 0)
			return 1;
	}
	status = finish(link);
	finish(outer);
	return status;
}

/* --- a PC of its own media, the virtualbox-pc engine's ------------------- */

/* Run in the outer vm, as the hosted machine's script is, for a machine
 * made of its own media: VirtualBox made root's, then the machine created
 * with no agent in it, its drives attached by media.sh, its first serial
 * port a socket the relay joins to the run's output. Its arguments: the
 * memory, the processors, the network, the firmware, the card's type, the
 * chipset, the TPM, the boot order's first and the controller. */
static const char pc_script[] =
    "#!/bin/sh\n"
    "memory=$1 cpus=$2 network=$3 firmware=$4 nic=$5 chipset=$6 tpm=$7 boot=$8 bus=$9\n"
    "screen=${10} key=${11} gpu=${12}\n"
    "release=$(uname -r)\n"
    "insmod /lib/modules/$release/misc/vboxdrv.ko || {\n"
    "	echo \"no vboxdrv.ko for $release: the host's VirtualBox builds it\"; exit 1; }\n"
    "cp -a /vbox/usr/lib/virtualbox /usr/lib/virtualbox.hull || exit 1\n"
    "rm -rf /usr/lib/virtualbox\n"
    "mv /usr/lib/virtualbox.hull /usr/lib/virtualbox || exit 1\n"
    "chown -R 0:0 /usr/lib/virtualbox\n"
    "chmod -R go-w /usr/lib/virtualbox\n"
    "for f in VBoxHeadless VirtualBoxVM VBoxNetDHCP VBoxNetNAT VBoxNetAdpCtl; do\n"
    "	chmod 4511 /usr/lib/virtualbox/$f\n"
    "done\n"
    "chown 0:0 / /usr /usr/lib\n"
    "chmod 755 / /usr /usr/lib\n"
    "export HOME=/root VBOX_USER_HOME=/share/vbox\n"
    "mkdir -p /root $VBOX_USER_HOME\n"
    "manage=/usr/lib/virtualbox/VBoxManage\n"
    "$manage createvm --name hull --register --basefolder /share/vbox >/dev/null || exit 1\n"
    "card='--nic1 none'\n"
    /* VirtualBox's NAT on 10.0.3.0/24, not its default 10.0.2.0/24, which
     * is the outer vm's own: a forwarded port reached by the outer vm's
     * address 10.0.2.15 went to a guest of that same address and hung
     * (measured: a banner by 127.0.0.1:2222 in the outer vm, none by
     * 10.0.2.15:2222), and every port forwarded into the machine comes
     * by that address. */
    "[ \"$network\" = nat ] && card=\"--nic1 nat --nic-type1 $nic --natnet1 10.0.3.0/24\"\n"
    "$manage modifyvm hull --memory $memory --cpus $cpus --ioapic on --firmware $firmware \\\n"
    "	--chipset $chipset $card --audio-driver none --usb-ohci off \\\n"
    "	--graphicscontroller $(cat /share/graphics) --vram 128 --boot1 $boot --boot2 disk --boot3 dvd \\\n"
    "	--uart1 0x3f8 4 --uart-mode1 server /tmp/uart.sock || exit 1\n"
    "[ \"$tpm\" = yes ] && { $manage modifyvm hull --tpm-type 2.0 || exit 1; }\n"
    /* 3D: VMSVGA's, rendered by VirtualBox's DXVK on the outer vm's
     * Vulkan, the host's GPU by Venus, or llvmpipe by virgl. */
    "[ \"$gpu\" != none ] && { $manage modifyvm hull --accelerate-3d on || exit 1; }\n"
    /* Secure Boot, as Windows 11 asks: VirtualBox's EFI variables made,
     * Microsoft's keys and Oracle's platform key enrolled, enforced. */
    "if [ \"$firmware\" = efi ] && [ -f /share/secure ]; then\n"
    "	$manage modifynvram hull inituefivarstore || exit 1\n"
    "	$manage modifynvram hull enrollmssignatures || exit 1\n"
    "	$manage modifynvram hull enrollorclpk || exit 1\n"
    "	$manage modifynvram hull secureboot --enable || exit 1\n"
    "fi\n"
    /* Through the page cache: the media are files on the outer vm's
     * share, where VirtualBox's own uncached I/O stalled a Windows
     * install until the guest reset its disk's port (measured). */
    "$manage storagectl hull --name disks --add $bus --hostiocache on || exit 1\n"
    ". /share/media.sh\n"
    "[ -f /share/ports.sh ] && . /share/ports.sh\n"
    /* (boot_key N): Enter once a second for N seconds from when VirtualBox
     * has the machine running, as a person at the screen presses it for a
     * disc's \"Press any key to boot from CD\". */
    "if [ \"$key\" -gt 0 ]; then\n"
    "	( until $manage showvminfo hull --machinereadable 2>/dev/null | grep -q '^VMState=\"running\"'; do\n"
    "		sleep 1; done\n"
    "	i=0; while [ $i -lt $key ]; do sleep 1; i=$((i + 1))\n"
    "		$manage controlvm hull keyboardputscancode 1c 9c >/dev/null 2>&1; done ) &\n"
    "fi\n"
    "if [ \"$screen\" = none ]; then\n"
    "	touch /share/up\n"
    "	exec /usr/lib/virtualbox/VBoxHeadless --startvm hull\n"
    "fi\n"
    /* A screen: X on the outer vm's own, the devices named, there being
     * no udev to find them, and VirtualBox's window filling it. */
    "$manage setextradata global GUI/SuppressMessages all\n"
    "$manage setextradata global GUI/UpdateDate never\n"
    "$manage modifyvm hull --usb-ohci on --mouse usbtablet || exit 1\n"
    "{\n"
    "	printf 'Section \"ServerFlags\"\\n\\tOption \"AutoAddDevices\" \"false\"\\nEndSection\\n'\n"
    "	printf 'Section \"Device\"\\n\\tIdentifier \"gpu\"\\n\\tDriver \"modesetting\"\\nEndSection\\n'\n"
    "	n=0\n"
    "	for e in /dev/input/event*; do\n"
    "		printf 'Section \"InputDevice\"\\n\\tIdentifier \"in%s\"\\n\\tDriver \"libinput\"\\n' $n\n"
    "		printf '\\tOption \"Device\" \"%s\"\\nEndSection\\n' $e\n"
    "		n=$((n + 1))\n"
    "	done\n"
    "	printf 'Section \"Screen\"\\n\\tIdentifier \"screen\"\\n\\tDevice \"gpu\"\\nEndSection\\n'\n"
    "	printf 'Section \"ServerLayout\"\\n\\tIdentifier \"hull\"\\n\\tScreen \"screen\"\\n'\n"
    "	i=0\n"
    "	while [ $i -lt $n ]; do printf '\\tInputDevice \"in%s\"\\n' $i; i=$((i + 1)); done\n"
    "	printf 'EndSection\\n'\n"
    "} > /tmp/xorg.conf\n"
    /* No screensaver and no DPMS: X would turn the screen off after ten
     * idle minutes, while a guest installs with nobody at it. */
    "HOME=/tmp Xorg :0 -config /tmp/xorg.conf -logfile /tmp/Xorg.log -nolisten tcp -noreset \\\n"
    "	-s 0 -dpms vt1 \\\n"
    "	>/dev/null 2>&1 &\n"
    "i=0\n"
    "while ! DISPLAY=:0 xdpyinfo >/dev/null 2>&1 && [ $i -lt 300 ]; do sleep 0.1; i=$((i + 1)); done\n"
    "touch /share/up\n"
    /* Qt loads no plugin into a setuid program unless root owns it,
     * and the host's tree is nobody's in a share: the kinds a window
     * loads made root's in the overlay, as VirtualBox's own files are. */
    "qt=/usr/lib/x86_64-linux-gnu/qt6\n"
    "chown 0:0 /usr/lib/x86_64-linux-gnu $qt $qt/plugins\n"
    "for kind in platforms xcbglintegrations platformthemes platforminputcontexts \\\n"
    "	imageformats iconengines generic egldeviceintegrations; do\n"
    "	[ -d $qt/plugins/$kind ] && chown -R 0:0 $qt/plugins/$kind\n"
    "done\n"
    "export DISPLAY=:0 XDG_RUNTIME_DIR=/tmp\n"
    "exec /usr/lib/virtualbox/VirtualBoxVM --startvm hull --fullscreen\n";

static volatile sig_atomic_t pc_stopping;

static void pc_stop_asked(int signum)
{
	(void)signum;
	pc_stopping = 1;
}

/* The magic of an image VirtualBox opens as it is: VDI, VMDK, VHD; 0 for
 * one it does not, raw or qcow2 among them. */
static int vbox_format(const char *path)
{
	char    head[64] = "";
	int     fd = open(path, O_RDONLY | O_CLOEXEC);
	ssize_t n = fd >= 0 ? read(fd, head, sizeof head - 1u) : -1;

	if (fd >= 0)
		close(fd);
	if (n <= 0)
		return 0;
	return strncmp(head, "<<< ", 4u) == 0 || strncmp(head, "KDMV", 4u) == 0 ||
	       strncmp(head, "# Disk DescriptorFile", 21u) == 0 || strncmp(head, "conectix", 8u) == 0;
}

/* Everything under `dir`, then `dir`, for the work this process made. */
static int remove_entry(const char *path, const struct stat *st, int type, struct FTW *ftw)
{
	(void)st;
	(void)ftw;
	return type == FTW_DP ? rmdir(path) : unlink(path);
}

/* --- ports: the machine's address to the outer vm's ------------------- */

typedef struct proxy {
	int             listener;
	struct in_addr  to;
	unsigned        port;
} proxy;

/* One connection: bytes both ways until either end closes. */
static void *relay_pair(void *arg)
{
	int          *ends = arg;
	struct pollfd fds[2] = { { .fd = ends[0], .events = POLLIN }, { .fd = ends[1], .events = POLLIN } };
	char          buffer[16384];

	for (;;) {
		if (poll(fds, 2u, -1) < 0) {
			if (errno == EINTR)
				continue;
			break;
		}
		if (((fds[0].revents | fds[1].revents) & (POLLERR | POLLNVAL)) != 0)
			break;
		for (unsigned i = 0u; i < 2u; i++) {
			ssize_t got;

			if ((fds[i].revents & (POLLIN | POLLHUP)) == 0)
				continue;
			got = read(fds[i].fd, buffer, sizeof buffer);
			if (got <= 0)
				goto closed;
			for (ssize_t put = 0; put < got;) {
				ssize_t wrote = write(fds[1u - i].fd, buffer + put, (size_t)(got - put));

				if (wrote <= 0)
					goto closed;
				put += wrote;
			}
		}
	}
closed:
	close(ends[0]);
	close(ends[1]);
	free(ends);
	return NULL;
}

/* A listener's connections, each to the outer vm's address. */
static void *accept_loop(void *arg)
{
	proxy *p = arg;

	for (;;) {
		int                client = accept4(p->listener, NULL, NULL, SOCK_CLOEXEC);
		int                outer;
		int               *ends;
		pthread_t          thread;
		struct sockaddr_in at = { .sin_family = AF_INET, .sin_port = htons((uint16_t)p->port),
		                          .sin_addr = p->to };

		if (client < 0) {
			if (errno == EINTR || errno == ECONNABORTED)
				continue;
			return NULL;
		}
		if ((outer = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0)) < 0 ||
		    connect(outer, (struct sockaddr *)&at, sizeof at) != 0 ||
		    (ends = malloc(2u * sizeof *ends)) == NULL) {
			if (outer >= 0)
				close(outer);
			close(client);
			continue;
		}
		ends[0] = client;
		ends[1] = outer;
		if (pthread_create(&thread, NULL, relay_pair, ends) != 0) {
			close(client);
			close(outer);
			free(ends);
			continue;
		}
		pthread_detach(thread);
	}
}

/* The outer vm's address, as its core lists it: the machine `share`. */
static int outer_address(const char *self, struct in_addr *out)
{
	int   pipe_fds[2];
	pid_t child;
	char  listing[4096] = "";
	size_t have = 0u;

	if (pipe2(pipe_fds, O_CLOEXEC) != 0)
		return -1;
	{
		const char *const argv[] = { self, "ps", NULL };

		child = start(argv, -1, pipe_fds[1]);
	}
	close(pipe_fds[1]);
	for (ssize_t got; have + 1u < sizeof listing &&
	     (got = read(pipe_fds[0], listing + have, sizeof listing - 1u - have)) > 0;)
		have += (size_t)got;
	close(pipe_fds[0]);
	finish(child);
	for (char *line = strtok(listing, "\n"); line != NULL; line = strtok(NULL, "\n")) {
		char id[64];
		char address[64];

		if (sscanf(line, "%63s %63s", id, address) == 2 && strcmp(id, "share") == 0)
			return inet_pton(AF_INET, address, out) == 1 ? 0 : -1;
	}
	return -1;
}

/* Each of `ports`, HOST:GUEST, listened on at `address` and carried to
 * the outer vm's same port, which its own forward and VirtualBox's NAT
 * take on to the guest. Threads of this process, ending with it. */
static void forward_ports(const char *self, const char *address, const char *ports)
{
	struct in_addr outer;
	struct in_addr here;
	char           copy[512];
	char          *save = NULL;

	if (inet_pton(AF_INET, address, &here) != 1 || outer_address(self, &outer) != 0) {
		say("the ports could not be forwarded: the outer vm's address is not known");
		return;
	}
	snprintf(copy, sizeof copy, "%s", ports);
	for (char *each = strtok_r(copy, ",", &save); each != NULL; each = strtok_r(NULL, ",", &save)) {
		unsigned           host = 0u;
		unsigned           guest = 0u;
		int                one = 1;
		proxy             *p;
		pthread_t          thread;
		struct sockaddr_in at = { .sin_family = AF_INET, .sin_addr = here };

		if (sscanf(each, "%u:%u", &host, &guest) != 2 || (p = calloc(1u, sizeof *p)) == NULL)
			continue;
		at.sin_port = htons((uint16_t)host);
		p->to = outer;
		p->port = host;
		if ((p->listener = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0)) < 0 ||
		    setsockopt(p->listener, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one) != 0 ||
		    bind(p->listener, (struct sockaddr *)&at, sizeof at) != 0 ||
		    listen(p->listener, 16) != 0 || pthread_create(&thread, NULL, accept_loop, p) != 0) {
			char said[128];

			snprintf(said, sizeof said, "port %u could not be listened on at %s", host, address);
			say(said);
			if (p->listener >= 0)
				close(p->listener);
			free(p);
			continue;
		}
		pthread_detach(thread);
	}
}

int hull_vbox_pc(const char *work, const char *agent, const char *const *settings,
                 const char *mode, char *const *media, unsigned media_count)
{
	char        self[PATH_MAX];
	char        vbox[4096];
	char        share[4300];
	char        runtime[4300];
	char        path[4400];
	char        target[4400];
	const char *parent_end = strrchr(work, '/');
	const char *bus = strcmp(settings[5], "ich9") == 0 ? "sata" : "ide";
	const char *given_ports = getenv("HULL_VBOX_PORTS");
	const char *address = getenv("HULL_VBOX_ADDRESS");
	char        ports[512] = "";
	ssize_t     n = readlink("/proc/self/exe", self, sizeof self - 1u);
	pid_t       outer;
	pid_t       link;
	time_t      deadline;
	FILE       *file;
	FILE       *attach;
	int         status = 1;
	unsigned    disks = 0u;

	signal(SIGPIPE, SIG_IGN);
	prctl(PR_SET_PDEATHSIG, SIGKILL);
	if (n <= 0 || parent_end == NULL || hull_vbox_dir(vbox, sizeof vbox) != 0) {
		say("this process's own paths cannot be found");
		return 1;
	}
	self[n] = '\0';
	if (given_ports != NULL && strspn(given_ports, "0123456789:,") == strlen(given_ports))
		snprintf(ports, sizeof ports, "%s", given_ports);
	snprintf(path, sizeof path, "%s/%s", vbox, HULL_VBOX_HEADLESS);
	if (access(path, X_OK) != 0) {
		say("no VirtualBox has been fetched: `hull virtualbox fetch` downloads it, once");
		return 1;
	}
	snprintf(path, sizeof path, "%.*s", (int)(parent_end - work), work);
	mkdir(path, 0700);
	snprintf(share, sizeof share, "%s/share", work);
	if (mkdir(work, 0700) != 0 || mkdir(share, 0700) != 0) {
		say("its work directory cannot be made");
		return 1;
	}
	/* The outer vm's core in a runtime directory of its own, beside the
	 * user's where there is one: its display monitor is a socket there,
	 * whose path a deep cache would make too long for one (measured: no
	 * screen, from a test's cache). */
	{
		const char *parent = getenv("XDG_RUNTIME_DIR");

		/* Named for this process, as the work is, so the engine takes it
		 * away after a machine killed at the end of its grace. */
		if (parent != NULL && parent[0] == '/' && strlen(parent) < 60u)
			snprintf(runtime, sizeof runtime, "%s/vbox-pc-%ld", parent, (long)getpid());
		else
			snprintf(runtime, sizeof runtime, "%s/run", work);
		if (mkdir(runtime, 0700) != 0) {
			say("its runtime directory cannot be made");
			nftw(work, remove_entry, 16, FTW_DEPTH | FTW_PHYS);
			return 1;
		}
	}
	setenv("XDG_RUNTIME_DIR", runtime, 1);
	setenv("HULL_CORE_IDLE", "1", 1);

	snprintf(path, sizeof path, "%s/hull-agent", share);
	snprintf(target, sizeof target, "%s/run.sh", share);
	if (copy_file(agent, path) != 0 || (file = fopen(target, "w")) == NULL ||
	    fputs(pc_script, file) < 0 || fclose(file) != 0) {
		say("the outer vm's files cannot be written");
		goto done;
	}
	snprintf(path, sizeof path, "%s/hull.conf", share);
	snprintf(target, sizeof target, "%s/media.sh", share);
	if ((file = fopen(path, "w")) == NULL || (attach = fopen(target, "w")) == NULL) {
		say("the outer vm's spec cannot be written");
		goto done;
	}
	fprintf(file, "(machine (backend vm) (source \"dir:/\") (mutable overlay)\n"
	        " (tuning (vm (profile full))%s%s%s)%s\n"
	        " (memory \"%luM\") (cpus %lu)\n"
	        " (volumes (volume (host \"%s\") (guest \"/vbox\") (mode ro))\n"
	        "          (volume (host \"%s\") (guest \"/share\"))\n",
	        strcmp(settings[11], "none") != 0 ? " (qemu (gpu " : "",
	        strcmp(settings[11], "none") != 0 ? settings[11] : "",
	        strcmp(settings[11], "none") != 0 ? "))" : "",
	        strcmp(settings[8], "local") == 0 ? " (display local)" :
	        strcmp(settings[8], "window") == 0 ? " (display window)" : "",
	        strtoul(settings[0], NULL, 10) + OUTER_MEMORY, strtoul(settings[1], NULL, 10) + 1ul,
	        vbox, share);
	/* Each medium's folder a share of the outer vm's, read-only unless the
	 * machine's writes are kept; each medium attached from there. A raw
	 * disk is given by a VMDK descriptor naming it as a flat extent, the
	 * one way VirtualBox opens a raw file; under (mutable overlay) a disk
	 * is VirtualBox's immutable, its writes in a differencing image the
	 * next start throws away. */
	for (unsigned i = 0u; i < media_count; i++) {
		const char *type = media[2u * i];
		const char *file_path = media[2u * i + 1u];
		const char *slash = strrchr(file_path, '/');
		const char *drive = strcmp(type, "cdrom") == 0 ? "dvddrive" :
		                    strcmp(type, "floppy") == 0 ? "fdd" : "hdd";
		int         writable = (strcmp(type, "disk") == 0 || strcmp(type, "usb") == 0) &&
		                       strcmp(mode, "direct") == 0;
		char        inner[4400];

		if (slash == NULL || strpbrk(file_path, "\"\\'") != NULL) {
			say("a medium's path is not absolute, or holds a quote or a backslash");
			fclose(attach);
			fclose(file);
			goto done;
		}
		fprintf(file, "          (volume (host \"%.*s\") (guest \"/media/m%u\")%s)\n",
		        (int)(slash - file_path), file_path, i, writable ? "" : " (mode ro)");
		snprintf(inner, sizeof inner, "/media/m%u/%s", i, slash + 1);
		if ((strcmp(type, "disk") == 0 || strcmp(type, "usb") == 0) && !vbox_format(file_path)) {
			struct stat st;
			FILE       *descriptor;

			snprintf(target, sizeof target, "%s/raw%u.vmdk", share, i);
			if (stat(file_path, &st) != 0 || (descriptor = fopen(target, "w")) == NULL) {
				say("a raw disk's descriptor cannot be written");
				fclose(attach);
				fclose(file);
				goto done;
			}
			/* VirtualBox opens a descriptor only with an image UUID
			 * of its own, written back otherwise and refused. */
			{
				uint8_t id[16] = { 0 };
				int     random = open("/dev/urandom", O_RDONLY | O_CLOEXEC);

				if (random >= 0) {
					if (read(random, id, sizeof id) != (ssize_t)sizeof id)
						id[0] ^= (uint8_t)i;
					close(random);
				}
				id[6] = (uint8_t)((id[6] & 0x0fu) | 0x40u);
				id[8] = (uint8_t)((id[8] & 0x3fu) | 0x80u);
				fprintf(descriptor, "# Disk DescriptorFile\nversion=1\nCID=fffffffe\n"
				        "parentCID=ffffffff\ncreateType=\"monolithicFlat\"\n\n"
				        "RW %llu FLAT \"../media/m%u/%s\" 0\n\n"
				        "ddb.virtualHWVersion = \"4\"\nddb.adapterType = \"ide\"\n"
				        "ddb.uuid.image = \"%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-"
				        "%02x%02x%02x%02x%02x%02x\"\n"
				        "ddb.uuid.parent = \"00000000-0000-0000-0000-000000000000\"\n",
				        (unsigned long long)st.st_size / 512u, i, slash + 1, id[0], id[1],
				        id[2], id[3], id[4], id[5], id[6], id[7], id[8], id[9], id[10],
				        id[11], id[12], id[13], id[14], id[15]);
			}
			fclose(descriptor);
			snprintf(inner, sizeof inner, "/share/raw%u.vmdk", i);
		}
		/* A USB stick: a disk on VirtualBox's USB storage, removable as
		 * Windows Setup looks for its answer on, behind an xHCI. */
		if (strcmp(type, "usb") == 0) {
			fprintf(attach, "[ -n \"$stick\" ] || { $manage modifyvm hull --usb-xhci on && "
			        "$manage storagectl hull --name stick --add usb --hostiocache on; } || exit 1\n"
			        "stick=$((${stick:--1} + 1))\n"
			        "$manage storageattach hull --storagectl stick --port $stick --device 0 "
			        "--type hdd --medium '%s'%s || exit 1\n", inner,
			        writable ? "" : " --mtype immutable");
			continue;
		}
		if (strcmp(drive, "fdd") == 0) {
			fprintf(attach, "$manage storagectl hull --name floppy --add floppy --hostiocache on 2>/dev/null\n"
			        "$manage storageattach hull --storagectl floppy --port 0 --device 0 "
			        "--type fdd --medium '%s' || exit 1\n", inner);
			continue;
		}
		fprintf(attach, "$manage storageattach hull --storagectl disks --port %u --device %u "
		        "--type %s --medium '%s'%s || exit 1\n",
		        strcmp(bus, "ide") == 0 ? disks / 2u : disks,
		        strcmp(bus, "ide") == 0 ? disks % 2u : 0u, drive, inner,
		        strcmp(drive, "hdd") == 0 && !writable ? " --mtype immutable" : "");
		disks++;
	}
	/* Its display card, (vga ...) as VirtualBox names it: VMSVGA, VMware's,
	 * unless asked; VBoxSVGA, its own, for a Windows guest, whose Guest
	 * Additions' WDDM driver names that card alone (measured: their
	 * VBoxWddm.inf lists PCI\VEN_80EE&DEV_BEEF, and on VMSVGA Windows
	 * kept its Basic Display Adapter). */
	{
		const char *vga = getenv("HULL_VBOX_VGA");
		FILE       *marker;

		snprintf(target, sizeof target, "%s/graphics", share);
		if ((marker = fopen(target, "w")) == NULL ||
		    fprintf(marker, "%s\n", vga == NULL || strcmp(vga, "vmware") == 0 ? "vmsvga" :
		            strcmp(vga, "vbox") == 0 ? "vboxsvga" : strcmp(vga, "std") == 0 ? "vboxvga" :
		            "none") < 0 || fclose(marker) != 0) {
			say("the outer vm's files cannot be written");
			fclose(file);
			goto done;
		}
	}
	if (strcmp(settings[3], "efi-secure") == 0) {
		FILE *marker;

		snprintf(target, sizeof target, "%s/secure", share);
		if ((marker = fopen(target, "w")) == NULL || fclose(marker) != 0) {
			say("the outer vm's files cannot be written");
			fclose(file);
			goto done;
		}
	}
	fprintf(file, " )\n");
	/* Each of the machine's ports the outer vm's own, at its address, and
	 * VirtualBox's NAT forwarding it on to the guest's. */
	if (ports[0] != '\0') {
		char        copy[sizeof ports];
		char       *save = NULL;
		FILE       *forward;
		unsigned    count = 0u;

		snprintf(target, sizeof target, "%s/ports.sh", share);
		if ((forward = fopen(target, "w")) == NULL) {
			say("the outer vm's files cannot be written");
			fclose(file);
			goto done;
		}
		fprintf(file, " (ports");
		snprintf(copy, sizeof copy, "%s", ports);
		for (char *each = strtok_r(copy, ",", &save); each != NULL; each = strtok_r(NULL, ",", &save)) {
			unsigned host = 0u;
			unsigned guest = 0u;

			if (sscanf(each, "%u:%u", &host, &guest) != 2)
				continue;
			fprintf(file, " (port (host %u) (guest %u))", host, host);
			fprintf(forward, "$manage modifyvm hull --natpf1 'p%u,tcp,,%u,,%u' || exit 1\n",
			        count++, host, guest);
		}
		fprintf(file, ")\n");
		fclose(forward);
	}
	fprintf(file, " (command \"/bin/sh\" \"/share/run.sh\"");
	for (unsigned i = 0u; i < 8u; i++)
		fprintf(file, " \"%s\"", i == 3u && strcmp(settings[3], "efi-secure") == 0 ? "efi" :
		        settings[i]);
	fprintf(file, " \"%s\" \"%s\" \"%s\" \"%s\"))\n", bus, settings[8], settings[10],
	        settings[11]);
	if (fclose(attach) != 0 || fclose(file) != 0) {
		say("the outer vm's spec cannot be written");
		goto done;
	}

	{
		const char *const argv[] = { self, "run", "-f", path, NULL };

		if ((outer = start(argv, -1, -1)) < 0)
			goto done;
	}
	snprintf(target, sizeof target, "%s/up", share);
	deadline = time(NULL) + OUTER_LIMIT;
	while (access(target, F_OK) != 0) {
		struct timespec pause = { 0, 100000000L };

		if (time(NULL) >= deadline || waitpid(outer, NULL, WNOHANG) != 0) {
			say("the outer vm did not reach VirtualBox");
			kill(outer, SIGTERM);
			finish(outer);
			goto done;
		}
		nanosleep(&pause, NULL);
	}
	/* hull-gui's screen: the outer vm's display monitor, the one its
	 * core links, linked where the engine said, which the user's own
	 * core's link for this machine's address names. */
	if (strcmp(settings[8], "local") == 0 && strcmp(settings[9], "-") != 0) {
		char       links[4400];
		DIR       *each;
		struct dirent *entry;

		snprintf(links, sizeof links, "%s/hull/display", runtime);
		if ((each = opendir(links)) != NULL) {
			while ((entry = readdir(each)) != NULL) {
				char found[4700];
				char real[PATH_MAX];

				if (entry->d_name[0] == '.')
					continue;
				snprintf(found, sizeof found, "%s/%s", links, entry->d_name);
				if (realpath(found, real) != NULL) {
					unlink(settings[9]);
					if (symlink(real, settings[9]) != 0)
						say("the screen could not be linked");
				}
				break;
			}
			closedir(each);
		}
	}
	/* The ports at the machine's own address, each to the outer vm's. */
	if (ports[0] != '\0' && address != NULL && address[0] != '\0')
		forward_ports(self, address, ports);
	/* A stop presses the machine's power button, as qemu-pc's does; the
	 * engine's KILL after its grace takes everything else with this. */
	{
		struct sigaction asked = { .sa_handler = pc_stop_asked };

		/* No SA_RESTART: the wait below must see the stop. */
		sigaction(SIGTERM, &asked, NULL);
	}
	{
		const char *const argv[] = { self, "exec", "share", "--", "/share/hull-agent", "relay",
		                             "/tmp/uart.sock", NULL };

		/* Its stdin the run's, typed into COM1; at its end it would end. */
		if ((link = start(argv, 0, 1)) < 0) {
			kill(outer, SIGTERM);
			finish(outer);
			goto done;
		}
	}
	for (;;) {
		int   gone;
		pid_t ended = waitpid(-1, &gone, 0);

		if (ended < 0 && errno == EINTR && pc_stopping == 1) {
			const char *const argv[] = { self, "exec", "share", "--", "/bin/sh", "-c",
			                             "HOME=/root VBOX_USER_HOME=/share/vbox "
			                             "/usr/lib/virtualbox/VBoxManage controlvm hull "
			                             "acpipowerbutton", NULL };

			pc_stopping = 2;
			start(argv, -1, -1);
			continue;
		}
		if (ended < 0 && errno == EINTR)
			continue;
		if (ended < 0 || ended == outer) {
			status = ended == outer && WIFEXITED(gone) ? WEXITSTATUS(gone) : 1;
			break;
		}
	}
	kill(link, SIGTERM);
	finish(link);
done:
	nftw(runtime, remove_entry, 16, FTW_DEPTH | FTW_PHYS);
	nftw(work, remove_entry, 16, FTW_DEPTH | FTW_PHYS);
	return status;
}
