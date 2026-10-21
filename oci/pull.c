/*
 * pull.c -- an image from an OCI registry into the store (section 3.5),
 * by the distribution spec's GETs: the manifest, by tag or digest; an
 * index's manifest for linux on the arch asked for; the config; and each
 * layer, applied in order into a tree under the store's tmp/, which is
 * then committed. See pull.h.
 *
 * Every body is hashed as it arrives and refused unless it is the digest
 * that named it. The registry's session, its token, the user's
 * credentials and its redirects, is remote.c's.
 */

#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include "https.h"
#include "json.h"
#include "pull.h"
#include "remote.h"
#include "sha256.h"
#include "store_private.h"

#define MAX_DOCUMENT  HULL_REMOTE_DOCUMENT
#define ACCEPT_MANIFESTS                                                     \
	"Accept: application/vnd.oci.image.index.v1+json, "                  \
	"application/vnd.docker.distribution.manifest.list.v2+json, "         \
	"application/vnd.oci.image.manifest.v1+json, "                        \
	"application/vnd.docker.distribution.manifest.v2+json"

static int say(char *why, size_t cap, const char *what, const char *detail)
{
	if (detail != NULL)
		snprintf(why, cap, "%s: %s", what, detail);
	else
		snprintf(why, cap, "%s", what);
	return -1;
}

/* A document by tag or digest: a manifest or an index into `doc`, its
 * digest into `digest`. */
static int fetch_manifest(hull_remote *s, const char *reference, hull_remote_sink *body,
                          hull_json *doc, hull_json_value *root, char digest[80], char *why,
                          size_t cap)
{
	char        url[2048];
	const char *bad;
	uint8_t     raw[32];
	char        hex[65];

	snprintf(url, sizeof url, "https://%s/v2/%s/manifests/%s", s->img.host, s->img.repository,
	         reference);
	if (hull_remote_fetch(s, url, ACCEPT_MANIFESTS, body, why, cap) != 0)
		return -1;
	hull_sha256_final(&body->hash, raw);
	hull_sha256_hex(raw, hex);
	snprintf(digest, 80, "sha256:%s", hex);
	/* By digest, the body must be it; by tag, it is what the registry
	 * says it sent, where it says. */
	if (hull_remote_is_digest(reference) && strcmp(reference, digest) != 0)
		return say(why, cap, reference, "the manifest is not the digest that names it");
	if (hull_json_open(doc, (const char *)body->data, body->length, root, &bad) != 0)
		return say(why, cap, reference, bad);
	return 0;
}

/* The index's manifest for linux on `arch`. */
static int choose(const hull_json *doc, const hull_json_value *root, const char *arch,
                  char digest[80], char *why, size_t cap)
{
	hull_json_value list;
	hull_json_value item;
	char            seen[512] = "";

	if (!hull_json_member(doc, root, "manifests", &list))
		return say(why, cap, "the index", "it lists no manifests");
	for (int more = hull_json_element(doc, &list, 1, &item); more;
	     more = hull_json_element(doc, &list, 0, &item)) {
		hull_json_value platform;
		char            os[64];
		char            architecture[64];

		if (!hull_json_member(doc, &item, "platform", &platform) ||
		    hull_json_member_string(doc, &platform, "os", os, sizeof os) < 0 ||
		    hull_json_member_string(doc, &platform, "architecture", architecture,
		                            sizeof architecture) < 0)
			continue;
		if (strcmp(os, "linux") == 0 && strcmp(architecture, arch) == 0)
			return hull_json_member_string(doc, &item, "digest", digest, 80) > 0 &&
			       hull_remote_is_digest(digest) ? 0 : say(why, cap, "the index", "its digest is not one");
		if (strlen(seen) + strlen(os) + strlen(architecture) + 4u < sizeof seen)
			snprintf(seen + strlen(seen), sizeof seen - strlen(seen), "%s%s/%s",
			         seen[0] != '\0' ? ", " : "", os, architecture);
	}
	return say(why, cap, arch, seen[0] != '\0' ? seen : "the index has no platform for it");
}

/* --- A.4 and A.5 --------------------------------------------------------- */

