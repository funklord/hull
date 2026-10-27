/*
 * access.c -- reading a plan, for engines.
 */

#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
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
	static const char fallback[] = "PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin";
	const char       *search = fallback + 5;
	const char       *file = argv[0];
	size_t            file_length = strlen(file);
	int               denied = 0;
	int               last = ENOENT;
	char             *with_path[1024];
	size_t            count = 0u;
	int               has_path = 0;

	if (file[0] == '\0') {
		errno = ENOENT;
		return -1;
	}
	for (char *const *e = envp; e != NULL && *e != NULL; e++, count++)
		if (strncmp(*e, "PATH=", 5u) == 0) {
			search = *e + 5;
			has_path = 1;
		}
	/* Where neither the spec nor the image names a PATH, the one searched
	 * is the workload's as well: a shell's own default is not exported,
	 * so its children had none, and gcc, which finds cc1 from where it
	 * was found, could not, nor `command -v` an sbin program. On the
	 * stack, since this runs in a child of a process with threads. */
	if (!has_path && count + 2u <= sizeof with_path / sizeof with_path[0]) {
		for (size_t i = 0u; i < count; i++)
			with_path[i] = envp[i];
		with_path[count]      = (char *)fallback;
		with_path[count + 1u] = NULL;
		envp = with_path;
	}
	if (strchr(file, '/') != NULL)
		return execve(file, argv, envp);
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

const char *hull_access_stack(const hull_sexpr_doc *doc, const hull_sexpr_form *volume,
                              int readonly, hull_stack *stack)
{
	hull_sexpr_form entry;
	hull_sexpr_form item;
	char           *upper;

	memset(stack, 0, sizeof *stack);
	if (!hull_access_find(doc, volume, "layers", &entry))
		return NULL;
	hull_sexpr_child(doc, &entry, &item);
	for (int more = hull_sexpr_next(doc, &item, &item); more;
	     more = hull_sexpr_next(doc, &item, &item)) {
		char *text = hull_access_text(doc, &item);

		if (stack->count == HULL_STACK_MAX) {
			free(text);
			return "a volume has too many layers";
		}
		if (text == NULL || strncmp(text, "dir:", 4u) != 0) {
			free(text);
			return "a volume's layer is a dir: source";
		}
		stack->layer[stack->count] = hull_access_path(doc, text + 4);
		free(text);
		if (stack->layer[stack->count++] == NULL)
			return "a volume's layer's path cannot be read";
	}
	stack->readonly = readonly;
	if ((upper = hull_access_value(doc, volume, "upper")) != NULL) {
		stack->upper = hull_access_path(doc, upper);
		free(upper);
		if (stack->upper == NULL || (stack->upper_work = hull_stack_work_of(stack->upper)) == NULL)
			return "a volume's upper's path cannot be read";
	}
	return NULL;
}

void hull_access_segment_mac(const char *address, const char *segment, unsigned index,
                             unsigned char mac[6])
{
	unsigned hash = 2166136261u;

	for (const char *p = address; *p != '\0'; p++)
		hash = (hash ^ (unsigned char)*p) * 16777619u;
	hash = (hash ^ '/') * 16777619u;
	for (const char *p = segment; *p != '\0'; p++)
		hash = (hash ^ (unsigned char)*p) * 16777619u;
	hash = (hash ^ (unsigned char)index) * 16777619u;
	mac[0] = 0x52u;
	mac[1] = 0x54u;
	mac[2] = 0x00u;
	mac[3] = (unsigned char)(hash >> 16);
	mac[4] = (unsigned char)(hash >> 8);
	mac[5] = (unsigned char)hash;
}

char *hull_access_segment_name(const hull_sexpr_doc *doc, const hull_sexpr_form *item)
{
	hull_sexpr_form name;

	if (item->kind != HULL_SEXPR_LIST)
		return hull_access_text(doc, item);
	return hull_sexpr_child(doc, item, &name) ? hull_access_text(doc, &name) : NULL;
}

