/*
 * access.c -- reading a plan, for engines.
 */

#define _POSIX_C_SOURCE 200809L

#include <stdlib.h>
#include <string.h>

#include "access.h"

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

int hull_access_machine(hull_sexpr_doc *doc, const uint8_t *plan, size_t length,
                        hull_sexpr_form *machine)
{
	hull_sexpr_form top;

	return hull_sexpr_open(doc, plan, length, HULL_SEXPR_CANONICAL, NULL) == HULL_SEXPR_OK &&
	       hull_sexpr_first(doc, &top) && hull_access_find(doc, &top, "machine", machine);
}