/* A canonical form, built up; `failed` once memory ran out. */
typedef struct canon {
	uint8_t *data;
	size_t   length;
	size_t   cap;
	int      failed;
} canon;

static void put_bytes(canon *c, const void *bytes, size_t n)
{
	if (c->failed)
		return;
	if (c->length + n > c->cap) {
		size_t   want = (c->length + n) * 2u + 256u;
		uint8_t *grown = realloc(c->data, want);

		if (grown == NULL) {
			c->failed = 1;
			return;
		}
		c->data = grown;
		c->cap = want;
	}
	memcpy(c->data + c->length, bytes, n);
	c->length += n;
}

static void put_raw(canon *c, const char *text)
{
	put_bytes(c, text, strlen(text));
}

static void put_atom(canon *c, const char *bytes, size_t n)
{
	char length[24];

	snprintf(length, sizeof length, "%zu:", n);
	put_raw(c, length);
	put_bytes(c, bytes, n);
}

static void put_text_atom(canon *c, const char *text)
{
	put_atom(c, text, strlen(text));
}

typedef struct named {
	char *name;
	char *value;
} named;

static int name_order(const void *a, const void *b)
{
	return strcmp(((const named *)a)->name, ((const named *)b)->name);
}

/* An array of strings, each an atom, as `(KEY ...)`; nothing for one that
 * is absent, null or empty. */
static int put_words(canon *c, const hull_json *doc, const hull_json_value *object,
                     const char *key, const char *open)
{
	hull_json_value list;
	hull_json_value item;
	char           *word = malloc(1u << 16);
	int             any = 0;

	if (word == NULL)
		return -1;
	if (hull_json_member(doc, object, key, &list) && list.kind == '[') {
		for (int more = hull_json_element(doc, &list, 1, &item); more;
		     more = hull_json_element(doc, &list, 0, &item)) {
			long n = hull_json_string(doc, &item, word, 1u << 16);

			if (n < 0) {
				free(word);
				return -1;
			}
			if (!any)
				put_raw(c, open);
			any = 1;
			put_atom(c, word, (size_t)n);
		}
	}
	if (any)
		put_raw(c, ")");
	free(word);
	return 0;
}

/* An object's names, sorted, each an atom, as `(KEY ...)`: OCI's
 * ExposedPorts and Volumes. */
static int put_names(canon *c, const hull_json *doc, const hull_json_value *object,
                     const char *key, const char *open)
{
	hull_json_value map;
	hull_json_value name;
	hull_json_value value;
	named          *items = NULL;
	size_t          count = 0u;
	int             result = 0;

	if (!hull_json_member(doc, object, key, &map) || map.kind != '{')
		return 0;
	for (int more = hull_json_next_member(doc, &map, 1, &name, &value); more;
	     more = hull_json_next_member(doc, &map, 0, &name, &value)) {
		char  text[4096];
		named *grown;

		if (hull_json_string(doc, &name, text, sizeof text) < 0 ||
		    (grown = realloc(items, (count + 1u) * sizeof *grown)) == NULL ||
		    (grown[count].name = strdup(text)) == NULL) {
			result = -1;
			break;
		}
		items = grown;
		items[count++].value = NULL;
	}
	if (result == 0 && count > 0u) {
		qsort(items, count, sizeof items[0], name_order);
		put_raw(c, open);
		for (size_t i = 0u; i < count; i++)
			if (i == 0u || strcmp(items[i].name, items[i - 1u].name) != 0)
				put_text_atom(c, items[i].name);
		put_raw(c, ")");
	}
	for (size_t i = 0u; i < count; i++)
		free(items[i].name);
	free(items);
	return result;
}

/* OCI's Env, `NAME=value` each, as `(3:env (NAME value) ...)`, sorted by
 * name, a name given twice its last value, as docker takes it. */
