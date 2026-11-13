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
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "https.h"
#include "json.h"
#include "pull.h"
#include "remote.h"
#include "push.h"
#include "sha256.h"

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
	if (c->failed || n == 0u)   /* nothing, onto what may be no buffer yet */
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
	hull_owner       *owners = NULL;
	size_t            owner_count = 0u;
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
	hull_https_allow_loopback();
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
	/* The layers' owners, which the tree on disk, all the user's, cannot
	 * keep, given to the commit over it. */
	hull_unpack_owners(u, &owners, &owner_count);
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
	extra = (hull_commit_extra){ provenance.data, provenance.length, runtime.data, runtime.length,
	                             owners, owner_count, 0 };
	if (hull_store_commit(store, tree, image_arch, ref, &extra, out, why, cap) != 0)
		goto done;
	out->skipped += skipped;
	result = 0;
done:
	hull_owners_free(owners, owner_count);
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

/* --- an archive on disk -------------------------------------------------- */

/* A member of the archive: where its bytes are. */
typedef struct member {
	char     name[256];
	uint64_t offset;
	uint64_t size;
} member;

typedef struct archive {
	int     fd;
	member *members;
	size_t  count;
} archive;

/* The octal number of a tar header's field. */
static uint64_t tar_number(const char *field, size_t n)
{
	uint64_t value = 0u;

	for (size_t i = 0u; i < n && field[i] >= '0' && field[i] <= '7'; i++)
		value = value * 8u + (uint64_t)(field[i] - '0');
	return value;
}

/* Every regular member of the tar at `fd`, into `a`. The names an image
 * archive holds are short, so ustar's two fields are enough; a longer one
 * is no name a manifest could use. 0, or -1. */
static int read_members(archive *a, char *why, size_t cap)
{
	uint8_t  header[512];
	uint64_t at = 0u;
	size_t   room = 0u;

	for (;;) {
		char     name[256];
		uint64_t size;
		size_t   n;

		ssize_t got = pread(a->fd, header, sizeof header, (off_t)at);

		if (got == 0 || (got == (ssize_t)sizeof header && header[0] == '\0'))
			return 0;
		if (got != (ssize_t)sizeof header || memcmp(header + 257, "ustar", 5u) != 0)
			return say(why, cap, "the archive", "it is not a tar; a compressed one wants "
			           "uncompressing first");
		size = tar_number((const char *)header + 124, 12u);
		name[0] = '\0';
		if (header[345] != '\0')
			snprintf(name, sizeof name, "%.155s/", (const char *)header + 345);
		n = strlen(name);
		snprintf(name + n, sizeof name - n, "%.100s", (const char *)header);
		if (header[156] == '0' || header[156] == '\0') {
			const char *plain = strncmp(name, "./", 2u) == 0 ? name + 2 : name;

			if (a->count == room) {
				member *grown;

				room = room != 0u ? room * 2u : 64u;
				if ((grown = realloc(a->members, room * sizeof *grown)) == NULL)
					return say(why, cap, "out of memory", NULL);
				a->members = grown;
			}
			snprintf(a->members[a->count].name, sizeof a->members[0].name, "%s", plain);
			a->members[a->count].offset = at + 512u;
			a->members[a->count].size   = size;
			a->count++;
		}
		at += 512u + (size + 511u) / 512u * 512u;
	}
}

static const member *member_called(const archive *a, const char *name)
{
	for (size_t i = 0u; i < a->count; i++)
		if (strcmp(a->members[i].name, name) == 0)
			return &a->members[i];
	return NULL;
}

/* `m`'s bytes into `out`, a document of at most MAX_DOCUMENT - 1, ended by
 * a NUL. Its length, or -1. */
static long member_text(const archive *a, const member *m, char *out)
{
	if (m->size >= MAX_DOCUMENT ||
	    pread(a->fd, out, (size_t)m->size, (off_t)m->offset) != (ssize_t)m->size)
		return -1;
	out[m->size] = '\0';
	return (long)m->size;
}

/* Whether the member `name` is a blob by its digest, blobs/sha256/HEX. */
static const char *blob_digest(const char *name)
{
	return strncmp(name, "blobs/sha256/", 13u) == 0 && strlen(name) == 13u + 64u ? name + 13 : NULL;
}

