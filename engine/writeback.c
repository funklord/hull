/*
 * writeback.c -- a dumped tree made a directory's again (writeback.h).
 *
 * Every step goes by descriptor, openat and O_NOFOLLOW, so a symlink the
 * guest made is written as a symlink and never followed out of `dir`. A
 * file is written beside its old self and renamed over it, so a reader
 * sees the old bytes or the new and never half of each; a file whose
 * bytes and mode match is not touched at all, so a run that changed one
 * file rewrites one file.
 */
#define _GNU_SOURCE

#include "writeback.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

int hull_writeback_remove(int parent, const char *name)
{
	struct stat st;
	int         fd;
	DIR        *list;
	struct dirent *entry;
	int         failed = 0;

	if (fstatat(parent, name, &st, AT_SYMLINK_NOFOLLOW) != 0)
		return errno == ENOENT ? 0 : -1;
	if (!S_ISDIR(st.st_mode))
		return unlinkat(parent, name, 0);
	/* Its children may be removed only where it may be written. */
	if ((st.st_mode & 0700) != 0700)
		fchmodat(parent, name, (st.st_mode & 07777) | 0700, 0);
	if ((fd = openat(parent, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC)) < 0)
		return -1;
	if ((list = fdopendir(fd)) == NULL) {
		close(fd);
		return -1;
	}
	while ((entry = readdir(list)) != NULL)
		if (strcmp(entry->d_name, ".") != 0 && strcmp(entry->d_name, "..") != 0 &&
		    hull_writeback_remove(dirfd(list), entry->d_name) != 0)
			failed = 1;
	closedir(list);
	return failed ? -1 : unlinkat(parent, name, AT_REMOVEDIR);
}

/* Whether the regular files `a` and `b`, opened, hold the same bytes. */
static int same_bytes(int a, int b, off_t size)
{
	char  left[65536];
	char  right[65536];
	off_t at = 0;

	while (at < size) {
		size_t  want = size - at < (off_t)sizeof left ? (size_t)(size - at) : sizeof left;
		ssize_t x = pread(a, left, want, at);
		ssize_t y = pread(b, right, want, at);

		if (x <= 0 || x != y || memcmp(left, right, (size_t)x) != 0)
			return 0;
		at += x;
	}
	return 1;
}

/* The regular file `name` of `from` written into `to`, unless it is
 * there already, the same. */
static int copy_file(int from, int to, const char *name, const struct stat *st, char *why,
                     size_t cap)
{
	char            temporary[300];
	struct stat     old;
	struct timespec times[2] = { st->st_atim, st->st_mtim };
	char            buffer[65536];
	int             in;
	int             out;
	int             fd;
	ssize_t         n;

	if ((in = openat(from, name, O_RDONLY | O_NOFOLLOW | O_CLOEXEC)) < 0) {
		snprintf(why, cap, "%s: %s", name, strerror(errno));
		return -1;
	}
	if ((fd = openat(to, name, O_RDONLY | O_NOFOLLOW | O_CLOEXEC)) >= 0) {
		int same = fstat(fd, &old) == 0 && S_ISREG(old.st_mode) && old.st_size == st->st_size &&
		           (old.st_mode & 07777) == (st->st_mode & 07777) &&
		           same_bytes(in, fd, st->st_size);

		close(fd);
		if (same) {
			close(in);
			return 0;
		}
	}
	snprintf(temporary, sizeof temporary, ".hull-writeback-%ld", (long)getpid());
	unlinkat(to, temporary, 0);
	if ((out = openat(to, temporary, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC,
	                  0600)) < 0) {
		snprintf(why, cap, "%s: %s", name, strerror(errno));
		close(in);
		return -1;
	}
	while ((n = read(in, buffer, sizeof buffer)) > 0 || (n < 0 && errno == EINTR)) {
		for (ssize_t done = 0; n > 0 && done < n; ) {
			ssize_t wrote = write(out, buffer + done, (size_t)(n - done));

			if (wrote < 0 && errno == EINTR)
				continue;
			if (wrote <= 0) {
				n = -1;
				break;
			}
			done += wrote;
		}
		if (n < 0)
			break;
	}
	close(in);
	if (n < 0 || fchmod(out, st->st_mode & 07777) != 0 || futimens(out, times) != 0 ||
	    close(out) != 0) {
		snprintf(why, cap, "%s: %s", name, strerror(errno));
		unlinkat(to, temporary, 0);
		return -1;
	}
	/* A directory where the file goes is the guest's change too. */
	if (fstatat(to, name, &old, AT_SYMLINK_NOFOLLOW) == 0 && S_ISDIR(old.st_mode) &&
	    hull_writeback_remove(to, name) != 0) {
		snprintf(why, cap, "%s: %s", name, strerror(errno));
		unlinkat(to, temporary, 0);
		return -1;
	}
	if (renameat(to, temporary, to, name) != 0) {
		snprintf(why, cap, "%s: %s", name, strerror(errno));
		unlinkat(to, temporary, 0);
		return -1;
	}
	return 0;
}