static int put_env(canon *c, const hull_json *doc, const hull_json_value *object)
{
	hull_json_value list;
	hull_json_value item;
	named          *items = NULL;
	size_t          count = 0u;
	int             result = 0;
	char           *text = malloc(1u << 16);

	if (text == NULL)
		return -1;
	if (hull_json_member(doc, object, "Env", &list) && list.kind == '[') {
		for (int more = hull_json_element(doc, &list, 1, &item); more;
		     more = hull_json_element(doc, &list, 0, &item)) {
			char  *equals;
			named *grown;

			if (hull_json_string(doc, &item, text, 1u << 16) < 0) {
				result = -1;
				break;
			}
			if ((equals = strchr(text, '=')) == NULL || equals == text)
				continue;
			*equals = '\0';
			for (size_t i = 0u; i < count; i++)
				if (strcmp(items[i].name, text) == 0) {
					free(items[i].name);
					free(items[i].value);
					items[i] = items[--count];
					break;
				}
			if ((grown = realloc(items, (count + 1u) * sizeof *grown)) == NULL) {
				result = -1;
				break;
			}
			items = grown;
			items[count].name = strdup(text);
			items[count].value = strdup(equals + 1);
			if (items[count].name == NULL || items[count].value == NULL) {
				free(items[count].name);
				free(items[count].value);
				result = -1;
				break;
			}
			count++;
		}
	}
	if (result == 0 && count > 0u) {
		qsort(items, count, sizeof items[0], name_order);
		put_raw(c, "(3:env");
		for (size_t i = 0u; i < count; i++) {
			put_raw(c, "(");
			put_text_atom(c, items[i].name);
			put_text_atom(c, items[i].value);
			put_raw(c, ")");
		}
		put_raw(c, ")");
	}
	for (size_t i = 0u; i < count; i++) {
		free(items[i].name);
		free(items[i].value);
	}
	free(items);
	free(text);
	return result;
}

/* A.4's runtime from the config's `config`, its keys in canonical order;
 * the workdir `/` where the image names none, the schema requiring one. */
static int runtime_of(const hull_json *doc, const hull_json_value *root, canon *c)
{
	hull_json_value config = { 0 };
	char            text[4096];
	int             has = hull_json_member(doc, root, "config", &config) && config.kind == '{';

	put_raw(c, "(7:runtime");
	if (has) {
		if (put_words(c, doc, &config, "Cmd", "(3:cmd") != 0 ||
		    put_words(c, doc, &config, "Entrypoint", "(10:entrypoint") != 0 ||
		    put_env(c, doc, &config) != 0 ||
		    put_names(c, doc, &config, "ExposedPorts", "(5:ports") != 0)
			return -1;
		if (hull_json_member_string(doc, &config, "StopSignal", text, sizeof text) > 0) {
			put_raw(c, "(11:stop_signal");
			put_text_atom(c, text);
			put_raw(c, ")");
		}
		if (hull_json_member_string(doc, &config, "User", text, sizeof text) > 0) {
			put_raw(c, "(4:user");
			put_text_atom(c, text);
			put_raw(c, ")");
		}
		if (put_names(c, doc, &config, "Volumes", "(7:volumes") != 0)
			return -1;
	}
	put_raw(c, "(7:workdir");
	if (has && hull_json_member_string(doc, &config, "WorkingDir", text, sizeof text) > 0)
		put_text_atom(c, text);
	else
		put_text_atom(c, "/");
	put_raw(c, "))");
	return c->failed ? -1 : 0;
}

/* --- the pull ----------------------------------------------------------- */

