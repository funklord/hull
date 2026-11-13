/*
 * pack.c -- the Hull pack, a stored tree as one file (project.md section
 * 3.13): its manifest and every object it names, each compressed alone by
 * zstd, behind an index upfront, so that one object is found and read
 * without the rest.
 *
 * The layout, every number big-endian:
 *
 *   "HULLPACK"                  8 bytes
 *   version                     u32, 1
 *   object count                u32
 *   manifest's length           u64, as stored
 *   manifest's frame's length   u64, compressed
 *   index, object count of:     the object's sha256 (32 bytes), its flags
 *                               (u8, 1 executable), its length (u64), its
 *                               frame's offset from the data's start (u64)
 *                               and its frame's length (u64)
 *   the manifest's frame
 *   the data: each object's frame, in the index's order, the objects'
 *   digests ascending and each once
 *
 * A pack made against a base (`hull pack --against`) may hold an object
 * as a recipe against the base's object at the same path, flag 2 in the
 * index, its frame then the recipe compressed: the base object's digest,
 * and ops, each a kind byte -- 0 copy (u64 offset in the base object, u64
 * length), 1 literal (u64 length, then the bytes) -- ended by 0xFF. The
 * ops are found by content-defined chunking (Appendix C): both objects cut
 * where a rolling gear hash of their bytes says, so a byte changed moves
 * one chunk and not the file. A pack holding any recipe is version 2, one
 * holding none version 1, as it always was.
 *
 * Its identity is its manifest's hash, which unpacking checks the
 * manifest against, as each object against its digest. Compressed at a
 * fixed level by one thread, the same tree gives the same bytes under one
 * libzstd; another version of libzstd may compress otherwise, the
 * library being the system's (the holder's word, 2026-11-02), and the
 * content and its hash are the same either way.
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <zstd.h>

#include "sexpr.h"
#include "sha256.h"
#include "store.h"

#define PACK_MAGIC   "HULLPACK"
#define PACK_VERSION 1u
#define PACK_DELTAS  2u    /* the version of a pack holding a recipe */
#define FLAG_DELTA   2u
#define DELTA_FLOOR  (64u << 10)  /* an object smaller is sent whole */
#define CHUNK_MIN    (8u << 10)
#define CHUNK_MASK   ((1u << 15) - 1u)  /* about 32 KiB a chunk */
#define CHUNK_MAX    (128u << 10)
#define PACK_LEVEL   9
#define INDEX_ENTRY  (32u + 1u + 8u + 8u + 8u)
#define MAX_OBJECTS  (1u << 22)

typedef struct object {
	uint8_t  digest[32];
	uint8_t  flags;
	uint64_t length;
	uint64_t offset;
	uint64_t frame;
} object;

static int say(char *why, size_t cap, const char *what, const char *detail)
{
	snprintf(why, cap, "%s%s%s", what, detail != NULL ? ": " : "", detail != NULL ? detail : "");
	return -1;
}

static void put64(uint8_t *at, uint64_t value)
{
	for (int i = 7; i >= 0; i--, value >>= 8)
		at[i] = (uint8_t)value;
}

static uint64_t get64(const uint8_t *at)
{
	uint64_t value = 0u;

	for (int i = 0; i < 8; i++)
		value = value << 8 | at[i];
	return value;
}

static int write_all(int fd, const void *data, size_t length)
{
	const uint8_t *at = data;

	while (length > 0u) {
		ssize_t n = write(fd, at, length);

		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0)
			return -1;
		at += n;
		length -= (size_t)n;
	}
	return 0;
}

static int read_all(int fd, void *data, size_t length)
{
	uint8_t *at = data;

	while (length > 0u) {
		ssize_t n = read(fd, at, length);

		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0)
			return -1;
		at += n;
		length -= (size_t)n;
	}
	return 0;
}

/* A file of the store's own, in its tmp/, gone once closed: what a pack
 * stages is as large as the tree, and belongs beside the objects, on the
 * filesystem that holds them, not in /tmp -- a tmpfs, or one nearly full,
 * as a measurement of a 796 MB tree found it. */
static FILE *store_temp(const hull_store *store)
{
	char  path[4200];
	int   fd;
	FILE *file;

	snprintf(path, sizeof path, "%s/tmp/pack.XXXXXX", store->root);
	if ((fd = mkostemp(path, O_CLOEXEC)) < 0)
		return NULL;
	unlink(path);
	if ((file = fdopen(fd, "w+")) == NULL)
		close(fd);
	return file;
}

static int by_digest(const void *a, const void *b)
{
	return memcmp(((const object *)a)->digest, ((const object *)b)->digest, 32u);
}

