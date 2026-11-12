/*
 * hull_root.c -- hull-root, root mode's helper (project.md section 7).
 *
 *   hull-root start HASH FIELD...   may this user run the plan HASH with
 *                                   these root fields? logged either way
 *   hull-root exec HASH             an exec into such a machine, logged
 *   hull-root stop HASH             its stop, logged
 *   hull-root bridge HASH PID       the plan on stdin: its machine, PID, a
 *                                   process of the user's own in network
 *                                   and user namespaces of its own, put on
 *                                   the host bridge the plan names, hull0
 *                                   made where it is its own, and hull0's
 *                                   DNS relay's port 53 bound and handed
 *                                   back on fd 3 where it is asked
 *   hull-root tap HASH              the plan on stdin: a tap on the host
 *                                   bridge the plan names, as for bridge,
 *                                   handed back on fd 3 for a vm's qemu,
 *                                   the relay's sockets after it where
 *                                   asked; not persistent, so it goes when
 *                                   the last descriptor of it closes
 *   hull-root device HASH NAME      the plan on stdin: each device its
 *                                   (devices (root ...)) names, a node of
 *                                   the host's the user may not open, made
 *                                   again as a node of the user's, of the
 *                                   same kind and number, in
 *                                   /run/hull-root/UID/NAME, on a tmpfs of
 *                                   root mode's own that is not nodev, for
 *                                   the engine to bind in its place
 *
 * Installed setuid root by `make install-root` and by nothing else; a host
 * without it has no root mode. It does the least root has to: it reads the
 * host owner's listing, /etc/hull/root.conf, and appends to the host's
 * audit log, /var/log/hull/root.log, and runs nothing. Each line of the
 * listing is `USER sha256:HEX FIELD...`: that user may run that plan with
 * those fields, `#` a comment. A start is granted only where a line names
 * the calling user, by its real uid, the plan, and every field asked; an
 * exec or a stop only where a line names the user and the plan. Every
 * call is logged, a refusal too, and one whose line cannot be written is
 * refused: root is never used unrecorded.
 *
 * Setuid, it reads no environment and trusts its two files only where
 * root owns them, and the directories holding them, and nobody else may
 * write them. Run as the user, as the suites run it, it honours
 * HULL_ROOT_CONF and HULL_ROOT_LOG for a listing and a log of their own:
 * it can then grant nothing the user could not do already.
 *
 * An op that acts as root acts only on what the plan it is handed says,
 * and only where that plan's own hash, worked out here from its bytes, is
 * the one listed: the listing names a hash, and the hash is the machine,
 * so nothing the caller asks can reach past what root agreed to.
 *
 * Exits 0 granted, 77 not listed (with the line that would list it), 1
 * anything else.
 */

#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <net/if.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/uio.h>
#include <time.h>
#include <unistd.h>

#include <limits.h>
#include <linux/if_tun.h>

#include "hostnet.h"
#include "sha256.h"

#define CONF_PATH "/etc/hull/root.conf"
#define LOG_PATH  "/var/log/hull/root.log"
#define MAX_FIELDS 32
#define MAX_LISTING (1 << 20)

#define MAX_PLAN (1 << 20)

/* Where the device op makes its nodes: a tmpfs of its own, mounted here
 * where it is not yet, nosuid and noexec but not nodev, which /run is. */
#define DEVICES_PATH "/run/hull-root"

/* --- the plan, canonical ------------------------------------------------- */

/* Where the element at `at` of the canonical bytes ends, or 0 where it is
 * not one: an atom, digits, a colon and that many bytes, or a list of
 * them in parentheses. */
static size_t element_end(const uint8_t *data, size_t length, size_t at, unsigned depth)
{
	if (at >= length || depth > 64u)
		return 0u;
	if (data[at] == '(') {
		at++;
		while (at < length && data[at] != ')') {
			size_t end = element_end(data, length, at, depth + 1u);

			if (end == 0u)
				return 0u;
			at = end;
		}
		return at < length ? at + 1u : 0u;
	}
	{
		size_t n = 0u;

		if (data[at] < '0' || data[at] > '9' || (data[at] == '0' && at + 1u < length &&
		                                         data[at + 1u] != ':'))
			return 0u;
		while (at < length && data[at] >= '0' && data[at] <= '9') {
			if (n > MAX_PLAN)
				return 0u;
			n = n * 10u + (size_t)(data[at++] - '0');
		}
		if (at >= length || data[at] != ':' || n > length - at - 1u)
			return 0u;
		return at + 1u + n;
	}
}