/* `m` copied to `to`, over what it held, its digest held to its name where
 * its name is one. 0, or -1. */
static int member_to(const archive *a, const member *m, int to, char *why, size_t cap)
{
	static uint8_t buffer[1u << 16];
	hull_sha256    hash;
	uint64_t       done = 0u;
	uint8_t        raw[32];
	char           hex[65];
	const char    *named = blob_digest(m->name);

	hull_sha256_init(&hash);
	if (ftruncate(to, 0) != 0 || lseek(to, 0, SEEK_SET) != 0)
		return say(why, cap, m->name, strerror(errno));
	while (done < m->size) {
		size_t  want = m->size - done < sizeof buffer ? (size_t)(m->size - done) : sizeof buffer;
		ssize_t n = pread(a->fd, buffer, want, (off_t)(m->offset + done));

		if (n <= 0)
			return say(why, cap, m->name, "the archive ends inside it");
		if (write(to, buffer, (size_t)n) != n)
			return say(why, cap, m->name, strerror(errno));
		hull_sha256_update(&hash, buffer, (size_t)n);
		done += (uint64_t)n;
	}
	hull_sha256_final(&hash, raw);
	hull_sha256_hex(raw, hex);
	if (named != NULL && strcmp(named, hex) != 0)
		return say(why, cap, m->name, "it is not the digest that names it");
	return 0;
}

/* A blob of an OCI layout, `sha256:HEX`, as a member's name in `out`. */
static int blob_name(const char *digest, char *out, size_t cap)
{
	if (!hull_remote_is_digest(digest))
		return -1;
	snprintf(out, cap, "blobs/sha256/%s", digest + 7);
	return 0;
}

/* The members of the image in `a`: its config's name, its layers' names
 * and a tag, by docker save's manifest.json or, with none, an OCI layout's
 * index.json, an index within narrowed to linux on `arch`. */
#define MAX_LAYERS 256u

typedef struct contents {
	char config[256];
	char layer[MAX_LAYERS][256];
	unsigned layers;
	char tag[256];
} contents;

static int read_contents(const archive *a, const char *arch, contents *c, char *text, char *why,
                         size_t cap)
{
	const member   *m = member_called(a, "manifest.json");
	hull_json       doc;
	hull_json_value root;
	hull_json_value list;
	hull_json_value item;
	const char     *bad;
	long            n;

	if (m != NULL) {
		hull_json_value image;

		if ((n = member_text(a, m, text)) < 0 ||
		    hull_json_open(&doc, text, (size_t)n, &root, &bad) != 0 ||
		    !hull_json_element(&doc, &root, 1, &image) ||
		    hull_json_member_string(&doc, &image, "Config", c->config, sizeof c->config) <= 0 ||
		    !hull_json_member(&doc, &image, "Layers", &list))
			return say(why, cap, "manifest.json", "it names no image");
		if (hull_json_member(&doc, &image, "RepoTags", &item)) {
			hull_json_value tag;

			if (hull_json_element(&doc, &item, 1, &tag))
				hull_json_string(&doc, &tag, c->tag, sizeof c->tag);
		}
		for (int more = hull_json_element(&doc, &list, 1, &item); more;
		     more = hull_json_element(&doc, &list, 0, &item)) {
			if (c->layers == MAX_LAYERS)
				return say(why, cap, "manifest.json", "it names too many layers");
			if (hull_json_string(&doc, &item, c->layer[c->layers], sizeof c->layer[0]) <= 0)
				return say(why, cap, "manifest.json", "a layer is not a name");
			c->layers++;
		}
		return 0;
	}
	if ((m = member_called(a, "index.json")) == NULL)
		return say(why, cap, "the archive", "it holds neither manifest.json nor index.json: no "
		           "image of docker save or an OCI layout");
	/* An index, perhaps of an index, down to a manifest. */
	for (int depth = 0; depth < 3; depth++) {
		char        digest[80] = "";
		char        name[256];
		char        media[256] = "";
		hull_json_value config;

		if ((n = member_text(a, m, text)) < 0 ||
		    hull_json_open(&doc, text, (size_t)n, &root, &bad) != 0)
			return say(why, cap, m->name, "it is no JSON this reads");
		hull_json_member_string(&doc, &root, "mediaType", media, sizeof media);
		if (hull_json_member(&doc, &root, "config", &config)) {
			if (hull_json_member_string(&doc, &config, "digest", digest, sizeof digest) <= 0 ||
			    blob_name(digest, c->config, sizeof c->config) != 0 ||
			    !hull_json_member(&doc, &root, "layers", &list))
				return say(why, cap, m->name, "it names no config, or no layers");
			for (int more = hull_json_element(&doc, &list, 1, &item); more;
			     more = hull_json_element(&doc, &list, 0, &item)) {
				if (c->layers == MAX_LAYERS)
					return say(why, cap, m->name, "it names too many layers");
				if (hull_json_member_string(&doc, &item, "digest", digest, sizeof digest) <= 0 ||
				    blob_name(digest, c->layer[c->layers], sizeof c->layer[0]) != 0)
					return say(why, cap, m->name, "a layer has no digest");
				c->layers++;
			}
			return 0;
		}
		if (!hull_json_member(&doc, &root, "manifests", &list) ||
		    !hull_json_element(&doc, &list, 1, &item))
			return say(why, cap, m->name, "it lists no manifests");
		/* One manifest is taken as it is, its platform often unsaid;
		 * several are chosen among. */
		config = item;
		if (hull_json_element(&doc, &list, 0, &config)) {
			if (choose(&doc, &root, arch, digest, why, cap) != 0)
				return -1;
		} else if (hull_json_member_string(&doc, &item, "digest", digest, sizeof digest) <= 0) {
			return say(why, cap, m->name, "its manifest has no digest");
		}
		if (blob_name(digest, name, sizeof name) != 0 || (m = member_called(a, name)) == NULL)
			return say(why, cap, digest, "the archive does not hold it");
	}
	return say(why, cap, "index.json", "its indexes go too deep");
}