/* Every object the manifest names, each once, into `*objects`. */
static int objects_of(const uint8_t *manifest, size_t length, object **objects, uint32_t *count,
                      char *why, size_t cap)
{
	hull_sexpr_doc  doc;
	hull_sexpr_form top;
	hull_sexpr_form item;
	uint32_t        n = 0u;
	uint32_t        room = 64u;
	object         *list = malloc(room * sizeof *list);

	if (list == NULL)
		return say(why, cap, "out of memory", NULL);
	if (hull_sexpr_open(&doc, manifest, length, HULL_SEXPR_CANONICAL, NULL) != HULL_SEXPR_OK ||
	    !hull_sexpr_first(&doc, &top) || !hull_sexpr_child(&doc, &top, &item)) {
		free(list);
		return say(why, cap, "the manifest is not one", NULL);
	}
	while (hull_sexpr_next(&doc, &item, &item)) {
		hull_sexpr_form head;
		hull_sexpr_form entry;

		if (item.kind != HULL_SEXPR_LIST || !hull_sexpr_child(&doc, &item, &head) ||
		    !hull_sexpr_is(&doc, &head, "entries"))
			continue;
		entry = head;
		while (hull_sexpr_next(&doc, &entry, &entry)) {
			hull_sexpr_form field;

			if (entry.kind != HULL_SEXPR_LIST || !hull_sexpr_child(&doc, &entry, &field))
				continue;
			while (hull_sexpr_next(&doc, &field, &field)) {
				hull_sexpr_form key;
				hull_sexpr_form value;

				if (field.kind != HULL_SEXPR_LIST || !hull_sexpr_child(&doc, &field, &key) ||
				    !hull_sexpr_is(&doc, &key, "object") || !hull_sexpr_next(&doc, &key, &value) ||
				    value.content_length != 32u)
					continue;
				if (n == room) {
					object *grown = room < MAX_OBJECTS ?
					                realloc(list, (size_t)room * 2u * sizeof *list) : NULL;

					if (grown == NULL) {
						free(list);
						return say(why, cap, "the manifest names too many objects", NULL);
					}
					list = grown;
					room *= 2u;
				}
				memset(&list[n], 0, sizeof list[n]);
				memcpy(list[n++].digest, doc.data + value.content, 32u);
			}
		}
	}
	if (n > 1u)
		qsort(list, n, sizeof *list, by_digest);
	{
		uint32_t kept = 0u;

		for (uint32_t i = 0u; i < n; i++)
			if (kept == 0u || memcmp(list[kept - 1u].digest, list[i].digest, 32u) != 0)
				list[kept++] = list[i];
		n = kept;
	}
	*objects = list;
	*count = n;
	return 0;
}

/* A path and the object it names, from a manifest's entries. */
typedef hull_manifest_file named;

static int by_path(const void *a, const void *b)
{
	const named *x = a;
	const named *y = b;
	size_t       n = x->length < y->length ? x->length : y->length;
	int          c = memcmp(x->path, y->path, n);

	return c != 0 ? c : (x->length > y->length) - (x->length < y->length);
}

static int named_by_digest(const void *a, const void *b)
{
	return memcmp(((const named *)a)->digest, ((const named *)b)->digest, 32u);
}

/* Each regular file's path and object, into `*list` (pointing into
 * `manifest`, which must outlive it); sorted by `order`. */
static int paths_of(const uint8_t *manifest, size_t length, named **list, uint32_t *count,
                    int (*order)(const void *, const void *))
{
	hull_sexpr_doc  doc;
	hull_sexpr_form top;
	hull_sexpr_form item;
	uint32_t        n = 0u;
	uint32_t        room = 64u;
	named          *out = malloc(room * sizeof *out);

	if (out == NULL)
		return -1;
	if (hull_sexpr_open(&doc, manifest, length, HULL_SEXPR_CANONICAL, NULL) != HULL_SEXPR_OK ||
	    !hull_sexpr_first(&doc, &top) || !hull_sexpr_child(&doc, &top, &item)) {
		free(out);
		return -1;
	}
	while (hull_sexpr_next(&doc, &item, &item)) {
		hull_sexpr_form head;
		hull_sexpr_form entry;

		if (item.kind != HULL_SEXPR_LIST || !hull_sexpr_child(&doc, &item, &head) ||
		    !hull_sexpr_is(&doc, &head, "entries"))
			continue;
		entry = head;
		while (hull_sexpr_next(&doc, &entry, &entry)) {
			hull_sexpr_form field;
			named           one = { .path = NULL };
			int             object = 0;

			if (entry.kind != HULL_SEXPR_LIST || !hull_sexpr_child(&doc, &entry, &field))
				continue;
			while (hull_sexpr_next(&doc, &field, &field)) {
				hull_sexpr_form key;
				hull_sexpr_form value;

				if (field.kind != HULL_SEXPR_LIST || !hull_sexpr_child(&doc, &field, &key) ||
				    !hull_sexpr_next(&doc, &key, &value))
					continue;
				if (hull_sexpr_is(&doc, &key, "object") && value.content_length == 32u) {
					memcpy(one.digest, doc.data + value.content, 32u);
					object = 1;
				} else if (hull_sexpr_is(&doc, &key, "path")) {
					one.path   = (const char *)doc.data + value.content;
					one.length = value.content_length;
				}
			}
			if (!object || one.path == NULL)
				continue;
			if (n == room) {
				named *grown = room < MAX_OBJECTS ? realloc(out, (size_t)room * 2u * sizeof *out) :
				               NULL;

				if (grown == NULL) {
					free(out);
					return -1;
				}
				out = grown;
				room *= 2u;
			}
			out[n++] = one;
		}
	}
	if (n > 1u)
		qsort(out, n, sizeof *out, order);
	*list = out;
	*count = n;
	return 0;
}

/* --- content-defined chunking -------------------------------------------- */

/* The gear table: 256 words of splitmix64 from a fixed seed, the same in
 * every hull, since writer and reader need not agree on it -- a recipe
 * says its copies by offset -- but two writers making the same pack
 * should. */
static uint64_t gear[256];

static void gear_init(void)
{
	uint64_t x = 0x68756c6c67656172ull;  /* "hullgear" */

	if (gear[255] != 0u)
		return;
	for (unsigned i = 0u; i < 256u; i++) {
		uint64_t z = (x += 0x9e3779b97f4a7c15ull);

		z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
		z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
		gear[i] = z ^ (z >> 31);
	}
}

/* The length of the chunk at `data`, of `left` bytes left: cut where the
 * rolling hash's bits under `mask` are zero, between `least` and `most`. */