/* The atom at `at`, its bytes and their count, or 0 where it is no atom. */
static int atom_at(const uint8_t *data, size_t length, size_t at, const uint8_t **bytes,
                   size_t *n)
{
	size_t end = element_end(data, length, at, 0u);
	size_t colon = at;

	if (end == 0u || data[at] == '(')
		return 0;
	while (data[colon] != ':')
		colon++;
	*bytes = data + colon + 1u;
	*n = end - colon - 1u;
	return 1;
}

static int atom_is(const uint8_t *bytes, size_t n, const char *text)
{
	return n == strlen(text) && memcmp(bytes, text, n) == 0;
}

/* In the list at `list` (whose head is its key), the entry `(KEY VALUE)`
 * named `key`: where it starts and ends, and where its value starts. */
static int entry_of(const uint8_t *data, size_t length, size_t list, const char *key,
                    size_t *start, size_t *end, size_t *value)
{
	size_t at = list + 1u;

	if (list >= length || data[list] != '(' || (at = element_end(data, length, at, 0u)) == 0u)
		return 0;
	while (at < length && data[at] != ')') {
		size_t         next = element_end(data, length, at, 0u);
		const uint8_t *bytes;
		size_t         n;

		if (next == 0u)
			return 0;
		if (data[at] == '(' && atom_at(data, length, at + 1u, &bytes, &n) &&
		    atom_is(bytes, n, key)) {
			*start = at;
			*end   = next;
			*value = element_end(data, length, at + 1u, 0u);
			return *value != 0u && *value < next - 1u;
		}
		at = next;
	}
	return 0;
}

/* The atom value of `(KEY VALUE)` in the list at `list`, as text into
 * `out`; 0 where there is none, or it is no atom, or too long. */
static int value_of(const uint8_t *data, size_t length, size_t list, const char *key, char *out,
                    size_t cap)
{
	size_t         start;
	size_t         end;
	size_t         value;
	const uint8_t *bytes;
	size_t         n;

	if (!entry_of(data, length, list, key, &start, &end, &value) ||
	    !atom_at(data, length, value, &bytes, &n) || n >= cap || memchr(bytes, '\0', n) != NULL)
		return 0;
	memcpy(out, bytes, n);
	out[n] = '\0';
	return 1;
}

/* The plan's root hash, worked out as hull's planner works it out
 * (plan/matrix.c's hull_matrix_root_hash): over "hull root 1", the engine
 * and class, each ended by a NUL, and the machine's canonical bytes,
 * which the plan carries as they are. Its machine's place into `*machine`.
 * 0, or -1 where the plan is no plan. */
static int plan_hash(const uint8_t *data, size_t length, char hex[72], size_t *machine)
{
	char        engine[64];
	char        class_name[16];
	size_t      start;
	size_t      end;
	size_t      value;
	hull_sha256 h;
	uint8_t     digest[32];

	if (element_end(data, length, 0u, 0u) != length || data[0] != '(' ||
	    !value_of(data, length, 0u, "engine", engine, sizeof engine) ||
	    !value_of(data, length, 0u, "class", class_name, sizeof class_name) ||
	    !entry_of(data, length, 0u, "machine", &start, &end, &value))
		return -1;
	hull_sha256_init(&h);
	hull_sha256_update(&h, "hull root 1\n", 12u);
	hull_sha256_update(&h, engine, strlen(engine) + 1u);
	hull_sha256_update(&h, class_name, strlen(class_name) + 1u);
	hull_sha256_update(&h, data + start, end - start);
	hull_sha256_final(&h, digest);
	memcpy(hex, "sha256:", 7u);
	hull_sha256_hex(digest, hex + 7);
	*machine = start;
	return 0;
}