int hull_load(hull_store *store, const char *path, const char *ref, const char *arch,
              const char *builder, hull_commit *out, char *ref_out, size_t ref_cap, char *why,
              size_t cap)
{
	canon             runtime = { 0 };
	canon             digests = { 0 };
	canon             provenance = { 0 };
	hull_commit_extra extra;
	hull_owner       *owners = NULL;
	size_t            owner_count = 0u;
	archive           a = { .fd = -1 };
	contents         *c = calloc(1u, sizeof *c);
	char             *text = malloc(MAX_DOCUMENT);
	hull_unpack      *u = NULL;
	const member     *m;
	hull_json         doc;
	hull_json_value   root;
	const char       *bad;
	char              scratch[4400] = "";
	char              tree[4500];
	char              file[4500];
	char              image_arch[64] = "";
	uint64_t          skipped = 0u;
	int               layer_fd = -1;
	int               result = -1;
	long              n;

	if (c == NULL || text == NULL) {
		say(why, cap, "out of memory", NULL);
		goto done;
	}
	if ((a.fd = open(path, O_RDONLY | O_CLOEXEC)) < 0) {
		say(why, cap, path, strerror(errno));
		goto done;
	}
	if (read_members(&a, why, cap) != 0 || read_contents(&a, arch, c, text, why, cap) != 0)
		goto done;
	if ((m = member_called(&a, c->config)) == NULL || (n = member_text(&a, m, text)) < 0 ||
	    hull_json_open(&doc, text, (size_t)n, &root, &bad) != 0 ||
	    hull_json_member_string(&doc, &root, "architecture", image_arch, sizeof image_arch) <= 0) {
		say(why, cap, c->config, "the image's config is not there, or names no architecture");
		goto done;
	}
	if (runtime_of(&doc, &root, &runtime) != 0) {
		say(why, cap, "the config", "its defaults could not be read");
		goto done;
	}
	/* Its digest, the image's name when it has no tag. */
	{
		hull_sha256 hash;
		uint8_t     raw[32];
		char        hex[65];
		char        digest[80];

		hull_sha256_init(&hash);
		hull_sha256_update(&hash, text, (size_t)n);
		hull_sha256_final(&hash, raw);
		hull_sha256_hex(raw, hex);
		if (blob_digest(c->config) != NULL && strcmp(blob_digest(c->config), hex) != 0) {
			say(why, cap, c->config, "it is not the digest that names it");
			goto done;
		}
		snprintf(digest, sizeof digest, "sha256:%s", hex);
		put_text_atom(&digests, digest);
		if (ref != NULL)
			snprintf(ref_out, ref_cap, "%s", ref);
		else if (c->tag[0] != '\0')
			snprintf(ref_out, ref_cap, "%s", c->tag);
		else
			snprintf(ref_out, ref_cap, "load/%.32s", hex);
	}
	flock(store->lock, LOCK_SH);
	snprintf(scratch, sizeof scratch, "%s/tmp/load-XXXXXX", store->root);
	if (mkdtemp(scratch) == NULL) {
		say(why, cap, scratch, strerror(errno));
		scratch[0] = '\0';
		goto done;
	}
	snprintf(tree, sizeof tree, "%s/tree", scratch);
	snprintf(file, sizeof file, "%s/layer", scratch);
	if (mkdir(tree, 0700) != 0 ||
	    (layer_fd = open(file, O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC, 0600)) < 0) {
		say(why, cap, scratch, strerror(errno));
		goto done;
	}
	if ((u = hull_unpack_begin(tree, why, cap)) == NULL)
		goto done;
	for (unsigned i = 0u; i < c->layers; i++) {
		char digest[80];

		if ((m = member_called(&a, c->layer[i])) == NULL) {
			say(why, cap, c->layer[i], "the archive does not hold this layer");
			goto done;
		}
		fprintf(stderr, "hull: load: layer %u of %u, %llu bytes\n", i + 1u, c->layers,
		        (unsigned long long)m->size);
		if (member_to(&a, m, layer_fd, why, cap) != 0 ||
		    hull_unpack_layer(u, layer_fd, &skipped, why, cap) != 0)
			goto done;
		if (blob_digest(m->name) != NULL) {
			snprintf(digest, sizeof digest, "sha256:%s", blob_digest(m->name));
			put_text_atom(&digests, digest);
		}
	}
	hull_unpack_owners(u, &owners, &owner_count);
	{
		hull_unpack *ended = u;

		u = NULL;
		if (hull_unpack_end(ended, why, cap) != 0)
			goto done;
	}
	put_raw(&provenance, "(10:provenance(7:builder");
	put_text_atom(&provenance, builder);
	put_raw(&provenance, ")(3:oci(7:digests");
	put_bytes(&provenance, digests.data, digests.length);
	put_raw(&provenance, ")(8:registry");
	{
		const char *base = strrchr(path, '/');
		char        from[512];

		snprintf(from, sizeof from, "file/%s", base != NULL ? base + 1 : path);
		put_text_atom(&provenance, from);
	}
	put_raw(&provenance, ")))");
	if (digests.failed || provenance.failed || runtime.failed) {
		say(why, cap, "out of memory", NULL);
		goto done;
	}
	extra = (hull_commit_extra){ provenance.data, provenance.length, runtime.data, runtime.length,
	                             owners, owner_count, 0 };
	if (hull_store_commit(store, tree, image_arch, ref_out, &extra, out, why, cap) != 0)
		goto done;
	out->skipped += skipped;
	result = 0;
done:
	hull_owners_free(owners, owner_count);
	{
		char ignored[64];

		hull_unpack_end(u, ignored, sizeof ignored);
	}
	if (layer_fd >= 0)
		close(layer_fd);
	if (a.fd >= 0)
		close(a.fd);
	if (scratch[0] != '\0')
		hull_store_remove_tree(AT_FDCWD, scratch);
	flock(store->lock, LOCK_UN);
	free(a.members);
	free(c);
	free(text);
	free(runtime.data);
	free(digests.data);
	free(provenance.data);
	return result;
}