int hull_pull(hull_store *store, const char *name, const char *ref, const char *arch,
              const char *builder, hull_commit *out, char *why, size_t cap)
{
	canon             runtime = { 0 };
	canon             digests = { 0 };   /* A.5's, in the order fetched */
	canon             provenance = { 0 };
	hull_commit_extra extra;
	hull_remote       s = { 0 };
	uint8_t          *data = malloc(MAX_DOCUMENT);
	hull_remote_sink  body = { .data = data, .cap = MAX_DOCUMENT, .fd = -1 };
	hull_json         doc;
	hull_json_value   root;
	hull_json_value   layers;
	hull_json_value   layer;
	hull_json_value   config;
	char              digest[80];
	char              config_digest[80];
	char              media[256] = "";
	char              scratch[4400];
	char              tree[4500];
	char              file[4500];
	char              image_arch[64] = "";
	uint64_t          skipped = 0u;
	hull_unpack      *u = NULL;
	int               count = 0;
	int               index = 0;
	int               result = -1;
	int               layer_fd = -1;

	scratch[0] = '\0';
	if (data == NULL)
		return say(why, cap, "out of memory", NULL);
	if (hull_remote_parse(name, &s.img, why, cap) != 0)
		goto done;
	if (fetch_manifest(&s, s.img.reference, &body, &doc, &root, digest, why, cap) != 0)
		goto done;
	hull_json_member_string(&doc, &root, "mediaType", media, sizeof media);
	if (strstr(media, "index") != NULL || strstr(media, "manifest.list") != NULL ||
	    (media[0] == '\0' && hull_json_member(&doc, &root, "manifests", &layers))) {
		char chosen[80];

		put_text_atom(&digests, digest);
		if (choose(&doc, &root, arch, chosen, why, cap) != 0)
			goto done;
		body.length = 0u;
		if (fetch_manifest(&s, chosen, &body, &doc, &root, digest, why, cap) != 0)
			goto done;
		media[0] = '\0';
		hull_json_member_string(&doc, &root, "mediaType", media, sizeof media);
	}
	if (media[0] != '\0' && strcmp(media, "application/vnd.oci.image.manifest.v1+json") != 0 &&
	    strcmp(media, "application/vnd.docker.distribution.manifest.v2+json") != 0) {
		say(why, cap, name, "its manifest is of a kind hull does not read");
		goto done;
	}
	if (!hull_json_member(&doc, &root, "config", &config) ||
	    hull_json_member_string(&doc, &config, "digest", config_digest, sizeof config_digest) < 0 ||
	    !hull_remote_is_digest(config_digest) || !hull_json_member(&doc, &root, "layers", &layers)) {
		say(why, cap, name, "its manifest names no config, or no layers");
		goto done;
	}
	fprintf(stderr, "hull: pull: %s is %s\n", name, digest);
	put_text_atom(&digests, digest);
	put_text_atom(&digests, config_digest);
	/* The store's lock, shared, from the first write under tmp/ to the
	 * commit, so that gc, which empties tmp/ under it alone, waits. */
	flock(store->lock, LOCK_SH);
	snprintf(scratch, sizeof scratch, "%s/tmp/pull-XXXXXX", store->root);
	if (mkdtemp(scratch) == NULL) {
		say(why, cap, scratch, strerror(errno));
		scratch[0] = '\0';
		goto done;
	}
	snprintf(tree, sizeof tree, "%s/tree", scratch);
	snprintf(file, sizeof file, "%s/layer", scratch);
	if (mkdir(tree, 0700) != 0 || (layer_fd = open(file, O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC, 0600)) < 0) {
		say(why, cap, scratch, strerror(errno));
		goto done;
	}
	/* The config, for the arch the image says it is. */
	{
		char            url[2048];
		hull_json       config_doc;
		hull_json_value config_root;
		const char     *bad;
		uint8_t        *keep = malloc(MAX_DOCUMENT);
		hull_remote_sink config_body = { .data = keep, .cap = MAX_DOCUMENT, .fd = -1 };

		if (keep == NULL) {
			say(why, cap, "out of memory", NULL);
			goto done;
		}
		snprintf(url, sizeof url, "https://%s/v2/%s/blobs/%s", s.img.host, s.img.repository,
		         config_digest);
		if (hull_remote_fetch(&s, url, NULL, &config_body, why, cap) != 0 ||
		    hull_remote_verify(&config_body, config_digest, "the config", why, cap) != 0 ||
		    hull_json_open(&config_doc, (const char *)keep, config_body.length, &config_root,
		                   &bad) != 0 ||
		    hull_json_member_string(&config_doc, &config_root, "architecture", image_arch,
		                            sizeof image_arch) <= 0) {
			if (why[0] == '\0')
				say(why, cap, "the config", "it names no architecture");
			free(keep);
			goto done;
		}
		if (runtime_of(&config_doc, &config_root, &runtime) != 0) {
			say(why, cap, "the config", "its defaults could not be read");
			free(keep);
			goto done;
		}
		free(keep);
		if (strcmp(image_arch, arch) != 0) {
			char text[160];

			snprintf(text, sizeof text, "the image is for %s, not %s", image_arch, arch);
			say(why, cap, name, text);
			goto done;
		}
	}
	if ((u = hull_unpack_begin(tree, why, cap)) == NULL)
		goto done;
	for (int more = hull_json_element(&doc, &layers, 1, &layer); more;
	     more = hull_json_element(&doc, &layers, 0, &layer))
		count++;
	for (int more = hull_json_element(&doc, &layers, 1, &layer); more;
	     more = hull_json_element(&doc, &layers, 0, &layer)) {
		char            layer_digest[80];
		char            type[256] = "";
		char            url[2048];
		hull_json_value size;
		hull_json_value urls;
		long long       declared = -1;
		hull_remote_sink to_file = { .fd = layer_fd };

		index++;
		hull_json_member_string(&doc, &layer, "mediaType", type, sizeof type);
		if (hull_json_member_string(&doc, &layer, "digest", layer_digest, sizeof layer_digest) < 0 ||
		    !hull_remote_is_digest(layer_digest) || !hull_json_member(&doc, &layer, "size", &size) ||
		    hull_json_integer(&doc, &size, &declared) != 0 || declared < 0) {
			say(why, cap, name, "a layer with no digest or size");
			goto done;
		}
		if (strstr(type, "zstd") != NULL || strstr(type, "nondistributable") != NULL ||
		    strstr(type, "foreign") != NULL || hull_json_member(&doc, &layer, "urls", &urls) ||
		    (type[0] != '\0' && strstr(type, "tar") == NULL)) {
			say(why, cap, layer_digest, strstr(type, "zstd") != NULL ?
			    "a zstd layer, which hull does not yet read" :
			    "a layer of a kind hull does not fetch");
			goto done;
		}
		fprintf(stderr, "hull: pull: layer %d of %d, %s, %lld bytes\n", index, count, layer_digest,
		        declared);
		put_text_atom(&digests, layer_digest);
		snprintf(url, sizeof url, "https://%s/v2/%s/blobs/%s", s.img.host, s.img.repository,
		         layer_digest);
		if (hull_remote_fetch(&s, url, NULL, &to_file, why, cap) != 0 ||
		    hull_remote_verify(&to_file, layer_digest, layer_digest, why, cap) != 0)
			goto done;
		if (to_file.written != declared) {
			say(why, cap, layer_digest, "its length is not the size the manifest gives");
			goto done;
		}
		if (hull_unpack_layer(u, layer_fd, &skipped, why, cap) != 0)
			goto done;
	}
	{
		hull_unpack *ended = u;

		u = NULL;
		if (hull_unpack_end(ended, why, cap) != 0)
			goto done;
	}
	/* A.5: who made it, and where it came from, the registry named as
	 * the image's name names it. */
	put_raw(&provenance, "(10:provenance(7:builder");
	put_text_atom(&provenance, builder);
	put_raw(&provenance, ")(3:oci(7:digests");
	put_bytes(&provenance, digests.data, digests.length);
	put_raw(&provenance, ")(8:registry");
	{
		char from[1024];

		snprintf(from, sizeof from, "%s/%s",
		         strcmp(s.img.host, "registry-1.docker.io") == 0 ? "docker.io" : s.img.host,
		         s.img.repository);
		put_text_atom(&provenance, from);
	}
	put_raw(&provenance, ")))");
	if (digests.failed || provenance.failed || runtime.failed) {
		say(why, cap, "out of memory", NULL);
		goto done;
	}
	extra = (hull_commit_extra){ provenance.data, provenance.length, runtime.data, runtime.length };
	if (hull_store_commit(store, tree, image_arch, ref, &extra, out, why, cap) != 0)
		goto done;
	out->skipped += skipped;
	result = 0;
done:
	{
		char ignored[64];

		hull_unpack_end(u, ignored, sizeof ignored);
	}
	if (layer_fd >= 0)
		close(layer_fd);
	if (scratch[0] != '\0')
		hull_store_remove_tree(AT_FDCWD, scratch);
	flock(store->lock, LOCK_UN);
	free(data);
	free(runtime.data);
	free(digests.data);
	free(provenance.data);
	return result;
}