static int fail(const char *what, const char *detail)
{
	fprintf(stderr, "hull-root: %s%s%s\n", what, detail != NULL ? ": " : "",
	        detail != NULL ? detail : "");
	return 1;
}

/* Only these bytes: a hash is sha256: and 64 hex digits, a field the
 * matrix's ids, a user the names useradd makes. */
static int made_of(const char *text, const char *allowed, size_t most)
{
	size_t n = strlen(text);

	return n > 0u && n <= most && strspn(text, allowed) == n;
}

static int is_hash(const char *text)
{
	return strncmp(text, "sha256:", 7u) == 0 && strlen(text) == 71u &&
	       made_of(text + 7, "0123456789abcdef", 64u);
}

static int is_field(const char *text)
{
	return made_of(text, "abcdefghijklmnopqrstuvwxyz0123456789._-", 96u);
}

/* Root's, and nobody else's to write: the file at `fd` and, by `path`,
 * the directory holding it. 0, or why. */
static const char *roots_alone(int fd, const char *path)
{
	struct stat file;
	struct stat dir;
	char        parent[256];
	const char *slash = strrchr(path, '/');

	snprintf(parent, sizeof parent, "%.*s", slash != NULL && slash != path ?
	         (int)(slash - path) : 1, path);
	if (fstat(fd, &file) != 0 || stat(parent, &dir) != 0)
		return "it cannot be looked at";
	if (file.st_uid != 0 || (file.st_mode & 022) != 0 || dir.st_uid != 0 ||
	    (dir.st_mode & 022) != 0)
		return "it, or its directory, is not root's alone";
	return NULL;
}

/* The listing's verdict for `user`, `hash` and `fields`: 0 a line grants
 * them all, 77 none does, 1 the listing cannot be read. */
static int listed(int fd, const char *user, const char *hash, char *const *fields,
                  int field_count)
{
	static char listing[MAX_LISTING + 1];
	size_t      length = 0u;
	ssize_t     got = 0;
	char       *line;
	char       *next;

	while (length < MAX_LISTING &&
	       (got = read(fd, listing + length, MAX_LISTING - length)) > 0)
		length += (size_t)got;
	if (got < 0 || length == MAX_LISTING)
		return 1;
	listing[length] = '\0';
	for (line = listing; line != NULL; line = next) {
		char *words[2 + MAX_FIELDS];
		int   count = 0;
		char *save = NULL;
		int   all = 1;

		next = strchr(line, '\n');
		if (next != NULL)
			*next++ = '\0';
		if (strchr(line, '#') != NULL)
			*strchr(line, '#') = '\0';
		for (char *word = strtok_r(line, " \t", &save); word != NULL && count < 2 + MAX_FIELDS;
		     word = strtok_r(NULL, " \t", &save))
			words[count++] = word;
		if (count < 2 || strcmp(words[0], user) != 0 || strcmp(words[1], hash) != 0)
			continue;
		for (int i = 0; i < field_count && all; i++) {
			int found = 0;

			for (int k = 2; k < count && !found; k++)
				found = strcmp(words[k], fields[i]) == 0;
			all = found;
		}
		if (all)
			return 0;
	}
	return 77;
}

/* Whether PID is a process of `uid`'s, in network and user namespaces
 * not this one's, as a rootless machine's init is: what the bridge op may
 * put a NIC into. */
static int machine_process(long pid, uid_t uid)
{
	char        path[64];
	char        line[256];
	FILE       *status;
	unsigned    real = 0u;
	int         found = 0;
	struct stat theirs;
	struct stat ours;

	snprintf(path, sizeof path, "/proc/%ld/status", pid);
	if ((status = fopen(path, "re")) == NULL)
		return 0;
	while (!found && fgets(line, sizeof line, status) != NULL)
		found = sscanf(line, "Uid: %u", &real) == 1;
	fclose(status);
	if (!found || real != (unsigned)uid)
		return 0;
	for (int i = 0; i < 2; i++) {
		const char *ns = i == 0 ? "net" : "user";

		snprintf(path, sizeof path, "/proc/%ld/ns/%s", pid, ns);
		if (stat(path, &theirs) != 0)
			return 0;
		snprintf(path, sizeof path, "/proc/self/ns/%s", ns);
		if (stat(path, &ours) != 0 || (theirs.st_ino == ours.st_ino && theirs.st_dev == ours.st_dev))
			return 0;
	}
	return 1;
}