static size_t cut(const uint8_t *data, size_t left, size_t least, uint64_t mask, size_t most)
{
	uint64_t h = 0u;
	size_t   i;

	gear_init();
	if (left <= least)
		return left;
	for (i = 0u; i < left && i < most; i++) {
		h = (h << 1) + gear[data[i]];
		if (i >= least && (h & mask) == 0u)
			return i + 1u;
	}
	return i;
}

static size_t chunk_length(const uint8_t *data, size_t left)
{
	return cut(data, left, CHUNK_MIN, CHUNK_MASK, CHUNK_MAX);
}

size_t hull_chunk_length(const uint8_t *data, size_t left)
{
	return cut(data, left, HULL_CHUNK_MIN, ((uint64_t)1u << 20) - 1u, HULL_CHUNK_MAX);
}

typedef struct chunk {
	uint8_t  digest[32];
	uint64_t offset;
	uint64_t length;
} chunk;

static int chunk_order(const void *a, const void *b)
{
	return memcmp(((const chunk *)a)->digest, ((const chunk *)b)->digest, 32u);
}

/* Write the recipe for `new` against `base` to `out`: copies where a
 * chunk of `new` is one of `base`'s, literals where it is not, adjacent
 * copies and literals merged. 0, or -1. */
static int write_recipe(const uint8_t base_digest[32], const uint8_t *base, size_t base_length,
                        const uint8_t *new, size_t new_length, FILE *out)
{
	chunk   *chunks = NULL;
	size_t   count = 0u;
	size_t   room = 0u;
	size_t   at;
	uint64_t copy_at = 0u;
	uint64_t copy_length = 0u;
	size_t   literal_at = 0u;
	size_t   literal_length = 0u;
	uint8_t  op[17];
	int      result = -1;

	gear_init();
	for (at = 0u; at < base_length;) {
		size_t n = chunk_length(base + at, base_length - at);

		if (count == room) {
			chunk *grown = realloc(chunks, (room = room != 0u ? room * 2u : 1024u) * sizeof *chunks);

			if (grown == NULL)
				goto done;
			chunks = grown;
		}
		{
			hull_sha256 h;

			hull_sha256_init(&h);
			hull_sha256_update(&h, base + at, n);
			hull_sha256_final(&h, chunks[count].digest);
		}
		chunks[count].offset = at;
		chunks[count].length = n;
		count++;
		at += n;
	}
	if (count > 1u)
		qsort(chunks, count, sizeof *chunks, chunk_order);
	if (fwrite(base_digest, 1u, 32u, out) != 32u)
		goto done;
	for (at = 0u; at <= new_length;) {
		size_t       n = at < new_length ? chunk_length(new + at, new_length - at) : 0u;
		const chunk *found = NULL;
		chunk        key;

		if (n > 0u) {
			hull_sha256 h;

			hull_sha256_init(&h);
			hull_sha256_update(&h, new + at, n);
			hull_sha256_final(&h, key.digest);
			found = count > 0u ? bsearch(&key, chunks, count, sizeof *chunks, chunk_order) : NULL;
			if (found != NULL && memcmp(base + found->offset, new + at, n) != 0)
				found = NULL;
		}
		/* What is pending goes out where the next chunk does not extend it. */
		if (literal_length > 0u && (found != NULL || n == 0u)) {
			op[0] = 1u;
			put64(op + 1, literal_length);
			if (fwrite(op, 1u, 9u, out) != 9u ||
			    fwrite(new + literal_at, 1u, literal_length, out) != literal_length)
				goto done;
			literal_length = 0u;
		}
		if (copy_length > 0u && (found == NULL || found->offset != copy_at + copy_length)) {
			op[0] = 0u;
			put64(op + 1, copy_at);
			put64(op + 9, copy_length);
			if (fwrite(op, 1u, 17u, out) != 17u)
				goto done;
			copy_length = 0u;
		}
		if (n == 0u)
			break;
		if (found != NULL) {
			if (copy_length == 0u)
				copy_at = found->offset;
			copy_length += n;
		} else {
			if (literal_length == 0u)
				literal_at = at;
			literal_length += n;
		}
		at += n;
	}
	op[0] = 0xFFu;
	result = fwrite(op, 1u, 1u, out) == 1u ? 0 : -1;
done:
	free(chunks);
	return result;
}

/* An object rebuilt from its recipe, `recipe` of `length` bytes, against
 * the base object it names, which the store holds, into `out`. 0, or -1
 * with why. */
static int apply_recipe(hull_store *store, const uint8_t *recipe, size_t length, int out,
                        char *why, size_t cap)
{
	int         executable;
	int         fd;
	struct stat st;
	uint8_t    *base = NULL;
	size_t      at = 32u;
	int         result = -1;

	if (length < 33u)
		return say(why, cap, "a recipe of the pack is cut", NULL);
	if ((fd = hull_store_object_open(store, recipe, &executable)) < 0)
		return say(why, cap, "a recipe of the pack names a base object this store does not "
		           "hold: a pack made against a base wants that base unpacked first", NULL);
	if (fstat(fd, &st) != 0 ||
	    (st.st_size > 0 && (base = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0)) ==
	                       MAP_FAILED)) {
		close(fd);
		return say(why, cap, "a base object could not be read", strerror(errno));
	}
	close(fd);
	while (at < length && recipe[at] != 0xFFu) {
		if (recipe[at] == 0u && length - at >= 17u) {
			uint64_t from = get64(recipe + at + 1);
			uint64_t n = get64(recipe + at + 9);

			if (from > (uint64_t)st.st_size || n > (uint64_t)st.st_size - from ||
			    write_all(out, base + from, (size_t)n) != 0)
				break;
			at += 17u;
		} else if (recipe[at] == 1u && length - at >= 9u) {
			uint64_t n = get64(recipe + at + 1);

			if (n > length - at - 9u || write_all(out, recipe + at + 9, (size_t)n) != 0)
				break;
			at += 9u + (size_t)n;
		} else {
			break;
		}
	}
	if (at < length && recipe[at] == 0xFFu && at + 1u == length)
		result = 0;
	else
		say(why, cap, "a recipe of the pack is not one", NULL);
	if (base != NULL)
		munmap(base, (size_t)st.st_size);
	return result;
}

