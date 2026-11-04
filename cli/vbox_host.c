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

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
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
	fprintf(file, "(machine (backend vm) (source \"dir:/\") (mutable overlay)\n"
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