/* `fds`, `count` of them, handed back to the caller over fd 3. 0, or 1
 * said. */
static int hand_back(const int *fds, unsigned count, const char *what)
{
	char            byte = 0;
	struct iovec    v = { &byte, 1u };
	char            control[CMSG_SPACE(3u * sizeof(int))];
	struct msghdr   message = { .msg_iov = &v, .msg_iovlen = 1u, .msg_control = control,
	                            .msg_controllen = CMSG_SPACE(count * sizeof(int)) };
	struct cmsghdr *c;

	memset(control, 0, sizeof control);
	c = CMSG_FIRSTHDR(&message);
	c->cmsg_level = SOL_SOCKET;
	c->cmsg_type  = SCM_RIGHTS;
	c->cmsg_len   = CMSG_LEN(count * sizeof(int));
	memcpy(CMSG_DATA(c), fds, count * sizeof(int));
	if (sendmsg(3, &message, 0) != 1)
		return fail(what, strerror(errno));
	return 0;
}

/* hull0's DNS relay's two sockets, bound on its gateway's port 53, which
 * only root may bind, into `fds`, where the caller gave a socket on fd 3
 * and the host's resolvers are all its loopback, which a machine on the
 * bridge could not reach: the relay then the caller's core's, run as the
 * user (engine/hostnet.c). How many, 0 or 2, or -1 said. */
static int relay_sockets(int fds[2])
{
	struct stat given;
	char       *resolvers = hull_hostnet_resolvers();

	if (resolvers != NULL || fstat(3, &given) != 0 || !S_ISSOCK(given.st_mode)) {
		free(resolvers);
		return 0;
	}
	if (hull_hostnet_relay_sockets(&fds[0], &fds[1]) != 0) {
		fail("hull0's DNS relay's port 53", strerror(errno));
		return -1;
	}
	return 2;
}

/* The bridge the plan's machine joins, into `name`: hull0, made, where it
 * names none, `*own` then 1; else the host bridge (bridge NAME) names,
 * which must be one already. 0, or 1 said. */
static int plan_bridge(const uint8_t *plan, size_t length, size_t machine, char *name,
                       size_t cap, int *own)
{
	char        network[16];
	char        why[512];
	char        sys[64 + IF_NAMESIZE];
	struct stat is;

	if (!value_of(plan, length, machine, "network", network, sizeof network) ||
	    strcmp(network, "bridge") != 0)
		return fail("the plan's machine is not (network bridge)", NULL);
	*own = 0;
	if (!value_of(plan, length, machine, "bridge", name, cap)) {
		*own = 1;
		snprintf(name, cap, "%s", HULL_BRIDGE);
		if (hull_hostnet_bridge(why, sizeof why) != NULL)
			return fail("hull0", why);
	} else {
		snprintf(sys, sizeof sys, "/sys/class/net/%s/bridge", name);
		if (strchr(name, '/') != NULL || if_nametoindex(name) == 0 || stat(sys, &is) != 0)
			return fail("(bridge NAME) names no bridge on the host", name);
	}
	if (hull_hostnet_ip() == NULL)
		return fail("ip is not installed", NULL);
	return 0;
}

/* The relay's sockets handed back, where they are asked for. 0, or 1 said. */
static int relay_back(void)
{
	int fds[2];
	int count = relay_sockets(fds);

	if (count <= 0)
		return count < 0;
	count = hand_back(fds, 2u, "hull0's DNS relay's sockets could not be handed back");
	close(fds[0]);
	close(fds[1]);
	return count;
}