/* `in`'s next `length` bytes as one zstd frame to `out`; its length into
 * `*frame`. */
static int compress_into(ZSTD_CCtx *cctx, int in, uint64_t length, int out, uint64_t *frame)
{
	static uint8_t from[1 << 17];
	static uint8_t to[1 << 17];
	uint64_t       left = length;
	uint64_t       wrote = 0u;

	ZSTD_CCtx_reset(cctx, ZSTD_reset_session_only);
	ZSTD_CCtx_setPledgedSrcSize(cctx, length);
	for (;;) {
		size_t         n = 0u;
		ZSTD_EndDirective end;
		ZSTD_inBuffer  input;
		size_t         remaining;

		if (left > 0u) {
			n = left < sizeof from ? (size_t)left : sizeof from;
			if (read_all(in, from, n) != 0)
				return -1;
			left -= n;
		}
		end = left == 0u ? ZSTD_e_end : ZSTD_e_continue;
		input = (ZSTD_inBuffer){ from, n, 0u };
		do {
			ZSTD_outBuffer output = { to, sizeof to, 0u };

			remaining = ZSTD_compressStream2(cctx, &output, &input, end);
			if (ZSTD_isError(remaining) || write_all(out, to, output.pos) != 0)
				return -1;
			wrote += output.pos;
		} while (end == ZSTD_e_end ? remaining != 0u : input.pos < input.size);
		if (end == ZSTD_e_end)
			break;
	}
	*frame = wrote;
	return 0;
}

