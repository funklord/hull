/*
 * doctor.c -- `hull doctor`: what this host gives a user, against what
 * hull can use, each missing setting with the one-time line that would
 * give it (section 7, "What needs root, and what only seems to").
 *
 * One line a check, its first word its status, so a script reads it:
 * `ok`, `missing`, `limited` or `absent`, and for what is missing a line
 * after it, `  fix (as root): ...`. It changes nothing: user namespaces
 * are tried in a child that exits, and nothing else is more than read.
 * The status is 1 only where what everything needs cannot be had: user
 * namespaces, which every rootless engine runs in, or a runtime directory,
 * which the core does; and 0 otherwise.
 */

#define _GNU_SOURCE

#include <grp.h>
#include <pwd.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <sys/vfs.h>
#include <sys/wait.h>
#include <sys/xattr.h>
#include <unistd.h>

#include "doctor.h"
#include "kernel.h"
#include "sshd.h"
#include "vbox.h"

#define TMPFS_MAGIC 0x01021994
#define RAMFS_MAGIC 0x858458f6

static void line(const char *status, const char *check, const char *detail)
{
	printf("%-8s %s%s%s\n", status, check, detail[0] != '\0' ? ": " : "", detail);
}

static void fix(const char *text)
{
	printf("  fix (as root): %s\n", text);
}

/* Whether the space-separated list `words` holds `word`. */
static int has_word(const char *words, const char *word)
{
	size_t n = strlen(word);

	for (const char *at = words; (at = strstr(at, word)) != NULL; at += n)
		if ((at == words || at[-1] == ' ') && (at[n] == ' ' || at[n] == '\0'))
			return 1;
	return 0;
}

/* The first line of a /proc or /sys file, its newline off, or 0. */
static int read_line(const char *path, char *out, size_t cap)
{
	FILE *f = fopen(path, "re");
	int   ok;

	if (f == NULL)
		return 0;
	ok = fgets(out, (int)cap, f) != NULL;
	fclose(f);
	if (ok)
		out[strcspn(out, "\n")] = '\0';
	return ok;
}

static int user_namespaces(void)
{
	char  value[64];
	pid_t child = fork();
	int   status = 0;

	if (child == 0)
		_exit(unshare(CLONE_NEWUSER) == 0 ? 0 : 1);
	if (child < 0 || waitpid(child, &status, 0) != child || !WIFEXITED(status) ||
	    WEXITSTATUS(status) != 0) {
		line("missing", "user namespaces", "this user cannot make one, and no rootless engine runs");
		if (read_line("/proc/sys/kernel/apparmor_restrict_unprivileged_userns", value, sizeof value) &&
		    strcmp(value, "1") == 0)
			fix("make install-apparmor (a profile that lets hull make user "
			    "namespaces), or sysctl kernel.apparmor_restrict_unprivileged_userns=0");
		if (read_line("/proc/sys/user/max_user_namespaces", value, sizeof value) &&
		    strcmp(value, "0") == 0)
			fix("sysctl user.max_user_namespaces=15000");
		if (read_line("/proc/sys/kernel/unprivileged_userns_clone", value, sizeof value) &&
		    strcmp(value, "0") == 0)
			fix("sysctl kernel.unprivileged_userns_clone=1");
		return 0;
	}
	line("ok", "user namespaces", "");
	return 1;
}

/* Whether `file` names `name` or `uid` with a range, as name:start:count. */
static int has_range(const char *file, const char *name, uid_t uid)
{
	FILE *f = fopen(file, "re");
	char  text[512];
	char  id[32];
	int   found = 0;

	if (f == NULL)
		return 0;
	snprintf(id, sizeof id, "%u", (unsigned)uid);
	while (!found && fgets(text, sizeof text, f) != NULL) {
		char *colon = strchr(text, ':');

		if (colon == NULL)
			continue;
		*colon = '\0';
		found = strcmp(text, name) == 0 || strcmp(text, id) == 0;
	}
	fclose(f);
	return found;
}