/* The bridge op's root part: hull0 made where the plan names no bridge of
 * its own, then a veth whose far end is the machine's eth0, in PID's
 * namespace, and whose near end is on the bridge, as a root core's
 * chroot engine does it (engine/chroot.c's start_bridge). 0, or 1 said. */
static int bridge(const uint8_t *plan, size_t length, size_t machine, long pid)
{
	char        name[IF_NAMESIZE];
	char        near[24];
	char        pid_text[24];
	char        why[512];
	const char *ip = hull_hostnet_ip();
	int         own;

	if (plan_bridge(plan, length, machine, name, sizeof name, &own) != 0)
		return 1;
	snprintf(near, sizeof near, "hb%ld", pid);
	snprintf(pid_text, sizeof pid_text, "%ld", pid);
	{
		const char *pair[] = { ip, "link", "add", near, "type", "veth", "peer", "name", "eth0",
		                       "netns", pid_text, NULL };
		const char *on[] = { ip, "link", "set", near, "master", name, "up", NULL };

		if (hull_hostnet_run(pair, why, sizeof why) != 0 ||
		    hull_hostnet_run(on, why, sizeof why) != 0)
			return fail("the machine's veth", why);
	}
	return own ? relay_back() : 0;
}

/* The tap op's root part: a tap of the kernel's naming, hullt0, hullt1,
 * ..., on the bridge the plan names, up, and handed back on fd 3, with
 * the relay's sockets after it where they are asked for. Not persistent:
 * it is the descriptor's, so it goes from the bridge and the host when
 * the vm's qemu, holding the last of it, ends, and root removes nothing
 * afterwards, where the persistent tap a core run as root makes is
 * removed with the machine. 0, or 1 said. */
static int tap(const uint8_t *plan, size_t length, size_t machine)
{
	char         name[IF_NAMESIZE];
	char         why[512];
	struct ifreq request;
	int          own;
	int          fds[3];
	int          count = 1;
	int          failed;
	struct stat  given;

	if (fstat(3, &given) != 0 || !S_ISSOCK(given.st_mode))
		return fail("the tap op hands its tap back on fd 3, and there is no socket there",
		            NULL);
	if (plan_bridge(plan, length, machine, name, sizeof name, &own) != 0)
		return 1;
	if ((fds[0] = open("/dev/net/tun", O_RDWR | O_CLOEXEC)) < 0)
		return fail("/dev/net/tun", strerror(errno));
	memset(&request, 0, sizeof request);
	request.ifr_flags = IFF_TAP | IFF_NO_PI;
	snprintf(request.ifr_name, sizeof request.ifr_name, "hullt%%d");
	if (ioctl(fds[0], TUNSETIFF, &request) != 0) {
		close(fds[0]);
		return fail("the machine's tap", strerror(errno));
	}
	{
		const char *on[] = { hull_hostnet_ip(), "link", "set", request.ifr_name, "master", name,
		                     "up", NULL };

		if (hull_hostnet_run(on, why, sizeof why) != 0) {
			close(fds[0]);
			return fail("the machine's tap", why);
		}
	}
	if (own) {
		int relay = relay_sockets(fds + 1);

		if (relay < 0) {
			close(fds[0]);
			return 1;
		}
		count += relay;
	}
	failed = hand_back(fds, (unsigned)count, "the machine's tap could not be handed back");
	for (int i = 0; i < count; i++)
		close(fds[i]);
	return failed;
}

/* --- devices -------------------------------------------------------------- */

/* The device op's root part: under DEVICES_PATH, a tmpfs root mode
 * mounts once, the user's own directory UID, and in it NAME, made anew;
 * in that, for each path of the plan's (devices (root ...)), a node of
 * the same kind and number as the host's, named "c" or "b", its major,
 * ":" and its minor, as the engine names it, owned by the user, 0600.
 * Every step is by descriptor and never follows a link, and DEVICES_PATH
 * is root's, so a step the user could turn lands at most in another
 * directory of the user's on the same tmpfs; NAME is the user's once its
 * nodes are in it, for the engine to remove. 0, or 1 said. */
