/*
 * iso_test.c -- engine/iso.c: a tree of the test's own beneath $BUILD_DIR
 * written as an ISO9660 image, twice to the same bytes, and read back
 * through engine/disc.situ's validators -- the volume descriptor and its
 * terminator, both path tables, every directory record and every file's
 * bytes -- as an emulator would read it; and the trees it refuses.
 */

#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <ftw.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "disc.h"
#include "iso.h"

#define SECTOR 2048u

static unsigned cases;
static unsigned failures;

static void check(const char *what, int ok, const char *detail)
{
	cases++;
	if (!ok) {
		failures++;
		printf("FAIL %s: %s\n", what, detail != NULL ? detail : "no");
	}
}

static void put(const char *root, const char *path, const char *text, size_t length)
{
	char  whole[600];
	FILE *f;

	snprintf(whole, sizeof whole, "%s/%s", root, path);
	if ((f = fopen(whole, "w")) == NULL || fwrite(text, 1u, length, f) != length || fclose(f) != 0)
		printf("FAIL writing %s: %s\n", whole, strerror(errno));
}

static void made(const char *root, const char *path)
{
	char whole[600];

	snprintf(whole, sizeof whole, "%s/%s", root, path);
	mkdir(whole, 0755);
}

/* `root` written as an image into `out`, `*length` its bytes: 0, or -1
 * with why in `detail`. */