static int copy_link(int from, int to, const char *name, char *why, size_t cap)
{
	char    target[4096];
	char    old[4096];
	ssize_t n = readlinkat(from, name, target, sizeof target - 1u);
	ssize_t m;

	if (n < 0) {
		snprintf(why, cap, "%s: %s", name, strerror(errno));
		return -1;
	}
	target[n] = '\0';
	m = readlinkat(to, name, old, sizeof old - 1u);
	if (m == n && memcmp(old, target, (size_t)n) == 0)
		return 0;
	if (hull_writeback_remove(to, name) != 0 || symlinkat(target, to, name) != 0) {
		snprintf(why, cap, "%s: %s", name, strerror(errno));
		return -1;
	}
	return 0;
}

/* `to` made to hold what `from` holds; `top` for the tree's own root. */
static int mirror(int from, int to, int top, char *why, size_t cap)
{
	DIR           *list;
	struct dirent *entry;
	int            fd;
	int            failed = 0;

	/* What the stage lacks, removed: a file, a directory or a symlink,
	 * which the dump would have carried; anything else kept. */
	if ((fd = dup(to)) < 0 || (list = fdopendir(fd)) == NULL) {
		if (fd >= 0)
			close(fd);
		snprintf(why, cap, "%s", strerror(errno));
		return -1;
	}
	while (!failed && (entry = readdir(list)) != NULL) {
		struct stat st;

		if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0 ||
		    fstatat(from, entry->d_name, &st, AT_SYMLINK_NOFOLLOW) == 0)
			continue;
		if (fstatat(to, entry->d_name, &st, AT_SYMLINK_NOFOLLOW) != 0 ||
		    !(S_ISREG(st.st_mode) || S_ISDIR(st.st_mode) || S_ISLNK(st.st_mode)))
			continue;
		if (hull_writeback_remove(to, entry->d_name) != 0) {
			snprintf(why, cap, "%s: %s", entry->d_name, strerror(errno));
			failed = 1;
		}
	}
	closedir(list);
	if (failed)
		return -1;
	/* What the stage holds, written. */
	if ((fd = dup(from)) < 0 || (list = fdopendir(fd)) == NULL) {
		if (fd >= 0)
			close(fd);
		snprintf(why, cap, "%s", strerror(errno));
		return -1;
	}
	while (!failed && (entry = readdir(list)) != NULL) {
		struct stat st;
		struct stat old;
		int         had;

		if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0 ||
		    fstatat(from, entry->d_name, &st, AT_SYMLINK_NOFOLLOW) != 0)
			continue;
		had = fstatat(to, entry->d_name, &old, AT_SYMLINK_NOFOLLOW) == 0;
		if (top && strcmp(entry->d_name, "lost+found") == 0 && !had)
			continue;
		if (S_ISREG(st.st_mode)) {
			failed = copy_file(from, to, entry->d_name, &st, why, cap) != 0;
		} else if (S_ISLNK(st.st_mode)) {
			failed = copy_link(from, to, entry->d_name, why, cap) != 0;
		} else if (S_ISDIR(st.st_mode)) {
			struct timespec times[2] = { st.st_atim, st.st_mtim };
			int             in;
			int             out;

			if (had && !S_ISDIR(old.st_mode) && hull_writeback_remove(to, entry->d_name) != 0)
				had = -1;
			if (had == -1 || ((had == 0 || !S_ISDIR(old.st_mode)) &&
			                  mkdirat(to, entry->d_name, 0700) != 0)) {
				snprintf(why, cap, "%s: %s", entry->d_name, strerror(errno));
				failed = 1;
				break;
			}
			/* Written into as the user, whatever its mode; given its
			 * own once its children are. */
			fchmodat(to, entry->d_name, 0700, 0);
			in = openat(from, entry->d_name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
			out = openat(to, entry->d_name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
			if (in < 0 || out < 0) {
				snprintf(why, cap, "%s: %s", entry->d_name, strerror(errno));
				failed = 1;
			} else if (mirror(in, out, 0, why, cap) != 0) {
				failed = 1;
			} else if (fchmod(out, st.st_mode & 07777) != 0 || futimens(out, times) != 0) {
				snprintf(why, cap, "%s: %s", entry->d_name, strerror(errno));
				failed = 1;
			}
			if (in >= 0)
				close(in);
			if (out >= 0)
				close(out);
		}
	}
	closedir(list);
	return failed ? -1 : 0;
}

int hull_writeback_mirror(const char *stage, const char *dir, char *why, size_t cap)
{
	int from = open(stage, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	int to = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	int status;

	if (from < 0 || to < 0) {
		snprintf(why, cap, "%s: %s", from < 0 ? stage : dir, strerror(errno));
		status = -1;
	} else {
		status = mirror(from, to, 1, why, cap);
	}
	if (from >= 0)
		close(from);
	if (to >= 0)
		close(to);
	return status;
}