static int devices(const uint8_t *plan, size_t length, size_t machine, const char *name,
                   uid_t uid, gid_t gid)
{
	size_t      start;
	size_t      end;
	size_t      value;
	size_t      at;
	struct stat st;
	struct stat run;
	char        text[32];
	int         base;
	int         mine;
	int         dir;
	int         made = 0;

	if (!entry_of(plan, length, machine, "devices", &start, &end, &value) ||
	    !entry_of(plan, length, start, "root", &start, &end, &value))
		return fail("the plan's machine names no (devices (root ...))", NULL);
	if (mkdir(DEVICES_PATH, 0755) != 0 && errno != EEXIST)
		return fail(DEVICES_PATH, strerror(errno));
	if ((base = open(DEVICES_PATH, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC)) < 0)
		return fail(DEVICES_PATH, strerror(errno));
	/* One mount, whoever comes first: two at once would hide the first's
	 * nodes under the second's tmpfs. */
	if (flock(base, LOCK_EX) != 0 || fstat(base, &st) != 0 || st.st_uid != 0 ||
	    (st.st_mode & 022) != 0 || stat("/run", &run) != 0) {
		close(base);
		return fail(DEVICES_PATH, "it is not root's alone");
	}
	if (st.st_dev == run.st_dev) {
		close(base);
		if (mount("hull-root", DEVICES_PATH, "tmpfs", MS_NOSUID | MS_NOEXEC,
		          "mode=0755,size=1m") != 0)
			return fail("a tmpfs for root mode's device nodes cannot be mounted",
			            strerror(errno));
		if ((base = open(DEVICES_PATH, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC)) < 0)
			return fail(DEVICES_PATH, strerror(errno));
	}
	snprintf(text, sizeof text, "%u", (unsigned)uid);
	if ((mkdirat(base, text, 0700) != 0 && errno != EEXIST) ||
	    (mine = openat(base, text, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC)) < 0) {
		close(base);
		return fail("the user's directory of device nodes cannot be made", strerror(errno));
	}
	close(base);
	/* The user's, so that the engine, as the user, removes NAME once its
	 * machine has gone; DEVICES_PATH above it is root's, so the user can
	 * neither move it nor point it elsewhere, and the tmpfs holds nothing
	 * else of anybody's. */
	if (fstat(mine, &st) != 0 || !S_ISDIR(st.st_mode) ||
	    (st.st_uid == 0 && uid != 0 && fchown(mine, uid, gid) != 0) ||
	    fstat(mine, &st) != 0 || st.st_uid != uid) {
		close(mine);
		return fail("the user's directory of device nodes is not the user's", NULL);
	}
	if (mkdirat(mine, name, 0700) != 0 ||
	    (dir = openat(mine, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC)) < 0) {
		int error = errno;

		close(mine);
		return fail(name, strerror(error));
	}
	/* (root (node "PATH" ...) (serial ...) (parallel ...)). */
	at = element_end(plan, length, start + 1u, 0u);
	while (at != 0u && at < end - 1u) {
		size_t         next = element_end(plan, length, at, 0u);
		const uint8_t *bytes;
		size_t         n;
		size_t         item;

		if (next == 0u || plan[at] != '(' || !atom_at(plan, length, at + 1u, &bytes, &n) ||
		    !(atom_is(bytes, n, "node") || atom_is(bytes, n, "serial") ||
		      atom_is(bytes, n, "parallel")))
			break;
		for (item = element_end(plan, length, at + 1u, 0u); item != 0u && item < next - 1u;
		     item = element_end(plan, length, item, 0u)) {
			char path[PATH_MAX];
			char real[PATH_MAX];
			char node[32];

			if (!atom_at(plan, length, item, &bytes, &n) || n >= sizeof path ||
			    memchr(bytes, '\0', n) != NULL)
				break;
			memcpy(path, bytes, n);
			path[n] = '\0';
			if (realpath(path, real) == NULL || strncmp(real, "/dev/", 5u) != 0 ||
			    stat(real, &st) != 0 || !(S_ISCHR(st.st_mode) || S_ISBLK(st.st_mode))) {
				close(dir);
				close(mine);
				return fail("not a device under /dev", path);
			}
			snprintf(node, sizeof node, "%c%u:%u", S_ISCHR(st.st_mode) ? 'c' : 'b',
			         major(st.st_rdev), minor(st.st_rdev));
			if ((mknodat(dir, node, (st.st_mode & S_IFMT) | 0600, st.st_rdev) != 0 &&
			     errno != EEXIST) ||
			    fchownat(dir, node, uid, gid, AT_SYMLINK_NOFOLLOW) != 0) {
				int error = errno;

				close(dir);
				close(mine);
				return fail(path, strerror(error));
			}
			made++;
		}
		at = next;
	}
	if (made == 0 || fchown(dir, uid, gid) != 0) {
		close(dir);
		close(mine);
		return fail(made == 0 ? "the plan's (devices (root ...)) names no device" : name,
		            made == 0 ? NULL : strerror(errno));
	}
	close(dir);
	close(mine);
	return 0;
}