static int image_of(const char *root, const char *volume, const char *path, uint8_t **out,
                    size_t *length, char *detail, size_t cap)
{
	int         fd = open(path, O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
	struct stat st;

	if (fd < 0)
		return snprintf(detail, cap, "%s: %s", path, strerror(errno)), -1;
	if (hull_iso_write(root, volume, fd, detail, cap) != 0 || fstat(fd, &st) != 0) {
		close(fd);
		return -1;
	}
	*length = (size_t)st.st_size;
	if ((*out = malloc(*length)) == NULL || pread(fd, *out, *length, 0) != (ssize_t)*length) {
		close(fd);
		return snprintf(detail, cap, "reading it back"), -1;
	}
	close(fd);
	return 0;
}

/* The records of the directory at `extent`, `size` bytes, each checked by
 * the schema; its entries' identifiers joined into `names`, and each
 * file's bytes compared with `host`'s. The entries, or -1. */
static int walk(const situ_msg_t *msg, const uint8_t *image, uint32_t extent, uint32_t size,
                const char *host, char *names, size_t cap)
{
	uint32_t at = extent * SECTOR;
	uint32_t end = at + size;
	int      count = 0;

	names[0] = '\0';
	while (at < end) {
		situ_view_t view;
		uint32_t    length = image[at];
		char        id[40];
		uint32_t    id_length;

		if (length == 0u) {
			at = (at / SECTOR + 1u) * SECTOR;   /* the rest of a sector is padding */
			continue;
		}
		if (hull_disc_directory_record_view(msg, at, length, &view) != SITU_OK ||
		    hull_disc_directory_record_validate(view) != SITU_OK)
			return -1;
		id_length = hull_disc_directory_record_id_length_get(view);
		memcpy(id, hull_disc_directory_record_id_ptr(view), id_length);
		id[id_length] = '\0';
		if (count >= 2) {
			/* Not . and .. : the entry, and a file's bytes against the host's. */
			char     path[700];
			size_t   n = strlen(names);
			uint32_t data = hull_disc_directory_record_extent_get(view) * SECTOR;
			uint32_t bytes = hull_disc_directory_record_size_get(view);

			snprintf(names + n, cap - n, "%s%s", n != 0u ? " " : "", id);
			if (strchr(id, ';') != NULL) {
				FILE    *f;
				uint8_t *want = malloc(bytes + 1u);
				size_t   got = 0u;

				*strchr(id, ';') = '\0';
				if (id[strlen(id) - 1u] == '.')
					id[strlen(id) - 1u] = '\0';
				snprintf(path, sizeof path, "%s/%s", host, id);
				if ((f = fopen(path, "r")) == NULL) {
					/* The host's name may be the lower case DOS reads as this. */
					for (char *c = path + strlen(host) + 1u; *c != '\0'; c++)
						*c = (char)(*c >= 'A' && *c <= 'Z' ? *c + 32 : *c);
					f = fopen(path, "r");
				}
				if (want == NULL || f == NULL) {
					if (f != NULL)
						fclose(f);
					free(want);
					return -1;
				}
				got = fread(want, 1u, bytes + 1u, f);
				fclose(f);
				if (got != bytes || memcmp(want, image + data, bytes) != 0) {
					free(want);
					return -1;
				}
				free(want);
			}
		}
		count++;
		at += length;
	}
	return count;
}

static int remove_one(const char *path, const struct stat *st, int kind, struct FTW *ftw)
{
	(void)st;
	(void)ftw;
	if (kind == FTW_DP)
		rmdir(path);
	else
		unlink(path);
	return 0;
}

int main(void)
{
	const char *build = getenv("BUILD_DIR");
	char        root[512];
	char        tree[560];
	char        path[600];
	char        again_path[600];
	char        detail[512];
	uint8_t    *image = NULL;
	uint8_t    *again = NULL;
	size_t      length = 0u;
	size_t      again_length = 0u;
	char        blob[5000];

	if (build == NULL || build[0] == '\0') {
		printf("iso: BUILD_DIR is not set\n");
		return 2;
	}
	snprintf(root, sizeof root, "%s/test/iso", build);
	nftw(root, remove_one, 16, FTW_DEPTH | FTW_PHYS | FTW_MOUNT);
	mkdir(root, 0700);
	snprintf(tree, sizeof tree, "%s/tree", root);
	mkdir(tree, 0755);
	made(tree, "DATA");
	made(tree, "DATA/DEEP");
	made(tree, "EMPTY");
	put(tree, "readme.txt", "on a disc\r\n", 11u);
	for (size_t i = 0u; i < sizeof blob; i++)
		blob[i] = (char)(i * 7u + 3u);
	put(tree, "DATA/BLOB.BIN", blob, sizeof blob);
	put(tree, "DATA/ZERO", "", 0u);
	/* Enough entries that DEEP's records cross a sector. */
	for (unsigned i = 0u; i < 90u; i++) {
		char name[32];

		snprintf(name, sizeof name, "DATA/DEEP/F%u.TXT", i);
		put(tree, name, name, strlen(name));
	}

	snprintf(path, sizeof path, "%s/one.iso", root);
	snprintf(again_path, sizeof again_path, "%s/two.iso", root);
	check("a tree written as an image",
	      image_of(tree, "hulldisc", path, &image, &length, detail, sizeof detail) == 0, detail);
	check("written again, the same bytes",
	      image_of(tree, "hulldisc", again_path, &again, &again_length, detail,
	               sizeof detail) == 0 && again_length == length &&
	      memcmp(image, again, length) == 0, detail);
	if (image != NULL && length >= 18u * SECTOR) {
		situ_msg_t  msg;
		situ_view_t pvd;
		situ_view_t end;
		situ_view_t root_record;
		situ_view_t entry;
		char        names[4096];
		uint32_t    root_extent = 0u;
		uint32_t    root_size = 0u;
		uint32_t    table;
		int         entries;

		situ_msg_init(&msg, image, (uint32_t)length);
		check("its volume descriptor, as the schema has one",
		      hull_disc_primary_descriptor_view(&msg, 16u * SECTOR, &pvd) == SITU_OK &&
		      hull_disc_primary_descriptor_validate(pvd) == SITU_OK &&
		      hull_disc_primary_descriptor_space_get(pvd) * SECTOR == length &&
		      memcmp(hull_disc_primary_descriptor_volume_id_ptr(pvd), "HULLDISC ", 9u) == 0, NULL);
		check("its terminator",
		      hull_disc_terminator_view(&msg, 17u * SECTOR, &end) == SITU_OK &&
		      hull_disc_terminator_validate(end) == SITU_OK, NULL);
		if (hull_disc_primary_descriptor_root_view(pvd, &root_record) == SITU_OK) {
			root_extent = hull_disc_root_record_extent_get(root_record);
			root_size = hull_disc_root_record_size_get(root_record);
		}
		table = hull_disc_primary_descriptor_l_path_table_get(pvd) * SECTOR;
		check("the L path table's root, its own parent",
		      hull_disc_path_entry_view(&msg, table, 10u, &entry) == SITU_OK &&
		      hull_disc_path_entry_validate(entry) == SITU_OK &&
		      hull_disc_path_entry_extent_get(entry) == root_extent &&
		      hull_disc_path_entry_parent_get(entry) == 1u, NULL);
		table = hull_disc_primary_descriptor_m_path_table_get(pvd) * SECTOR;
		check("the M path table's root, big-endian",
		      hull_disc_path_entry_big_view(&msg, table, 10u, &entry) == SITU_OK &&
		      hull_disc_path_entry_big_validate(entry) == SITU_OK &&
		      hull_disc_path_entry_big_extent_get(entry) == root_extent, NULL);
		entries = walk(&msg, image, root_extent, root_size, tree, names, sizeof names);
		check("the root's records, sorted, the names DOS's in capitals",
		      entries == 5 && strcmp(names, "DATA EMPTY README.TXT;1") == 0, names);
		{
			/* DATA, then DATA/DEEP, each found by its record in its parent. */
			uint32_t at = root_extent * SECTOR + 68u;
			situ_view_t data;
			char        deep_host[600];

			if (hull_disc_directory_record_view(&msg, at, image[at], &data) == SITU_OK) {
				char data_host[600];

				snprintf(data_host, sizeof data_host, "%s/DATA", tree);
				entries = walk(&msg, image, hull_disc_directory_record_extent_get(data),
				               hull_disc_directory_record_size_get(data), data_host, names,
				               sizeof names);
				check("a subdirectory's files, their bytes the host's",
				      entries == 5 && strcmp(names, "BLOB.BIN;1 DEEP ZERO.;1") == 0, names);
				at = hull_disc_directory_record_extent_get(data) * SECTOR + 68u + 44u;
				snprintf(deep_host, sizeof deep_host, "%s/DATA/DEEP", tree);
				if (hull_disc_directory_record_view(&msg, at, image[at], &data) == SITU_OK) {
					entries = walk(&msg, image, hull_disc_directory_record_extent_get(data),
					               hull_disc_directory_record_size_get(data), deep_host,
					               names, sizeof names);
					check("a directory past one sector, every record whole",
					      entries == 92 && hull_disc_directory_record_size_get(data) > SECTOR,
					      names);
				}
			}
		}
	}
	free(image);
	free(again);

	/* What a disc cannot hold, each refused and named. */
	{
		static const struct { const char *name; const char *says; } bad[] = {
			{ "long-name.txt", "DOS's 8.3" },
			{ "toolongname.txt", "DOS's 8.3" },
			{ "two.dots.txt", "DOS's 8.3" },
		};

		for (unsigned i = 0u; i < sizeof bad / sizeof bad[0]; i++) {
			char refused[600];

			snprintf(refused, sizeof refused, "%s/bad%u", root, i);
			mkdir(refused, 0755);
			put(refused, bad[i].name, "x", 1u);
			image = NULL;
			check(bad[i].name, image_of(refused, "", path, &image, &length, detail,
			                            sizeof detail) != 0 &&
			      strstr(detail, bad[i].says) != NULL, detail);
			free(image);
		}
		snprintf(path, sizeof path, "%s/badlink", root);
		mkdir(path, 0755);
		snprintf(again_path, sizeof again_path, "%s/badlink/LINK", root);
		if (symlink("/etc/passwd", again_path) == 0) {
			image = NULL;
			snprintf(again_path, sizeof again_path, "%s/link.iso", root);
			check("a link refused, not followed",
			      image_of(path, "", again_path, &image, &length, detail, sizeof detail) != 0 &&
			      strstr(detail, "not a link") != NULL, detail);
			free(image);
		}
		snprintf(path, sizeof path, "%s/badcase", root);
		mkdir(path, 0755);
		put(path, "A.TXT", "x", 1u);
		put(path, "a.txt", "y", 1u);
		image = NULL;
		snprintf(again_path, sizeof again_path, "%s/case.iso", root);
		check("two names one on a disc, which knows no case",
		      image_of(path, "", again_path, &image, &length, detail, sizeof detail) != 0 &&
		      strstr(detail, "knows no case") != NULL, detail);
		free(image);
		image = NULL;
		check("a label that is not d-characters",
		      image_of(tree, "my disc", again_path, &image, &length, detail, sizeof detail) != 0 &&
		      strstr(detail, "volume label") != NULL, detail);
		free(image);
	}

	nftw(root, remove_one, 16, FTW_DEPTH | FTW_PHYS | FTW_MOUNT);
	printf("iso: %u cases, %u failed\n", cases, failures);
	return failures != 0u;
}
