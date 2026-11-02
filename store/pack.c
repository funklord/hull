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
#include <sys/stat.h>
#include <unistd.h>

#include <zstd.h>

#include "sexpr.h"
#include "sha256.h"
#include "store.h"

#define PACK_MAGIC   "HULLPACK"
#define PACK_VERSION 1u
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

int hull_store_pack(hull_store *store, const char *name, int out, char hex[65], char *why,
                    size_t cap)
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

	if (hull_store_manifest_of(store, name, hex, &manifest, &manifest_length, why, cap) != 0)
		return -1;
	if (objects_of(manifest, manifest_length, &objects, &count, why, cap) != 0)
		goto done;
	if ((cctx = ZSTD_createCCtx()) == NULL ||
	    ZSTD_isError(ZSTD_CCtx_setParameter(cctx, ZSTD_c_compressionLevel, PACK_LEVEL)) ||
	    ZSTD_isError(ZSTD_CCtx_setParameter(cctx, ZSTD_c_checksumFlag, 1)) ||
	    (data = tmpfile()) == NULL) {
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
	head[8] = 0u, head[9] = 0u, head[10] = 0u, head[11] = (uint8_t)PACK_VERSION;
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
	if (head[8] != 0u || head[9] != 0u || head[10] != 0u || head[11] != PACK_VERSION)
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
		if ((made = tmpfile()) == NULL ||
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
	/* The manifest last, once every object it names is in, and each it
	 * names among the pack's. */
	{
		object  *named = NULL;
		uint32_t named_count = 0u;
		int      all = 1;

		if (objects_of(manifest, manifest_length, &named, &named_count, why, cap) != 0)
			goto done;
		for (uint32_t i = 0u; i < named_count && all; i++)
			all = bsearch(&named[i], objects, count, sizeof *objects, by_digest) != NULL;
		free(named);
		if (!all) {
			say(why, cap, "the pack's manifest names an object the pack does not hold", NULL);
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