int hull_store_pack(hull_store *store, const char *name, const char *against, int out,
                    char hex[65], char *why, size_t cap)
{
	uint8_t   *manifest = NULL;
	size_t     manifest_length = 0u;
	object    *objects = NULL;
	uint32_t   count = 0u;
	ZSTD_CCtx *cctx = NULL;
	FILE      *data = NULL;
	void      *frame = NULL;
	size_t     frame_length;
	uint8_t    head[8 + 4 + 4 + 8 + 8];
	int        result = -1;
	uint8_t   *base = NULL;          /* the base's manifest, against one */
	size_t     base_length = 0u;
	named     *base_paths = NULL;    /* its files by path */
	uint32_t   base_count = 0u;
	named     *new_paths = NULL;     /* this tree's files by object */
	uint32_t   new_count = 0u;
	int        deltas = 0;

	if (hull_store_manifest_of(store, name, hex, &manifest, &manifest_length, why, cap) != 0)
		return -1;
	if (objects_of(manifest, manifest_length, &objects, &count, why, cap) != 0)
		goto done;
	/* Against a base: only what the base does not name, both lists in
	 * digest order, for a store that holds the base already. */
	if (against != NULL) {
		object  *held = NULL;
		uint32_t held_count = 0u;
		uint32_t kept = 0u;
		char     base_hex[65];

		if (hull_store_manifest_of(store, against, base_hex, &base, &base_length, why, cap) != 0)
			goto done;
		if (objects_of(base, base_length, &held, &held_count, why, cap) != 0)
			goto done;
		for (uint32_t i = 0u; i < count; i++)
			if (held_count == 0u ||
			    bsearch(&objects[i], held, held_count, sizeof *held, by_digest) == NULL)
				objects[kept++] = objects[i];
		count = kept;
		free(held);
		/* And which file each is, and which object the base had there,
		 * for a recipe against it (Appendix C). */
		if (paths_of(base, base_length, &base_paths, &base_count, by_path) != 0 ||
		    paths_of(manifest, manifest_length, &new_paths, &new_count, named_by_digest) != 0) {
			say(why, cap, "a manifest's paths could not be read", NULL);
			goto done;
		}
	}
	if ((cctx = ZSTD_createCCtx()) == NULL ||
	    ZSTD_isError(ZSTD_CCtx_setParameter(cctx, ZSTD_c_compressionLevel, PACK_LEVEL)) ||
	    ZSTD_isError(ZSTD_CCtx_setParameter(cctx, ZSTD_c_checksumFlag, 1)) ||
	    (data = store_temp(store)) == NULL) {
		say(why, cap, "the pack's compressor could not be made", NULL);
		goto done;
	}
	/* The data first, into a file of its own, since the index before it
	 * holds each frame's offset and length. */
	for (uint32_t i = 0u; i < count; i++) {
		int         executable = 0;
		int         fd = hull_store_object_open(store, objects[i].digest, &executable);
		struct stat st;
		long        at = ftell(data);

		if (fd < 0 || fstat(fd, &st) != 0) {
			if (fd >= 0)
				close(fd);
			say(why, cap, "an object the manifest names is not in the store", NULL);
			goto done;
		}
		fflush(data);
		objects[i].flags  = executable ? 1u : 0u;
		objects[i].length = (uint64_t)st.st_size;
		objects[i].offset = (uint64_t)at;
		/* A large file the base had at its path: a recipe against the
		 * base's object, where that is smaller than the object. */
		if (base_paths != NULL && objects[i].length >= DELTA_FLOOR) {
			named        key;
			const named *mine;
			const named *theirs = NULL;

			memcpy(key.digest, objects[i].digest, 32u);
			mine = new_count > 0u ? bsearch(&key, new_paths, new_count, sizeof *new_paths,
			                                named_by_digest) : NULL;
			if (mine != NULL && base_count > 0u)
				theirs = bsearch(mine, base_paths, base_count, sizeof *base_paths, by_path);
			if (theirs != NULL) {
				int         base_executable;
				int         base_fd = hull_store_object_open(store, theirs->digest,
				                                             &base_executable);
				struct stat base_st;
				uint8_t    *old = NULL;
				uint8_t    *now = NULL;
				FILE       *recipe = NULL;
				long        recipe_length = 0L;

				if (base_fd >= 0 && fstat(base_fd, &base_st) == 0 && base_st.st_size > 0 &&
				    (old = mmap(NULL, (size_t)base_st.st_size, PROT_READ, MAP_PRIVATE, base_fd,
				                0)) != MAP_FAILED &&
				    (now = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0)) !=
				        MAP_FAILED &&
				    (recipe = store_temp(store)) != NULL &&
				    write_recipe(theirs->digest, old, (size_t)base_st.st_size, now,
				                 (size_t)st.st_size, recipe) == 0 &&
				    fflush(recipe) == 0 && (recipe_length = ftell(recipe)) > 0 &&
				    (uint64_t)recipe_length < objects[i].length) {
					rewind(recipe);
					if (compress_into(cctx, fileno(recipe), (uint64_t)recipe_length,
					                  fileno(data), &objects[i].frame) != 0) {
						say(why, cap, "a recipe could not be packed", strerror(errno));
						recipe_length = -1L;
					} else {
						objects[i].flags |= FLAG_DELTA;
						deltas = 1;
					}
				}
				if (old != NULL && old != MAP_FAILED)
					munmap(old, (size_t)base_st.st_size);
				if (now != NULL && now != MAP_FAILED)
					munmap(now, (size_t)st.st_size);
				if (recipe != NULL)
					fclose(recipe);
				if (base_fd >= 0)
					close(base_fd);
				if (recipe_length < 0L) {
					close(fd);
					goto done;
				}
				if (objects[i].flags & FLAG_DELTA) {
					close(fd);
					fseek(data, 0L, SEEK_END);
					continue;
				}
				/* No recipe: the object whole, from its start. */
				if (lseek(fd, 0, SEEK_SET) != 0) {
					close(fd);
					say(why, cap, "an object could not be packed", strerror(errno));
					goto done;
				}
				fseek(data, at, SEEK_SET);
			}
		}
		if (compress_into(cctx, fd, objects[i].length, fileno(data), &objects[i].frame) != 0) {
			close(fd);
			say(why, cap, "an object could not be packed", strerror(errno));
			goto done;
		}
		close(fd);
		fseek(data, 0L, SEEK_END);
	}
	frame_length = ZSTD_compressBound(manifest_length);
	if ((frame = malloc(frame_length)) == NULL) {
		say(why, cap, "out of memory", NULL);
		goto done;
	}
	ZSTD_CCtx_reset(cctx, ZSTD_reset_session_only);
	frame_length = ZSTD_compress2(cctx, frame, frame_length, manifest, manifest_length);
	if (ZSTD_isError(frame_length)) {
		say(why, cap, "the manifest could not be packed", ZSTD_getErrorName(frame_length));
		goto done;
	}
	memcpy(head, PACK_MAGIC, 8u);
	head[8] = 0u, head[9] = 0u, head[10] = 0u;
	head[11] = (uint8_t)(deltas ? PACK_DELTAS : PACK_VERSION);
	head[12] = (uint8_t)(count >> 24), head[13] = (uint8_t)(count >> 16);
	head[14] = (uint8_t)(count >> 8), head[15] = (uint8_t)count;
	put64(head + 16, manifest_length);
	put64(head + 24, frame_length);
	if (write_all(out, head, sizeof head) != 0)
		goto failed;
	for (uint32_t i = 0u; i < count; i++) {
		uint8_t entry[INDEX_ENTRY];

		memcpy(entry, objects[i].digest, 32u);
		entry[32] = objects[i].flags;
		put64(entry + 33, objects[i].length);
		put64(entry + 41, objects[i].offset);
		put64(entry + 49, objects[i].frame);
		if (write_all(out, entry, sizeof entry) != 0)
			goto failed;
	}
	if (write_all(out, frame, frame_length) != 0)
		goto failed;
	rewind(data);
	{
		uint8_t chunk[1 << 16];
		size_t  n;

		while ((n = fread(chunk, 1u, sizeof chunk, data)) > 0u)
			if (write_all(out, chunk, n) != 0)
				goto failed;
		if (ferror(data))
			goto failed;
	}
	result = 0;
	goto done;
failed:
	say(why, cap, "the pack could not be written", strerror(errno));
done:
	if (data != NULL)
		fclose(data);
	ZSTD_freeCCtx(cctx);
	free(frame);
	free(objects);
	free(base_paths);
	free(new_paths);
	free(base);
	free(manifest);
	return result;
}

/* `length` compressed bytes of `in`, one zstd frame, into `out`
 * decompressed: exactly `expected` bytes, or -1. */