/* A helper that can map ranges: setuid root, or holding a capability. */
static int mapper(const char *path)
{
	struct stat st;

	if (stat(path, &st) != 0)
		return 0;
	return (st.st_uid == 0 && (st.st_mode & S_ISUID) != 0) ||
	       getxattr(path, "security.capability", NULL, 0) > 0;
}

static void subordinate_ids(const char *name, uid_t uid)
{
	char text[512];
	int  ranges = has_range("/etc/subuid", name, uid) && has_range("/etc/subgid", name, uid);
	int  helpers = (mapper("/usr/bin/newuidmap") || mapper("/bin/newuidmap")) &&
	               (mapper("/usr/bin/newgidmap") || mapper("/bin/newgidmap"));

	if (ranges && helpers) {
		line("ok", "many uids in a machine", "subuid and subgid ranges, and newuidmap");
		return;
	}
	line("limited", "many uids in a machine",
	     "one uid maps, so an image's other users, apt's _apt among them, are the user's");
	if (!ranges) {
		snprintf(text, sizeof text, "usermod --add-subuids 100000-165535 --add-subgids "
		         "100000-165535 %s", name);
		fix(text);
	}
	if (!helpers)
		fix("apt install uidmap (newuidmap and newgidmap, setuid root)");
}

static void overlay(void)
{
	struct utsname host;
	unsigned       major = 0u;
	unsigned       minor = 0u;

	if (uname(&host) == 0)
		sscanf(host.release, "%u.%u", &major, &minor);
	if (major > 5u || (major == 5u && minor >= 11u))
		line("ok", "overlay in a user namespace", host.release);
	else
		line("limited", "overlay in a user namespace",
		     "a kernel before 5.11, so (mutable overlay) is refused on chroot");
}

static void cgroups(void)
{
	char own[4096];
	char path[4600];
	char controllers[512] = "";

	if (access("/sys/fs/cgroup/cgroup.controllers", R_OK) != 0) {
		line("limited", "cpu and memory limits", "no cgroup v2, so cpus and memory go unenforced");
		return;
	}
	if (read_line("/proc/self/cgroup", own, sizeof own) && strncmp(own, "0::", 3u) == 0) {
		snprintf(path, sizeof path, "/sys/fs/cgroup%s/cgroup.controllers", own + 3);
		read_line(path, controllers, sizeof controllers);
		snprintf(path, sizeof path, "/sys/fs/cgroup%s/cgroup.subtree_control", own + 3);
		if (has_word(controllers, "memory") && has_word(controllers, "cpu") &&
		    access(path, W_OK) == 0) {
			line("ok", "cpu and memory limits", "cgroup v2, delegated to this user");
			return;
		}
	}
	/* A cgroup the user owns is not enough: a process moves only by one
	 * who may write the cgroup.procs of the lowest cgroup holding both
	 * where it is and where it goes, which is root's, so the user's
	 * processes must be started in it, as a login or a service is. */
	line("limited", "cpu and memory limits",
	     "cgroup v2 is not delegated to this user, so cpus and memory go unenforced on ns and "
	     "chroot, a vm's holding through qemu's own");
	fix("systemd's user@.service delegates it at login; without systemd, pam_cgfs "
	    "(apt install libpam-cgfs) in the session's PAM stack does, for a login or an ssh session");
	fix("for a service with no login, its init script makes a cgroup, chowns it to the user, "
	    "and starts the service in it");
}

static void ports(void)
{
	char value[64];

	if (!read_line("/proc/sys/net/ipv4/ip_unprivileged_port_start", value, sizeof value)) {
		line("limited", "ports below 1024", "the setting could not be read");
		return;
	}
	if (atoi(value) <= 80) {
		line("ok", "ports below 1024", value[0] == '0' ? "any" : value);
		return;
	}
	line("limited", "ports below 1024", "a machine's port 80 cannot be bound on its address");
	fix("sysctl net.ipv4.ip_unprivileged_port_start=80");
}

