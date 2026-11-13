/*
 * esp_fetch.c -- `hull esp fetch` (project.md section 5.0): Espressif's
 * qemu for the host, its xtensa and riscv32 builds, pinned in engine/esp.h,
 * downloaded over https, each checked against its sha256 and unpacked as
 * the user into $XDG_CACHE_HOME/hull/esp-qemu-VERSION, where an ESP32
 * board's machine runs it from. Built beside where it goes and renamed
 * in, so a fetch cut short leaves nothing that looks whole.
 */

#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <unistd.h>

#include "esp.h"
#include "esp_fetch.h"
#include "kernel_fetch.h"
#include "store.h"

int hull_esp_fetch(char *why, size_t cap)
{
	static const char *const made_by[] = { "qemu/bin/qemu-system-xtensa",
	                                       "qemu/bin/qemu-system-riscv32" };
	struct utsname host;
	char           dir[4096];
	char           work[4200];
	char           file[4300], tar[4300], tree[4300], made[4400], url[1024];
	uint64_t       skipped = 0u;
	hull_unpack   *u = NULL;
	unsigned       fetched = 0u;
	int            status = -1;

	if (uname(&host) != 0) {
		snprintf(why, cap, "the host's machine cannot be read");
		return -1;
	}
	if (hull_esp_dir(dir, sizeof dir) != 0) {
		snprintf(why, cap, "neither XDG_CACHE_HOME nor HOME names a place for Espressif's qemu");
		return -1;
	}
	snprintf(made, sizeof made, "%s/%s", dir, made_by[1]);
	if (access(made, X_OK) == 0) {
		printf("%s\n", dir);
		return 0;
	}
	for (char *slash = strchr(dir + 1, '/'); slash != NULL; slash = strchr(slash + 1, '/')) {
		*slash = '\0';
		if (mkdir(dir, 0755) != 0 && errno != EEXIST) {
			snprintf(why, cap, "%s: %s", dir, strerror(errno));
			return -1;
		}
		*slash = '/';
	}
	snprintf(work, sizeof work, "%s.part-XXXXXX", dir);
	if (mkdtemp(work) == NULL) {
		snprintf(why, cap, "%s: %s", work, strerror(errno));
		return -1;
	}
	snprintf(file, sizeof file, "%s/release.tar.xz", work);
	snprintf(tar, sizeof tar, "%s/release.tar", work);
	snprintf(tree, sizeof tree, "%s/tree", work);
	if (mkdir(tree, 0755) != 0) {
		snprintf(why, cap, "%s: %s", tree, strerror(errno));
		goto done;
	}
	if ((u = hull_unpack_begin(tree, why, cap)) == NULL)
		goto done;
	/* Both emulators, one tarball each, into the one tree: they share
	 * qemu/share's files, the same bytes in each. */
	for (unsigned i = 0u; i < sizeof hull_esp_pins / sizeof hull_esp_pins[0]; i++) {
		const hull_esp_pin *pin = &hull_esp_pins[i];
		int                 fd;
		int                 applied;

		if (strcmp(pin->host, host.machine) != 0)
			continue;
		snprintf(url, sizeof url, "%s%s", HULL_ESP_URL, pin->file);
		fprintf(stderr, "hull: fetching Espressif's qemu, about 17 MB: %s\n", url);
		if (hull_fetch_tar_xz(url, pin->sha256, file, tar, why, cap) != 0)
			goto done;
		if ((fd = open(tar, O_RDONLY | O_CLOEXEC)) < 0) {
			snprintf(why, cap, "%s: %s", tar, strerror(errno));
			goto done;
		}
		applied = hull_unpack_layer(u, fd, &skipped, why, cap);
		close(fd);
		unlink(tar);
		if (applied != 0)
			goto done;
		fetched++;
	}
	if (fetched == 0u) {
		snprintf(why, cap, "Espressif builds its qemu for x86_64 and aarch64 Linux hosts, "
		         "not %s", host.machine);
		goto done;
	}
	{
		hull_unpack *ending = u;

		u = NULL;
		if (hull_unpack_end(ending, why, cap) != 0)
			goto done;
	}
	for (unsigned i = 0u; i < sizeof made_by / sizeof made_by[0]; i++) {
		snprintf(made, sizeof made, "%s/%s", tree, made_by[i]);
		if (access(made, X_OK) != 0) {
			snprintf(why, cap, "the release holds no %s", made_by[i]);
			goto done;
		}
	}
	if (rename(tree, dir) != 0) {
		snprintf(why, cap, "%s: %s", dir, strerror(errno));
		goto done;
	}
	printf("%s\n", dir);
	status = 0;
done:
	if (u != NULL) {
		char ignored[200];

		hull_unpack_end(u, ignored, sizeof ignored);
	}
	unlink(file);
	unlink(tar);
	{
		int parent = open(work, O_RDONLY | O_DIRECTORY | O_CLOEXEC);

		if (parent >= 0) {
			hull_store_remove_tree(parent, "tree");
			close(parent);
		}
	}
	rmdir(work);
	return status;
}