static int decompress_into(ZSTD_DCtx *dctx, int in, uint64_t length, uint64_t expected, int out)
{
	static uint8_t from[1 << 17];
	static uint8_t to[1 << 17];
	uint64_t       left = length;
	uint64_t       made = 0u;
	size_t         last = 1u;

	ZSTD_DCtx_reset(dctx, ZSTD_reset_session_only);
	while (left > 0u) {
		size_t        n = left < sizeof from ? (size_t)left : sizeof from;
		ZSTD_inBuffer input;

		if (read_all(in, from, n) != 0)
			return -1;
		left -= n;
		input = (ZSTD_inBuffer){ from, n, 0u };
		while (input.pos < input.size) {
			ZSTD_outBuffer output = { to, sizeof to, 0u };

			last = ZSTD_decompressStream(dctx, &output, &input);
			if (ZSTD_isError(last) || made + output.pos > expected ||
			    write_all(out, to, output.pos) != 0)
				return -1;
			made += output.pos;
		}
	}
	return last == 0u && made == expected ? 0 : -1;
}

/* As decompress_into, for a frame whose length is not known before: at
 * most `most` bytes, how many into `*got`. */
static int decompress_up_to(ZSTD_DCtx *dctx, int in, uint64_t length, uint64_t most, int out,
                            uint64_t *got)
{
	static uint8_t from[1 << 17];
	static uint8_t to[1 << 17];
	uint64_t       left = length;
	size_t         last = 1u;

	*got = 0u;
	ZSTD_DCtx_reset(dctx, ZSTD_reset_session_only);
	while (left > 0u) {
		size_t        n = left < sizeof from ? (size_t)left : sizeof from;
		ZSTD_inBuffer input;

		if (read_all(in, from, n) != 0)
			return -1;
		left -= n;
		input = (ZSTD_inBuffer){ from, n, 0u };
		while (input.pos < input.size) {
			ZSTD_outBuffer output = { to, sizeof to, 0u };

			last = ZSTD_decompressStream(dctx, &output, &input);
			if (ZSTD_isError(last) || *got + output.pos > most ||
			    write_all(out, to, output.pos) != 0)
				return -1;
			*got += output.pos;
		}
	}
	return last == 0u ? 0 : -1;
}

int hull_store_unpack(hull_store *store, int in, const char *ref, char hex[65], char *why,
                      size_t cap)
{
	uint8_t     head[8 + 4 + 4 + 8 + 8];
	uint32_t    count;
	uint64_t    manifest_length;
	uint64_t    frame_length;
	object     *objects = NULL;
	uint8_t    *frame = NULL;
	uint8_t    *manifest = NULL;
	ZSTD_DCtx  *dctx = NULL;
	FILE       *made = NULL;
	uint64_t    at = 0u;
	int         result = -1;

	if (read_all(in, head, sizeof head) != 0 || memcmp(head, PACK_MAGIC, 8u) != 0)
		return say(why, cap, "it is not a Hull pack", NULL);
	if (head[8] != 0u || head[9] != 0u || head[10] != 0u ||
	    (head[11] != PACK_VERSION && head[11] != PACK_DELTAS))
		return say(why, cap, "a pack of a version this hull does not read", NULL);
	count = (uint32_t)head[12] << 24 | (uint32_t)head[13] << 16 | (uint32_t)head[14] << 8 |
	        head[15];
	manifest_length = get64(head + 16);
	frame_length    = get64(head + 24);
	if (count > MAX_OBJECTS || manifest_length > (256u << 20) || frame_length > (256u << 20))
		return say(why, cap, "the pack's header is not one hull reads", NULL);
	if ((objects = calloc(count > 0u ? count : 1u, sizeof *objects)) == NULL ||
	    (frame = malloc(frame_length > 0u ? frame_length : 1u)) == NULL ||
	    (manifest = malloc(manifest_length > 0u ? manifest_length : 1u)) == NULL ||
	    (dctx = ZSTD_createDCtx()) == NULL) {
		say(why, cap, "out of memory", NULL);
		goto done;
	}
	for (uint32_t i = 0u; i < count; i++) {
		uint8_t entry[INDEX_ENTRY];

		if (read_all(in, entry, sizeof entry) != 0) {
			say(why, cap, "the pack ends in its index", NULL);
			goto done;
		}
		memcpy(objects[i].digest, entry, 32u);
		objects[i].flags  = entry[32];
		objects[i].length = get64(entry + 33);
		objects[i].offset = get64(entry + 41);
		objects[i].frame  = get64(entry + 49);
		/* In order, each where the last ended: the data is read once,
		 * from the front, which a pipe can give. */
		if (objects[i].offset != at || (i > 0u && memcmp(objects[i - 1u].digest,
		                                                 objects[i].digest, 32u) >= 0)) {
			say(why, cap, "the pack's index is not in its order", NULL);
			goto done;
		}
		at += objects[i].frame;
	}
	if (read_all(in, frame, frame_length) != 0) {
		say(why, cap, "the pack ends in its manifest", NULL);
		goto done;
	}
	{
		size_t got = ZSTD_decompressDCtx(dctx, manifest, manifest_length, frame, frame_length);

		if (ZSTD_isError(got) || got != manifest_length) {
			say(why, cap, "the pack's manifest does not decompress whole", NULL);
			goto done;
		}
	}
	/* Each object through a file of its own, into the store, checked
	 * against its digest there. */
	for (uint32_t i = 0u; i < count; i++) {
		/* A recipe (Appendix C): decompressed, then applied to the base
		 * object it names into the object's own file. A recipe is never
		 * longer than its object, which the writer holds it to. */
		if ((objects[i].flags & FLAG_DELTA) != 0u) {
			FILE    *recipe = head[11] == PACK_DELTAS ? store_temp(store) : NULL;
			uint8_t *bytes = MAP_FAILED;
			uint64_t got = 0u;
			int      said = 0;

			if (recipe != NULL &&
			    decompress_up_to(dctx, in, objects[i].frame, objects[i].length, fileno(recipe),
			                     &got) == 0 && got > 0u &&
			    (bytes = mmap(NULL, (size_t)got, PROT_READ, MAP_PRIVATE, fileno(recipe), 0)) !=
			        MAP_FAILED && (made = store_temp(store)) != NULL)
				said = apply_recipe(store, bytes, (size_t)got, fileno(made), why, cap) == 0 ? 2 : 1;
			if (said != 2) {
				if (said == 0)
					say(why, cap, "a recipe of the pack is cut or not what its index says", NULL);
				if (bytes != MAP_FAILED)
					munmap(bytes, (size_t)got);
				if (recipe != NULL)
					fclose(recipe);
				goto done;
			}
			munmap(bytes, (size_t)got);
			fclose(recipe);
		} else if ((made = store_temp(store)) == NULL ||
		    decompress_into(dctx, in, objects[i].frame, objects[i].length, fileno(made)) != 0) {
			say(why, cap, "an object of the pack is cut or not what its index says", NULL);
			goto done;
		}
		if (lseek(fileno(made), 0, SEEK_SET) != 0 ||
		    hull_store_object_put(store, fileno(made), objects[i].length, objects[i].flags & 1u,
		                          objects[i].digest, why, cap) != 0)
			goto done;
		fclose(made);
		made = NULL;
	}
	/* The manifest last, once every object it names is in: each it names
	 * among the pack's, or, for a pack made against a base, already in
	 * the store. */
	{
		object  *named = NULL;
		uint32_t named_count = 0u;
		int      all = 1;

		if (objects_of(manifest, manifest_length, &named, &named_count, why, cap) != 0)
			goto done;
		for (uint32_t i = 0u; i < named_count && all; i++) {
			int executable;
			int fd;

			if (count > 0u &&
			    bsearch(&named[i], objects, count, sizeof *objects, by_digest) != NULL)
				continue;
			if ((fd = hull_store_object_open(store, named[i].digest, &executable)) >= 0)
				close(fd);
			else
				all = 0;
		}
		free(named);
		if (!all) {
			say(why, cap, "the pack's manifest names an object neither the pack nor this store "
			    "holds: a pack made against a base wants that base unpacked first", NULL);
			goto done;
		}
	}
	result = hull_store_manifest_put(store, manifest, manifest_length, ref, hex, why, cap);
done:
	if (made != NULL)
		fclose(made);
	ZSTD_freeDCtx(dctx);
	free(manifest);
	free(frame);
	free(objects);
	return result;
}