int main(int argc, char **argv)
{
	int             setuid_run = getuid() != geteuid() || getgid() != getegid();
	uid_t           uid = getuid();
	gid_t           gid = getgid();
	const char     *conf_path = CONF_PATH;
	const char     *log_path = LOG_PATH;
	const char     *why;
	struct passwd  *who = getpwuid(uid);
	const char     *op = argc > 1 ? argv[1] : "";
	int             bridging = strcmp(op, "bridge") == 0;
	int             tapping = strcmp(op, "tap") == 0;
	int             deviced = strcmp(op, "device") == 0;
	int             planned = bridging || tapping || deviced;  /* an op that acts, on its plan */
	char           *bridge_field[] = { (char *)"network.bridge" };
	char           *device_field[] = { (char *)"devices.root" };
	char *const    *fields = argv + 3;
	int             field_count = argc - 3;
	static uint8_t  plan[MAX_PLAN + 1];
	size_t          plan_length = 0u;
	size_t          machine = 0u;
	long            pid = 0;
	char            user[64];
	char            line[4096];
	char            when[32];
	time_t          now = time(NULL);
	struct tm       utc;
	int             conf;
	int             log;
	int             verdict;
	int             at;

	if (argc < 3 || (strcmp(op, "start") != 0 && strcmp(op, "exec") != 0 &&
	                 strcmp(op, "stop") != 0 && !planned) ||
	    ((strcmp(op, "exec") == 0 || strcmp(op, "stop") == 0 || tapping) && argc != 3) ||
	    ((bridging || deviced) && argc != 4) || argc > 3 + MAX_FIELDS)
		return fail("usage", "hull-root start HASH FIELD... | exec HASH | stop HASH | "
		            "bridge HASH PID < PLAN | tap HASH < PLAN | device HASH NAME < PLAN");
	if (!is_hash(argv[2]))
		return fail("not a plan's hash", argv[2]);
	if (bridging) {
		char *end;

		pid = strtol(argv[3], &end, 10);
		if (*end != '\0' || pid <= 1 || !made_of(argv[3], "0123456789", 10u))
			return fail("not a process", argv[3]);
		fields      = bridge_field;
		field_count = 1;
	} else if (tapping) {
		fields      = bridge_field;
		field_count = 1;
	} else if (deviced) {
		if (!made_of(argv[3], "abcdefghijklmnopqrstuvwxyz0123456789-", 64u) ||
		    argv[3][0] == '-')
			return fail("not a name for a machine's devices", argv[3]);
		fields      = device_field;
		field_count = 1;
	} else {
		for (int i = 3; i < argc; i++)
			if (!is_field(argv[i]))
				return fail("not a field", argv[i]);
	}
	if (who == NULL || !made_of(who->pw_name, "abcdefghijklmnopqrstuvwxyz0123456789._-", 32u))
		return fail("the calling user has no name root mode can list", NULL);
	snprintf(user, sizeof user, "%s", who->pw_name);
	if (!setuid_run) {
		if (getenv("HULL_ROOT_CONF") != NULL)
			conf_path = getenv("HULL_ROOT_CONF");
		if (getenv("HULL_ROOT_LOG") != NULL)
			log_path = getenv("HULL_ROOT_LOG");
	}

	/* An op that acts takes its plan whole, and acts only where the plan's
	 * own hash is the one asked for, worked out here from its bytes. */
	if (planned) {
		char    hex[72];
		ssize_t got = 0;

		while (plan_length < MAX_PLAN &&
		       (got = read(0, plan + plan_length, MAX_PLAN - plan_length)) > 0)
			plan_length += (size_t)got;
		if (got < 0 || plan_length == MAX_PLAN)
			return fail("the plan on stdin cannot be read whole", NULL);
		if (plan_hash(plan, plan_length, hex, &machine) != 0)
			return fail("the plan on stdin is not a plan", NULL);
		if (strcmp(hex, argv[2]) != 0)
			return fail("the plan on stdin is not the plan asked for", hex);
		if (bridging && !machine_process(pid, uid))
			return fail("not a machine of this user's, in namespaces of its own", argv[3]);
	}

	conf = open(conf_path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
	if (conf < 0)
		return fail(conf_path, strerror(errno));
	if (setuid_run && (why = roots_alone(conf, conf_path)) != NULL)
		return fail(conf_path, why);
	verdict = listed(conf, user, argv[2], fields, field_count);
	close(conf);
	if (verdict == 1)
		return fail(conf_path, "it cannot be read whole");

	/* The audit line, the verdict in it, before anything is granted. */
	gmtime_r(&now, &utc);
	strftime(when, sizeof when, "%Y-%m-%dT%H:%M:%SZ", &utc);
	at = snprintf(line, sizeof line, "%s %s uid=%u pid=%u %s %s %s", when, user,
	              (unsigned)uid, (unsigned)getppid(), op, argv[2],
	              verdict == 0 ? "granted" : "refused");
	for (int i = 0; i < field_count && at > 0 && (size_t)at < sizeof line; i++)
		at += snprintf(line + at, sizeof line - (size_t)at, " %s", fields[i]);
	if (bridging && at > 0 && (size_t)at < sizeof line)
		at += snprintf(line + at, sizeof line - (size_t)at, " machine=%ld", pid);
	if (deviced && at > 0 && (size_t)at < sizeof line)
		at += snprintf(line + at, sizeof line - (size_t)at, " name=%s", argv[3]);
	if (at <= 0 || (size_t)at >= sizeof line - 1u)
		return fail("the audit line is too long", NULL);
	line[at++] = '\n';
	log = open(log_path, O_WRONLY | O_APPEND | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600);
	if (log < 0)
		return fail("the audit log cannot be written, so nothing is granted", strerror(errno));
	if (setuid_run && (why = roots_alone(log, log_path)) != NULL) {
		close(log);
		return fail(log_path, why);
	}
	if (write(log, line, (size_t)at) != at || fsync(log) != 0) {
		close(log);
		return fail("the audit log cannot be written, so nothing is granted", strerror(errno));
	}
	close(log);

	if (verdict == 77) {
		fprintf(stderr, "hull-root: %s may not %s %s in root mode; the host's owner lists it "
		        "in %s with:\n%s %s", user, op, argv[2], conf_path, user, argv[2]);
		for (int i = 0; i < field_count; i++)
			fprintf(stderr, " %s", fields[i]);
		fputc('\n', stderr);
		return 77;
	}
	if (!planned)
		return 0;
	/* Root's part, wholly root's: its own ids and no environment of the
	 * caller's for the tools it runs. */
	if (setuid_run && (setresgid(0, 0, 0) != 0 || setresuid(0, 0, 0) != 0 || clearenv() != 0 ||
	                   setenv("PATH", "/usr/sbin:/usr/bin:/sbin:/bin", 1) != 0))
		return fail("root's own ids and environment cannot be had", strerror(errno));
	if (deviced)
		return devices(plan, plan_length, machine, argv[3], uid, gid);
	return bridging ? bridge(plan, plan_length, machine, pid) : tap(plan, plan_length, machine);
}