/* --- an object by its chunks (Appendix C) -------------------------------- */

/* A chunk the puller has: where in which bytes. */
typedef struct held_chunk {
	uint8_t  digest[32];
	uint64_t offset;
	uint64_t length;
	int      fetched;   /* in the object being made, else in the seed */
} held_chunk;

/* The object being made from its chunks: its digest, the file it is
 * written to, and the chunks it can be made from without fetching -- the
 * seed's, the last version of the same path cut the same way, and those
 * fetched for it already. */
typedef struct assembly {
	int            active;
	int            skip;        /* the store has it */
	int            executable;
	uint8_t        object[32];
	int            fd;
	uint64_t       length;
	const uint8_t *seed;
	size_t         seed_length;
	held_chunk    *held;
	size_t         count;
	size_t         room;
} assembly;

static int hold(assembly *a, const uint8_t digest[32], uint64_t offset, uint64_t length,
                int fetched)
{
	if (a->count == a->room) {
		size_t      room = a->room != 0u ? a->room * 2u : 256u;
		held_chunk *grown = realloc(a->held, room * sizeof *grown);

		if (grown == NULL)
			return -1;
		a->held = grown;
		a->room = room;
	}
	memcpy(a->held[a->count].digest, digest, 32u);
	a->held[a->count].offset = offset;
	a->held[a->count].length = length;
	a->held[a->count].fetched = fetched;
	a->count++;
	return 0;
}