int hull_pack_inspect(int in, FILE *out, char *why, size_t cap)
{
	uint8_t     head[8 + 4 + 4 + 8 + 8];
	uint8_t     entry[INDEX_ENTRY];
	uint32_t    count;
	uint64_t    manifest_length;
	uint64_t    frame_length;
	uint64_t    data = 0u;
	uint64_t    total = 0u;
	uint64_t    end = 0u;
	uint8_t    *frame = NULL;
	uint8_t    *manifest = NULL;
	uint8_t     digest[32];
	char        hex[65];
	hull_sha256 h;
	ssize_t     got;
	uint8_t     buffer[65536];
	int         result = -1;

	if (read_all(in, head, sizeof head) != 0 || memcmp(head, PACK_MAGIC, 8u) != 0)
		return say(why, cap, "it is not a Hull pack", NULL);
	count = (uint32_t)head[12] << 24 | (uint32_t)head[13] << 16 | (uint32_t)head[14] << 8 |
	        head[15];
	manifest_length = get64(head + 16);
	frame_length    = get64(head + 24);
	fprintf(out, "version %u\n", (unsigned)head[8] << 24 | (unsigned)head[9] << 16 |
	        (unsigned)head[10] << 8 | head[11]);
	if (head[8] != 0u || head[9] != 0u || head[10] != 0u ||
	    (head[11] != PACK_VERSION && head[11] != PACK_DELTAS))
		return say(why, cap, "a pack of a version this hull does not read", NULL);
	if (count > MAX_OBJECTS || manifest_length > (256u << 20) || frame_length > (256u << 20))
		return say(why, cap, "the pack's header is not one hull reads", NULL);
	fprintf(out, "objects %u\n", count);
	for (uint32_t i = 0u; i < count; i++) {
		uint64_t length;
		uint64_t offset;
		uint64_t compressed;

		if (read_all(in, entry, sizeof entry) != 0)
			return say(why, cap, "the pack ends in its index", NULL);
		hull_sha256_hex(entry, hex);
		length     = get64(entry + 33);
		offset     = get64(entry + 41);
		compressed = get64(entry + 49);
		fprintf(out, "object sha256:%s %s%s %llu bytes, frame %llu at %llu\n", hex,
		        entry[32] & 1u ? "executable" : "plain",
		        entry[32] & FLAG_DELTA ? ", a recipe against the base's" : "",
		        (unsigned long long)length,
		        (unsigned long long)compressed, (unsigned long long)offset);
		total += length;
		if (offset + compressed > end)
			end = offset + compressed;
	}
	if ((frame = malloc(frame_length > 0u ? frame_length : 1u)) == NULL ||
	    (manifest = malloc(manifest_length > 0u ? manifest_length : 1u)) == NULL) {
		say(why, cap, "out of memory", NULL);
		goto done;
	}
	if (read_all(in, frame, frame_length) != 0) {
		say(why, cap, "the pack ends in its manifest", NULL);
		goto done;
	}
	if (ZSTD_decompress(manifest, manifest_length, frame, frame_length) != manifest_length) {
		say(why, cap, "the pack's manifest does not decompress to its length", NULL);
		goto done;
	}
	while ((got = read(in, buffer, sizeof buffer)) > 0 || (got < 0 && errno == EINTR))
		if (got > 0)
			data += (uint64_t)got;
	hull_sha256_init(&h);
	hull_sha256_update(&h, manifest, manifest_length);
	hull_sha256_final(&h, digest);
	hull_sha256_hex(digest, hex);
	fprintf(out, "manifest sha256:%s %llu bytes, frame %llu\n", hex,
	        (unsigned long long)manifest_length, (unsigned long long)frame_length);
	fprintf(out, "data %llu bytes compressed of %llu, %s\n", (unsigned long long)data,
	        (unsigned long long)total, data == end ? "as the index says" :
	        "NOT as the index says: the pack is cut short or has more");
	if (hull_manifest_print(manifest, manifest_length, out) != 0) {
		say(why, cap, "the pack's manifest is not one", NULL);
		goto done;
	}
	result = data == end ? 0 : say(why, cap, "the pack's data is not where its index says", NULL);
done:
	free(frame);
	free(manifest);
	return result;
}