static void ping(void)
{
	char         value[64];
	unsigned     low = 1u;
	unsigned     high = 0u;
	unsigned     gid = (unsigned)getgid();

	if (read_line("/proc/sys/net/ipv4/ping_group_range", value, sizeof value))
		sscanf(value, "%u %u", &low, &high);
	if (low <= gid && gid <= high) {
		line("ok", "ping", "");
		return;
	}
	line("limited", "ping", "this user's group may not send ICMP echo");
	fix("sysctl net.ipv4.ping_group_range='0 2147483647'");
}

/* A device the user must open: ok, absent, or missing with the group to
 * join. `instead`, where hull does without it, makes missing limited and
 * says what it does then. */
static void device(const char *path, const char *what, const char *name, const char *instead)
{
	struct stat   st;
	char          text[512];
	struct group *owner;

	if (stat(path, &st) != 0) {
		line("absent", what, "the device is not on this host");
		return;
	}
	if (access(path, R_OK | W_OK) == 0) {
		line("ok", what, path);
		return;
	}
	if (instead != NULL)
		line("limited", what, instead);
	else
		line("missing", what, "the device is not this user's to open");
	owner = getgrgid(st.st_gid);
	snprintf(text, sizeof text, "usermod -aG %s %s", owner != NULL ? owner->gr_name : "GROUP", name);
	fix(text);
}

/* A vm's kernel: the host's own where the user may read it, else one
 * `hull kernel fetch` put in the cache (Appendix F). Not root's to fix. */
static void vm_kernel(void)
{
	struct utsname host;
	const char    *boot = getenv("HULL_BOOT_DIR");
	char           path[4400];
	char           fetched[4096];

	if (uname(&host) != 0)
		return;
	/* HULL_BOOT_DIR names another /boot, as the engine reads it. */
	snprintf(path, sizeof path, "%s/vmlinuz-%s",
	         boot != NULL && boot[0] == '/' ? boot : "/boot", host.release);
	if (access(path, R_OK) == 0) {
		line("ok", "a vm's kernel, the host's", path);
		return;
	}
	if (hull_kernel_dir(fetched, sizeof fetched) == 0) {
		snprintf(path, sizeof path, "%s/boot/vmlinuz-%s", fetched, HULL_KERNEL_RELEASE);
		if (access(path, R_OK) == 0) {
			line("ok", "a vm's kernel, fetched", path);
			return;
		}
	}
	line("missing", "a vm's kernel", "the host's cannot be read by this user, and none is fetched");
	printf("  fix (as the user): hull kernel fetch\n");
}

/* The virtualbox engine's two needs: the package `hull virtualbox fetch`
 * puts in the cache, and the host's vboxdrv.ko for this kernel, which the
 * outer vm loads. Neither matters to a host that runs no such machine. */
static void virtualbox(void)
{
	struct utsname name;
	char           dir[4096];
	char           path[4400];

	if (hull_vbox_dir(dir, sizeof dir) != 0)
		dir[0] = '\0';
	snprintf(path, sizeof path, "%s/%s", dir, HULL_VBOX_HEADLESS);
	if (dir[0] == '\0' || access(path, X_OK) != 0) {
		line("absent", "VirtualBox for (backend virtualbox)", "not fetched");
		printf("  fix (as the user): hull virtualbox fetch\n");
		return;
	}
	if (uname(&name) != 0)
		return;
	snprintf(path, sizeof path, "/lib/modules/%s/misc/vboxdrv.ko", name.release);
	if (access(path, R_OK) != 0) {
		line("absent", "VirtualBox for (backend virtualbox)", "no vboxdrv.ko for this kernel");
		fix("install VirtualBox " HULL_VBOX_VERSION " on the host, which builds it");
		return;
	}
	line("ok", "VirtualBox for (backend virtualbox)", dir);
}

/* The door of a pc machine, (ssh true) there being the host's own OpenSSH
 * (section 8.1). Absent rather than missing: only such a machine wants it. */
static void host_sshd(void)
{
	static const char *const places[] = { "/usr/sbin/sshd", "/usr/local/sbin/sshd", "/sbin/sshd",
	                                      NULL };

	for (const char *const *place = places; *place != NULL; place++)
		if (access(*place, X_OK) == 0) {
			line("ok", "the host's sshd, the door of a pc machine", *place);
			return;
		}
	line("absent", "the host's sshd, the door of a pc machine", "(ssh true) on one is refused");
	fix("install the host's openssh-server");
}