static void assembly_end(assembly *a)
{
	if (a->fd >= 0)
		close(a->fd);
	if (a->seed != NULL)
		munmap((void *)a->seed, a->seed_length);
	free(a->held);
	*a = (assembly){ .fd = -1 };
}

/* Begin making `object`: the seed is the object at the same path in the
 * tree `ref` named before, where it had one and it is not this. */
static int assembly_begin(assembly *a, hull_store *store, const uint8_t object[32],
                          int executable, const hull_manifest_file *now, uint32_t now_count,
                          const hull_manifest_file *was, uint32_t was_count, const char *tmp,
                          char *why, size_t cap)
{
	int fd;
	int ignored;

	assembly_end(a);
	a->active = 1;
	memcpy(a->object, object, 32u);
	a->executable = executable;
	if ((fd = hull_store_object_open(store, object, &ignored)) >= 0) {
		close(fd);
		a->skip = 1;
		return 0;
	}
	if ((a->fd = open(tmp, O_TMPFILE | O_RDWR | O_CLOEXEC, 0600)) < 0)
		return say(why, cap, tmp, strerror(errno));
	for (uint32_t i = 0u; i < now_count; i++) {
		const hull_manifest_file *old;
		struct stat               st;

		if (memcmp(now[i].digest, object, 32u) != 0 ||
		    (old = hull_manifest_file_at(was, was_count, now[i].path, now[i].length)) == NULL ||
		    (fd = hull_store_object_open(store, old->digest, &ignored)) < 0)
			continue;
		if (fstat(fd, &st) == 0 && st.st_size > 0) {
			void *seed = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);

			if (seed != MAP_FAILED) {
				a->seed = seed;
				a->seed_length = (size_t)st.st_size;
			}
		}
		close(fd);
		break;
	}
	for (size_t at = 0u; a->seed != NULL && at < a->seed_length;) {
		size_t      n = hull_chunk_length(a->seed + at, a->seed_length - at);
		hull_sha256 h;
		uint8_t     raw[32];

		hull_sha256_init(&h);
		hull_sha256_update(&h, a->seed + at, n);
		hull_sha256_final(&h, raw);
		if (hold(a, raw, at, n, 0) != 0)
			return say(why, cap, "out of memory", NULL);
		at += n;
	}
	return 0;
}

static int write_at(int fd, const uint8_t *data, size_t n, uint64_t at)
{
	while (n > 0u) {
		ssize_t wrote = pwrite(fd, data, n, (off_t)at);

		if (wrote < 0 && errno == EINTR)
			continue;
		if (wrote <= 0)
			return -1;
		data += wrote;
		n -= (size_t)wrote;
		at += (uint64_t)wrote;
	}
	return 0;
}

/* The next chunk, `digest` of `size` bytes: from what is held where it
 * is, else fetched into `buffer`, held to its digest. */