int hull_manifest_objects(const uint8_t *data, size_t length, uint8_t (**digests)[32],
                          uint32_t *count)
{
	object  *objects = NULL;
	uint32_t n = 0u;
	char     why[200];

	*digests = NULL;
	*count = 0u;
	if (objects_of(data, length, &objects, &n, why, sizeof why) != 0)
		return -1;
	if ((*digests = malloc((n > 0u ? n : 1u) * sizeof **digests)) == NULL) {
		free(objects);
		return -1;
	}
	for (uint32_t i = 0u; i < n; i++)
		memcpy((*digests)[i], objects[i].digest, 32u);
	*count = n;
	free(objects);
	return 0;
}

int hull_manifest_files(const uint8_t *data, size_t length, hull_manifest_file **list,
                        uint32_t *count)
{
	return paths_of(data, length, list, count, by_path);
}

const hull_manifest_file *hull_manifest_file_at(const hull_manifest_file *list, uint32_t count,
                                                const char *path, size_t length)
{
	hull_manifest_file key = { .path = path, .length = length };

	return count > 0u ? bsearch(&key, list, count, sizeof *list, by_path) : NULL;
}

int hull_pack_describe(int in, unsigned *version, char arch[64], uint8_t **runtime,
                       size_t *runtime_length, char *why, size_t cap)
{
	uint8_t         head[8 + 4 + 4 + 8 + 8];
	uint32_t        count;
	uint64_t        manifest_length;
	uint64_t        frame_length;
	uint8_t        *frame = NULL;
	uint8_t        *manifest = NULL;
	hull_sexpr_doc  doc;
	hull_sexpr_form top;
	hull_sexpr_form item;
	int             result = -1;

	snprintf(arch, 64u, "none");
	*runtime = NULL;
	*runtime_length = 0u;
	if (read_all(in, head, sizeof head) != 0 || memcmp(head, PACK_MAGIC, 8u) != 0)
		return say(why, cap, "it is not a Hull pack", NULL);
	*version = (unsigned)head[8] << 24 | (unsigned)head[9] << 16 | (unsigned)head[10] << 8 |
	           head[11];
	count = (uint32_t)head[12] << 24 | (uint32_t)head[13] << 16 | (uint32_t)head[14] << 8 |
	        head[15];
	manifest_length = get64(head + 16);
	frame_length    = get64(head + 24);
	if (count > MAX_OBJECTS || manifest_length == 0u || manifest_length > (256u << 20) ||
	    frame_length > (256u << 20))
		return say(why, cap, "the pack's header is not one hull reads", NULL);
	if (lseek(in, (off_t)(sizeof head + (uint64_t)count * INDEX_ENTRY), SEEK_SET) < 0 ||
	    (frame = malloc(frame_length > 0u ? frame_length : 1u)) == NULL ||
	    (manifest = malloc(manifest_length)) == NULL || read_all(in, frame, frame_length) != 0 ||
	    ZSTD_decompress(manifest, manifest_length, frame, frame_length) != manifest_length ||
	    hull_sexpr_open(&doc, manifest, manifest_length, HULL_SEXPR_CANONICAL, NULL) !=
	        HULL_SEXPR_OK ||
	    !hull_sexpr_first(&doc, &top) || !hull_sexpr_child(&doc, &top, &item)) {
		say(why, cap, "the pack's manifest cannot be read", NULL);
		goto done;
	}
	while (hull_sexpr_next(&doc, &item, &item)) {
		hull_sexpr_form key;
		hull_sexpr_form value;

		if (item.kind != HULL_SEXPR_LIST || !hull_sexpr_child(&doc, &item, &key))
			continue;
		if (hull_sexpr_is(&doc, &key, "arch") && hull_sexpr_next(&doc, &key, &value) &&
		    value.content_length > 0u && value.content_length < 64u) {
			memcpy(arch, doc.data + value.content, value.content_length);
			arch[value.content_length] = '\0';
		} else if (hull_sexpr_is(&doc, &key, "runtime") && *runtime == NULL) {
			if ((*runtime = malloc(item.extent)) == NULL) {
				say(why, cap, "out of memory", NULL);
				goto done;
			}
			memcpy(*runtime, doc.data + item.offset, item.extent);
			*runtime_length = item.extent;
		}
	}
	result = 0;
done:
	free(frame);
	free(manifest);
	return result;
}
