/*
 * hull_root.c -- hull-root, root mode's helper (project.md section 7).
 *
 *   hull-root start HASH FIELD...   may this user run the plan HASH with
 *                                   these root fields? logged either way
 *   hull-root exec HASH             an exec into such a machine, logged
 *   hull-root stop HASH             its stop, logged
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
 * Exits 0 granted, 77 not listed (with the line that would list it), 1
 * anything else.
 */

#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define CONF_PATH "/etc/hull/root.conf"
#define LOG_PATH  "/var/log/hull/root.log"
#define MAX_FIELDS 32
#define MAX_LISTING (1 << 20)

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

int main(int argc, char **argv)
{
	int             setuid_run = getuid() != geteuid() || getgid() != getegid();
	const char     *conf_path = CONF_PATH;
	const char     *log_path = LOG_PATH;
	const char     *why;
	struct passwd  *who = getpwuid(getuid());
	char            user[64];
	char            line[4096];
	char            when[32];
	time_t          now = time(NULL);
	struct tm       utc;
	int             conf;
	int             log;
	int             verdict;
	int             at;

	if (argc < 3 || (strcmp(argv[1], "start") != 0 && strcmp(argv[1], "exec") != 0 &&
	                 strcmp(argv[1], "stop") != 0) ||
	    (strcmp(argv[1], "start") != 0 && argc != 3) || argc > 3 + MAX_FIELDS)
		return fail("usage", "hull-root start HASH FIELD... | exec HASH | stop HASH");
	if (!is_hash(argv[2]))
		return fail("not a plan's hash", argv[2]);
	for (int i = 3; i < argc; i++)
		if (!is_field(argv[i]))
			return fail("not a field", argv[i]);
	if (who == NULL || !made_of(who->pw_name, "abcdefghijklmnopqrstuvwxyz0123456789._-", 32u))
		return fail("the calling user has no name root mode can list", NULL);
	snprintf(user, sizeof user, "%s", who->pw_name);
	if (!setuid_run) {
		if (getenv("HULL_ROOT_CONF") != NULL)
			conf_path = getenv("HULL_ROOT_CONF");
		if (getenv("HULL_ROOT_LOG") != NULL)
			log_path = getenv("HULL_ROOT_LOG");
	}

	conf = open(conf_path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
	if (conf < 0)
		return fail(conf_path, strerror(errno));
	if (setuid_run && (why = roots_alone(conf, conf_path)) != NULL)
		return fail(conf_path, why);
	verdict = listed(conf, user, argv[2], argv + 3, argc - 3);
	close(conf);
	if (verdict == 1)
		return fail(conf_path, "it cannot be read whole");

	/* The audit line, the verdict in it, before anything is granted. */
	gmtime_r(&now, &utc);
	strftime(when, sizeof when, "%Y-%m-%dT%H:%M:%SZ", &utc);
	at = snprintf(line, sizeof line, "%s %s uid=%u pid=%u %s %s %s", when, user,
	              (unsigned)getuid(), (unsigned)getppid(), argv[1], argv[2],
	              verdict == 0 ? "granted" : "refused");
	for (int i = 3; i < argc && at > 0 && (size_t)at < sizeof line; i++)
		at += snprintf(line + at, sizeof line - (size_t)at, " %s", argv[i]);
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
		        "in %s with:\n%s %s", user, argv[1], argv[2], conf_path, user, argv[2]);
		for (int i = 3; i < argc; i++)
			fprintf(stderr, " %s", argv[i]);
		fputc('\n', stderr);
		return 77;
	}
	return 0;
}