static int assembly_add(assembly *a, hull_remote *s, const char *digest, uint64_t size,
                        uint8_t *buffer, unsigned *fetched, char *why, size_t cap)
{
	uint8_t raw[32];

	if (a->skip)
		return 0;
	for (unsigned i = 0u; i < 32u; i++) {
		unsigned byte;

		sscanf(digest + 7 + 2u * i, "%2x", &byte);
		raw[i] = (uint8_t)byte;
	}
	for (size_t i = 0u; i < a->count; i++) {
		const held_chunk *c = &a->held[i];

		if (memcmp(c->digest, raw, 32u) != 0 || c->length != size)
			continue;
		if (!c->fetched) {
			if (write_at(a->fd, a->seed + c->offset, (size_t)size, a->length) != 0)
				return say(why, cap, "an object could not be written", strerror(errno));
		} else {
			if (pread(a->fd, buffer, (size_t)size, (off_t)c->offset) != (ssize_t)size ||
			    write_at(a->fd, buffer, (size_t)size, a->length) != 0)
				return say(why, cap, "an object could not be written", strerror(errno));
		}
		a->length += size;
		return 0;
	}
	{
		hull_remote_sink blob = { .data = buffer, .cap = HULL_CHUNK_MAX, .fd = -1 };
		char             url[8192];

		if (size > HULL_CHUNK_MAX)
			return say(why, cap, digest, "a chunk is larger than a chunk may be");
		hull_sha256_init(&blob.hash);
		snprintf(url, sizeof url, "https://%s/v2/%s/blobs/%s", s->img.host, s->img.repository,
		         digest);
		if (hull_remote_fetch(s, url, NULL, &blob, why, cap) != 0 ||
		    hull_remote_verify(&blob, digest, "a chunk", why, cap) != 0)
			return -1;
		if (blob.length != size)
			return say(why, cap, digest, "a chunk is not the size its layer says");
		if (write_at(a->fd, buffer, blob.length, a->length) != 0)
			return say(why, cap, "an object could not be written", strerror(errno));
		if (hold(a, raw, a->length, size, 1) != 0)
			return say(why, cap, "out of memory", NULL);
		a->length += size;
		(*fetched)++;
	}
	return 0;
}

/* The object made: into the store, which holds it to its digest. */
static int assembly_finish(assembly *a, hull_store *store, char *why, size_t cap)
{
	int result = 0;

	if (a->active && !a->skip)
		result = hull_store_object_put(store, a->fd, a->length, a->executable, a->object, why,
		                               cap);
	assembly_end(a);
	return result;
}