/* (ssh true)'s sshd for an image with none, which `hull sshd fetch` puts
 * in the cache (section 8.1 item 3). Absent rather than missing: only a
 * machine whose image has no sshd of its own needs it. */
static void fetched_sshd(void)
{
	char dir[4096];
	char path[4200];

	if (hull_sshd_dir(dir, sizeof dir) == 0) {
		snprintf(path, sizeof path, "%s/%s", dir, HULL_SSHD_DROPBEAR);
		if (access(path, R_OK) == 0) {
			line("ok", "an sshd for an image with none, fetched", dir);
			return;
		}
	}
	line("absent", "an sshd for an image with none", "(ssh true) needs the image's own until fetched");
	printf("  fix (as the user): hull sshd fetch\n");
}

static int runtime(const char *why)
{
	const char   *dir = getenv("XDG_RUNTIME_DIR");
	struct statfs fs;
	char          text[4400];

	if (dir == NULL || dir[0] == '\0') {
		snprintf(text, sizeof text, "none would do: %s", why);
		line("missing", "runtime directory", text);
		return 0;
	}
	if (statfs(dir, &fs) == 0 && (fs.f_type == (__fsword_t)TMPFS_MAGIC ||
	                              fs.f_type == (__fsword_t)RAMFS_MAGIC)) {
		line("ok", "runtime directory", dir);
		return 1;
	}
	snprintf(text, sizeof text, "%s, which is not in memory, so a machine's secrets are refused", dir);
	line("limited", "runtime directory", text);
	return 1;
}

/* Where a program is: on PATH, or in the sbin directories a user's PATH
 * leaves out, as Debian's has mkfs.ext4 in /usr/sbin. */
static int find_program(const char *name, char *out, size_t cap)
{
	const char *path = getenv("PATH");
	char        dirs[4200];

	snprintf(dirs, sizeof dirs, "%s:/usr/local/sbin:/usr/sbin:/sbin",
	         path != NULL ? path : "/usr/bin:/bin");
	for (char *dir = strtok(dirs, ":"); dir != NULL; dir = strtok(NULL, ":"))
		if ((size_t)snprintf(out, cap, "%s/%s", dir, name) < cap && access(out, X_OK) == 0)
			return 1;
	return 0;
}

static void tool(const char *name, const char *for_what)
{
	char found[4400];
	char text[4600];

	if (!find_program(name, found, sizeof found)) {
		line("absent", name, for_what);
		return;
	}
	snprintf(text, sizeof text, "%s (%s)", for_what, found);
	line("ok", name, text);
}

int hull_doctor(const char *runtime_why)
{
	struct passwd *me = getpwuid(getuid());
	const char    *name = me != NULL ? me->pw_name : "USER";
	int            usable;

	usable = user_namespaces();
	subordinate_ids(name, getuid());
	overlay();
	cgroups();
	ports();
	ping();
	device("/dev/kvm", "vm and emu machines (/dev/kvm)", name, NULL);
	device("/dev/vhost-vsock", "a vm's agent over vsock (/dev/vhost-vsock)", name,
	       "not this user's to open: a vm's agent is reached by a serial port instead");
	device("/dev/fuse", "FUSE, an image read or changed without a mount (/dev/fuse)", name, NULL);
	vm_kernel();
	fetched_sshd();
	host_sshd();
	virtualbox();
	usable = runtime(runtime_why) && usable;
	tool("passt", "a vm's network");
	tool("qemu-system-x86_64", "vm and emu machines on x86_64");
	tool("docker", "the docker engine");
	tool("mkfs.ext4", "an ext4 image made from a directory, -d, with no loop mount");
	tool("mkfs.erofs", "an EROFS image made from a directory");
	tool("mksquashfs", "a squashfs image made from a directory");
	tool("fuse2fs", "an ext4 image read or changed through FUSE");
	tool("squashfuse", "a squashfs image read through FUSE");
	return usable ? 0 : 1;
}