int hull_access_named(const hull_sexpr_doc *doc, const hull_sexpr_form *names,
                      const char *segment, char address[16], unsigned *prefix)
{
	hull_sexpr_form record;
	char           *text;
	char           *bits;
	int             found;

	if (!hull_access_find(doc, names, segment, &record))
		return 0;
	text  = hull_access_value(doc, &record, "address");
	bits  = hull_access_value(doc, &record, "prefix");
	found = text != NULL && bits != NULL && strlen(text) < 16u;
	if (found) {
		strcpy(address, text);
		*prefix = (unsigned)strtoul(bits, NULL, 10);
	}
	free(text);
	free(bits);
	return found;
}

char *hull_access_hosts(const hull_sexpr_doc *doc, const hull_sexpr_form *names)
{
	static const char opening[] = "127.0.0.1\tlocalhost\n::1\tlocalhost ip6-localhost ip6-loopback\n";
	hull_sexpr_form   segment;
	size_t            cap = sizeof opening;
	size_t            at;
	char             *text;

	/* Its size first: a line is at most the address, two ids and ".hull". */
	if (!hull_sexpr_child(doc, names, &segment))
		return NULL;
	for (hull_sexpr_form s = segment; hull_sexpr_next(doc, &s, &s);) {
		hull_sexpr_form hosts;
		hull_sexpr_form host;

		if (hull_access_find(doc, &s, "hosts", &hosts) && hull_sexpr_child(doc, &hosts, &host))
			while (hull_sexpr_next(doc, &host, &host))
				cap += 2u * (size_t)host.extent + 16u;
	}
	if ((text = malloc(cap)) == NULL)
		return NULL;
	memcpy(text, opening, sizeof opening);
	at = sizeof opening - 1u;
	while (hull_sexpr_next(doc, &segment, &segment)) {
		hull_sexpr_form hosts;
		hull_sexpr_form host;

		if (!hull_access_find(doc, &segment, "hosts", &hosts) || !hull_sexpr_child(doc, &hosts, &host))
			continue;
		while (hull_sexpr_next(doc, &host, &host)) {
			hull_sexpr_form id_form;
			hull_sexpr_form address_form;
			char           *id = NULL;
			char           *address = NULL;
			int             n = -1;

			if (hull_sexpr_child(doc, &host, &id_form) &&
			    hull_sexpr_next(doc, &id_form, &address_form)) {
				id      = hull_access_text(doc, &id_form);
				address = hull_access_text(doc, &address_form);
			}
			if (id != NULL && address != NULL)
				n = snprintf(text + at, cap - at, "%s\t%s %s.hull\n", address, id, id);
			free(id);
			free(address);
			if (n < 0 || (size_t)n >= cap - at) {
				free(text);
				return NULL;
			}
			at += (size_t)n;
		}
	}
	return text;
}

char *hull_access_resolver(const hull_sexpr_doc *doc, const hull_sexpr_form *names)
{
	hull_sexpr_form segment;
	char           *text = malloc(3u * 32u + 16u);
	size_t          at = 0u;
	unsigned        servers = 0u;

	if (text == NULL || !hull_sexpr_child(doc, names, &segment)) {
		free(text);
		return NULL;
	}
	/* Three, as many as a resolver reads (resolv.conf(5)). */
	while (servers < 3u && hull_sexpr_next(doc, &segment, &segment)) {
		char        *address = hull_access_value(doc, &segment, "address");
		char        *bits = hull_access_value(doc, &segment, "prefix");
		unsigned     a, b, c, d;
		unsigned     prefix = bits != NULL ? (unsigned)strtoul(bits, NULL, 10) : 0u;
		unsigned     self;

		if (address != NULL && prefix >= 1u && prefix <= 30u &&
		    sscanf(address, "%u.%u.%u.%u", &a, &b, &c, &d) == 4) {
			self = ((a << 24 | b << 16 | c << 8 | d) & 0xffffffffu << (32u - prefix)) + 1u;
			at += (size_t)snprintf(text + at, 32u, "nameserver %u.%u.%u.%u\n", self >> 24,
			                       self >> 16 & 0xffu, self >> 8 & 0xffu, self & 0xffu);
			servers++;
		}
		free(address);
		free(bits);
	}
	if (servers == 0u) {
		free(text);
		return NULL;
	}
	memcpy(text + at, "search hull\n", sizeof "search hull\n");
	return text;
}
