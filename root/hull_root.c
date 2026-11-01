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
 *                                   made where it is its own
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
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "hostnet.h"
#include "sha256.h"

#define CONF_PATH "/etc/hull/root.conf"
#define LOG_PATH  "/var/log/hull/root.log"
#define MAX_FIELDS 32
#define MAX_LISTING (1 << 20)

#define MAX_PLAN (1 << 20)

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
	char        sys[64 + IF_NAMESIZE];
	struct stat is;
	const char *ip = hull_hostnet_ip();
	char        network[16];

	if (!value_of(plan, length, machine, "network", network, sizeof network) ||
	    strcmp(network, "bridge") != 0)
		return fail("the plan's machine is not (network bridge)", NULL);
	if (!value_of(plan, length, machine, "bridge", name, sizeof name)) {
		snprintf(name, sizeof name, "%s", HULL_BRIDGE);
		if (hull_hostnet_bridge(why, sizeof why) != NULL)
			return fail("hull0", why);
	} else {
		snprintf(sys, sizeof sys, "/sys/class/net/%s/bridge", name);
		if (strchr(name, '/') != NULL || if_nametoindex(name) == 0 || stat(sys, &is) != 0)
			return fail("(bridge NAME) names no bridge on the host", name);
	}
	if (ip == NULL)
		return fail("ip is not installed", NULL);
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
	return 0;
}

int main(int argc, char **argv)
{
	int             setuid_run = getuid() != geteuid() || getgid() != getegid();
	uid_t           uid = getuid();
	const char     *conf_path = CONF_PATH;
	const char     *log_path = LOG_PATH;
	const char     *why;
	struct passwd  *who = getpwuid(uid);
	const char     *op = argc > 1 ? argv[1] : "";
	int             bridging = strcmp(op, "bridge") == 0;
	char           *bridge_field[] = { (char *)"network.bridge" };
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
	                 strcmp(op, "stop") != 0 && !bridging) ||
	    ((strcmp(op, "exec") == 0 || strcmp(op, "stop") == 0) && argc != 3) ||
	    (bridging && argc != 4) || argc > 3 + MAX_FIELDS)
		return fail("usage", "hull-root start HASH FIELD... | exec HASH | stop HASH | "
		            "bridge HASH PID < PLAN");
	if (!is_hash(argv[2]))
		return fail("not a plan's hash", argv[2]);
	if (bridging) {
		char *end;

		pid = strtol(argv[3], &end, 10);
		if (*end != '\0' || pid <= 1 || !made_of(argv[3], "0123456789", 10u))
			return fail("not a process", argv[3]);
		fields      = bridge_field;
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
	if (bridging) {
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
		if (!machine_process(pid, uid))
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
	if (!bridging)
		return 0;
	/* Root's part, wholly root's: its own ids and no environment of the
	 * caller's for the tools it runs. */
	if (setuid_run && (setresgid(0, 0, 0) != 0 || setresuid(0, 0, 0) != 0 || clearenv() != 0 ||
	                   setenv("PATH", "/usr/sbin:/usr/bin:/sbin:/bin", 1) != 0))
		return fail("root's own ids and environment cannot be had", strerror(errno));
	return bridge(plan, plan_length, machine, pid);
}
