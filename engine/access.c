/*
 * access.c -- reading a plan, for engines.
 */

#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "access.h"

#define SECRET_MAX 65536u  /* a secret's value, at most: the frontend's limit */

int hull_access_find(const hull_sexpr_doc *doc, const hull_sexpr_form *list, const char *key,
                     hull_sexpr_form *entry)
{
	hull_sexpr_form item;
	hull_sexpr_form name;
	int             more;

	if (!hull_sexpr_child(doc, list, &item))
		return 0;
	for (more = hull_sexpr_next(doc, &item, &item); more;
	     more = hull_sexpr_next(doc, &item, &item)) {
		if (item.kind == HULL_SEXPR_LIST && hull_sexpr_child(doc, &item, &name) &&
		    hull_sexpr_is(doc, &name, key)) {
			*entry = item;
			return 1;
		}
	}
	return 0;
}

char *hull_access_text(const hull_sexpr_doc *doc, const hull_sexpr_form *form)
{
	char *text;

	if (form->kind != HULL_SEXPR_ATOM ||
	    memchr(doc->data + form->content, '\0', form->content_length) != NULL)
		return NULL;
	text = malloc((size_t)form->content_length + 1u);
	if (text == NULL)
		return NULL;
	memcpy(text, doc->data + form->content, form->content_length);
	text[form->content_length] = '\0';
	return text;
}

char *hull_access_value(const hull_sexpr_doc *doc, const hull_sexpr_form *record,
                        const char *key)
{
	hull_sexpr_form entry;
	hull_sexpr_form head;
	hull_sexpr_form value;

	if (!hull_access_find(doc, record, key, &entry) || !hull_sexpr_child(doc, &entry, &head) ||
	    !hull_sexpr_next(doc, &head, &value))
		return NULL;
	return hull_access_text(doc, &value);
}

int hull_access_true(const hull_sexpr_doc *doc, const hull_sexpr_form *record, const char *key)
{
	char *value = hull_access_value(doc, record, key);
	int   yes   = value != NULL && strcmp(value, "true") == 0;

	free(value);
	return yes;
}

int hull_access_terminal(const hull_sexpr_doc *doc, const hull_sexpr_form *exec, int *cols,
                         int *rows)
{
	hull_sexpr_form tty;
	char           *across;
	char           *down;
	int             yes;

	if (!hull_access_find(doc, exec, "tty", &tty))
		return 0;
	across = hull_access_value(doc, &tty, "cols");
	down   = hull_access_value(doc, &tty, "rows");
	yes    = across != NULL && down != NULL;
	if (yes) {
		*cols = atoi(across);
		*rows = atoi(down);
	}
	free(across);
	free(down);
	return yes;
}

char *hull_access_path(const hull_sexpr_doc *doc, const char *path)
{
	hull_sexpr_form top;
	char           *base;
	char           *joined;

	if (path[0] == '/' || !hull_sexpr_first(doc, &top) ||
	    (base = hull_access_value(doc, &top, "base")) == NULL)
		return strdup(path);
	joined = malloc(strlen(base) + strlen(path) + 2u);
	if (joined != NULL) {
		strcpy(joined, base);
		strcat(joined, "/");
		strcat(joined, path);
	}
	free(base);
	return joined;
}

int hull_access_exposed(const hull_sexpr_doc *doc, const hull_sexpr_form *machine,
                        void (*each)(void *context, const char *pair), void *context)
{
	hull_sexpr_form top;
	hull_sexpr_form secrets;
	hull_sexpr_form item;
	hull_sexpr_form name_form;
	char           *dir;
	int             more;
	int             result = 0;

	if (!hull_access_find(doc, machine, "secrets", &secrets))
		return 0;
	hull_sexpr_first(doc, &top);
	dir = hull_access_value(doc, &top, "secrets");
	if (dir == NULL)
		return -1;
	hull_sexpr_child(doc, &secrets, &item);
	for (more = hull_sexpr_next(doc, &item, &item); more && result == 0;
	     more = hull_sexpr_next(doc, &item, &item)) {
		char   *expose = hull_access_value(doc, &item, "expose");
		char   *name;
		char   *path;
		char   *pair;
		size_t  prefix;
		size_t  cap;
		size_t  n;
		ssize_t got = 0;
		int     fd;

		if (expose == NULL || strcmp(expose, "env") != 0) {
			free(expose);
			continue;
		}
		free(expose);
		name = hull_sexpr_child(doc, &item, &name_form) ? hull_access_text(doc, &name_form) : NULL;
		if (name == NULL) {
			result = -1;
			break;
		}
		/* NAME=, then the value, at most the 64 KiB the frontend sends:
		 * a byte more is room to see that it was more. */
		prefix = strlen(name) + 1u;
		cap    = prefix + SECRET_MAX + 1u;
		path   = malloc(strlen(dir) + strlen(name) + 2u);
		pair   = malloc(cap + 1u);
		fd     = -1;
		if (path != NULL && pair != NULL) {
			strcpy(path, dir);
			strcat(path, "/");
			strcat(path, name);
			strcpy(pair, name);
			strcat(pair, "=");
			fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
		}
		n = prefix;
		while (fd >= 0 && n < cap && (got = read(fd, pair + n, cap - n)) > 0)
			n += (size_t)got;
		if (fd < 0 || got < 0 || n == cap || memchr(pair, '\0', n) != NULL) {
			result = -1;
		} else {
			pair[n] = '\0';
			each(context, pair);
		}
		if (fd >= 0)
			close(fd);
		free(path);
		free(pair);
		free(name);
	}
	free(dir);
	return result;
}

int hull_access_machine(hull_sexpr_doc *doc, const uint8_t *plan, size_t length,
                        hull_sexpr_form *machine)
{
	hull_sexpr_form top;

	return hull_sexpr_open(doc, plan, length, HULL_SEXPR_CANONICAL, NULL) == HULL_SEXPR_OK &&
	       hull_sexpr_first(doc, &top) && hull_access_find(doc, &top, "machine", machine);
}

int hull_access_execute(char *const argv[], char *const envp[])
{
	static const char fallback[] = "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin";
	const char       *search = fallback;
	const char       *file = argv[0];
	size_t            file_length = strlen(file);
	int               denied = 0;
	int               last = ENOENT;

	if (file[0] == '\0') {
		errno = ENOENT;
		return -1;
	}
	if (strchr(file, '/') != NULL)
		return execve(file, argv, envp);
	for (char *const *e = envp; e != NULL && *e != NULL; e++)
		if (strncmp(*e, "PATH=", 5u) == 0)
			search = *e + 5;
	for (const char *at = search;; ) {
		const char *end = strchr(at, ':');
		size_t      n = end != NULL ? (size_t)(end - at) : strlen(at);
		char        path[PATH_MAX];

		/* An empty part is the working directory, as POSIX has it. */
		if (n + 1u + file_length < sizeof path) {
			memcpy(path, n > 0u ? at : ".", n > 0u ? n : 1u);
			path[n > 0u ? n : 1u] = '/';
			memcpy(path + (n > 0u ? n : 1u) + 1u, file, file_length + 1u);
			execve(path, argv, envp);
			if (errno == EACCES)
				denied = 1;
			else if (errno != ENOENT && errno != ENOTDIR && errno != ELOOP &&
			         errno != ENAMETOOLONG)
				last = errno;
		}
		if (end == NULL)
			break;
		at = end + 1;
	}
	errno = denied ? EACCES : last;
	return -1;
}