int hull_pull_objects(hull_store *store, const char *image, const char *ref, char hex[65],
                      unsigned *fetched, unsigned *total, char *why, size_t cap)
{
	static const char *const accept[] = { "Accept: application/vnd.oci.image.manifest.v1+json",
	                                      NULL };
	hull_remote         s = { 0 };
	uint8_t            *data = malloc(MAX_DOCUMENT);
	hull_remote_sink    body = { .data = data, .cap = MAX_DOCUMENT, .fd = -1 };
	hull_https_response got;
	hull_json           doc;
	hull_json_value     root;
	hull_json_value     config;
	hull_json_value     layers;
	hull_json_value     layer;
	const char         *bad;
	char                url[8192];
	char                tree_digest[80];
	char                artifact[128] = "";
	uint8_t            *tree = NULL;
	uint8_t           (*named)[32] = NULL;
	uint32_t            named_count = 0u;
	hull_manifest_file *now = NULL;
	uint32_t            now_count = 0u;
	hull_manifest_file *was = NULL;
	uint32_t            was_count = 0u;
	uint8_t            *was_tree = NULL;
	size_t              was_length = 0u;
	char                was_hex[65];
	uint8_t            *chunk_buffer = NULL;
	assembly            building = { .fd = -1 };
	int                 first = 1;
	int                 result = -1;

	*fetched = 0u;
	*total = 0u;
	if (data == NULL)
		return say(why, cap, "out of memory", NULL);
	hull_https_allow_loopback();
	if (hull_remote_parse(image, &s.img, why, cap) != 0)
		goto done;
	if (s.img.by_digest) {
		say(why, cap, image, "objects are published under a tag");
		goto done;
	}
	if (strlen(s.img.reference) + sizeof HULL_OBJECTS_SUFFIX > sizeof s.img.reference) {
		say(why, cap, image, "its tag is too long to have .hull added");
		goto done;
	}
	strcat(s.img.reference, HULL_OBJECTS_SUFFIX);
	snprintf(url, sizeof url, "https://%s/v2/%s/manifests/%s", s.img.host, s.img.repository,
	         s.img.reference);
	if (hull_remote_request(&s, "GET", url, accept, NULL, &body, &got, why, cap) != 0)
		goto done;
	if (got.status == 404) {
		result = 1;
		goto done;
	}
	if (got.status != 200) {
		say(why, cap, s.img.reference, "the registry did not give its manifest");
		goto done;
	}
	if (hull_json_open(&doc, (const char *)data, body.length, &root, &bad) != 0) {
		say(why, cap, s.img.reference, bad);
		goto done;
	}
	hull_json_member_string(&doc, &root, "artifactType", artifact, sizeof artifact);
	if (strcmp(artifact, HULL_TREE_TYPE) != 0 || !hull_json_member(&doc, &root, "config", &config) ||
	    hull_json_member_string(&doc, &config, "digest", tree_digest, sizeof tree_digest) < 0 ||
	    !hull_remote_is_digest(tree_digest) || !hull_json_member(&doc, &root, "layers", &layers)) {
		say(why, cap, s.img.reference, "it is not a tree hull published");
		goto done;
	}
	/* The store's lock, shared, while objects are written under tmp/. */
	flock(store->lock, LOCK_SH);
	/* The tree's manifest, which names every object: by its digest. */
	{
		char             tmp[4400];
		struct stat      st;
		hull_remote_sink into = { .fd = -1 };

		snprintf(tmp, sizeof tmp, "%s/tmp", store->root);
		if ((into.fd = open(tmp, O_TMPFILE | O_RDWR | O_CLOEXEC, 0600)) < 0) {
			say(why, cap, tmp, strerror(errno));
			goto done;
		}
		snprintf(url, sizeof url, "https://%s/v2/%s/blobs/%s", s.img.host, s.img.repository,
		         tree_digest);
		if (hull_remote_fetch(&s, url, NULL, &into, why, cap) != 0 ||
		    hull_remote_verify(&into, tree_digest, "the tree's manifest", why, cap) != 0 ||
		    fstat(into.fd, &st) != 0 || st.st_size <= 0 || st.st_size > (256 << 20) ||
		    (tree = malloc((size_t)st.st_size)) == NULL ||
		    pread(into.fd, tree, (size_t)st.st_size, 0) != st.st_size) {
			if (why[0] == '\0')
				say(why, cap, "the tree's manifest", "it could not be read");
			close(into.fd);
			goto done;
		}
		close(into.fd);
		if (hull_manifest_objects(tree, (size_t)st.st_size, &named, &named_count) != 0) {
			say(why, cap, "the tree's manifest", "it is not a manifest");
			goto done;
		}
		/* What this tree has at each path, and what the ref named before
		 * had: a large object's seed is the last version's at its path. */
		if (hull_manifest_files(tree, (size_t)st.st_size, &now, &now_count) != 0) {
			say(why, cap, "the tree's manifest", "it is not a manifest");
			goto done;
		}
		{
			char ignored[200];

			if (hull_store_manifest_of(store, ref, was_hex, &was_tree, &was_length, ignored,
			                           sizeof ignored) == 0 &&
			    hull_manifest_files(was_tree, was_length, &was, &was_count) != 0)
				was_count = 0u;
		}
		/* Each object the store lacks, by its digest, which is its name;
		 * a large one by its chunks, those the seed has taken from it. */
		while (hull_json_element(&doc, &layers, first, &layer)) {
			char            digest[80];
			char            media[128] = "";
			char            of[80] = "";
			uint8_t         raw[32];
			int             executable;
			int             fd;
			int             chunk;
			long long       size = -1;
			hull_json_value annotations;
			hull_json_value size_value;
			struct stat     object;

			first = 0;
			if (hull_json_member_string(&doc, &layer, "digest", digest, sizeof digest) < 0 ||
			    !hull_remote_is_digest(digest) ||
			    hull_json_member_string(&doc, &layer, "mediaType", media, sizeof media) < 0) {
				say(why, cap, s.img.reference, "a layer of it names no object");
				goto done;
			}
			(*total)++;
			chunk = strcmp(media, HULL_CHUNK_TYPE) == 0 ||
			        strcmp(media, HULL_CHUNK_EXECUTABLE_TYPE) == 0;
			if (chunk) {
				uint8_t of_raw[32];

				if (!hull_json_member(&doc, &layer, "annotations", &annotations) ||
				    hull_json_member_string(&doc, &annotations, HULL_CHUNK_OF, of, sizeof of) < 0 ||
				    !hull_remote_is_digest(of) ||
				    !hull_json_member(&doc, &layer, "size", &size_value) ||
				    hull_json_integer(&doc, &size_value, &size) != 0 || size <= 0) {
					say(why, cap, s.img.reference, "a chunk of it names no object");
					goto done;
				}
				for (unsigned j = 0u; j < 32u; j++) {
					unsigned byte;

					sscanf(of + 7 + 2u * j, "%2x", &byte);
					of_raw[j] = (uint8_t)byte;
				}
				if (!building.active || memcmp(building.object, of_raw, 32u) != 0) {
					if (assembly_finish(&building, store, why, cap) != 0 ||
					    assembly_begin(&building, store, of_raw,
					                   strcmp(media, HULL_CHUNK_EXECUTABLE_TYPE) == 0, now,
					                   now_count, was, was_count, tmp, why, cap) != 0)
						goto done;
				}
				if (chunk_buffer == NULL && (chunk_buffer = malloc(HULL_CHUNK_MAX)) == NULL) {
					say(why, cap, "out of memory", NULL);
					goto done;
				}
				if (assembly_add(&building, &s, digest, (uint64_t)size, chunk_buffer, fetched,
				                 why, cap) != 0)
					goto done;
				continue;
			}
			if (assembly_finish(&building, store, why, cap) != 0)
				goto done;
			for (unsigned j = 0u; j < 32u; j++) {
				unsigned byte;

				sscanf(digest + 7 + 2u * j, "%2x", &byte);
				raw[j] = (uint8_t)byte;
			}
			if ((fd = hull_store_object_open(store, raw, &executable)) >= 0) {
				close(fd);
				continue;
			}
			{
				hull_remote_sink blob = { .fd = open(tmp, O_TMPFILE | O_RDWR | O_CLOEXEC, 0600) };

				if (blob.fd < 0) {
					say(why, cap, tmp, strerror(errno));
					goto done;
				}
				snprintf(url, sizeof url, "https://%s/v2/%s/blobs/%s", s.img.host,
				         s.img.repository, digest);
				if (hull_remote_fetch(&s, url, NULL, &blob, why, cap) != 0 ||
				    hull_remote_verify(&blob, digest, "an object", why, cap) != 0 ||
				    fstat(blob.fd, &object) != 0 || lseek(blob.fd, 0, SEEK_SET) != 0 ||
				    hull_store_object_put(store, blob.fd, (uint64_t)object.st_size,
				                          strcmp(media, HULL_EXECUTABLE_TYPE) == 0, raw, why,
				                          cap) != 0) {
					close(blob.fd);
					goto done;
				}
				close(blob.fd);
				(*fetched)++;
			}
		}
		if (assembly_finish(&building, store, why, cap) != 0)
			goto done;
		/* Every object the manifest names in the store now, whatever the
		 * layers said. */
		for (uint32_t i = 0u; i < named_count; i++) {
			int executable;
			int fd = hull_store_object_open(store, named[i], &executable);

			if (fd < 0) {
				say(why, cap, s.img.reference, "its manifest names an object it does not hold");
				goto done;
			}
			close(fd);
		}
		result = hull_store_manifest_put(store, tree, (size_t)st.st_size, ref, hex, why, cap);
		if (result == 0 && strcmp(tree_digest + 7, hex) != 0) {
			say(why, cap, "the tree's manifest", "it is not the hash it was stored by");
			result = -1;
		}
	}
done:
	assembly_end(&building);
	flock(store->lock, LOCK_UN);
	free(chunk_buffer);
	free(was);
	free(was_tree);
	free(now);
	free(named);
	free(tree);
	free(data);
	return result;
}
