/*
 * esp.h -- the qemu Espressif's ESP32 boards run on (project.md section
 * 5.0): Espressif's own fork, which carries the esp32 and esp32s3 machines
 * in its qemu-system-xtensa and the RISC-V ones, esp32c3 and the rest, in
 * its qemu-system-riscv32, each with the chip's mask ROM. qemu proper has
 * none of them, and the fork's binaries share qemu's names, so they are
 * never looked for on PATH: `hull esp fetch` downloads Espressif's release
 * builds for the host, pinned by sha256, into the user's cache, and a
 * board runs them from there.
 */
#ifndef HULL_ENGINE_ESP_H
#define HULL_ENGINE_ESP_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HULL_ESP_VERSION "9.2.2-20261109"
#define HULL_ESP_RELEASE "esp_develop_9.2.2_20261109"
#define HULL_ESP_URL     "https://github.com/espressif/qemu/releases/download/esp-develop-9.2.2-20261109/"

/* A release tarball for one host and one emulator, its sha256 the one
 * GitHub's release lists for it, checked 2026-11-13. */
typedef struct hull_esp_pin {
	const char *host;      /* uname's machine */
	const char *file;      /* under HULL_ESP_URL */
	const char *sha256;
} hull_esp_pin;

static const hull_esp_pin hull_esp_pins[] = {
	{ "x86_64", "qemu-xtensa-softmmu-" HULL_ESP_RELEASE "-x86_64-linux-gnu.tar.xz",
	  "fbc3bffa9fa71192847969a627a584aabe2e86d85bed4902a096ff06c2b8a815" },
	{ "x86_64", "qemu-riscv32-softmmu-" HULL_ESP_RELEASE "-x86_64-linux-gnu.tar.xz",
	  "afa59465495295b8aa4f804768983fa30b0bbf33d0f68a047f9caa436b9164c0" },
	{ "aarch64", "qemu-xtensa-softmmu-" HULL_ESP_RELEASE "-aarch64-linux-gnu.tar.xz",
	  "dad4a8f44b5649b1f110963d479a77edb0ad2484cde79403e3a19c50efdd5281" },
	{ "aarch64", "qemu-riscv32-softmmu-" HULL_ESP_RELEASE "-aarch64-linux-gnu.tar.xz",
	  "4038e03d6e312b2372106516855a2c72eda02b49f6480917c55e56e04d958781" },
};

/* Where it is kept, $XDG_CACHE_HOME/hull/esp-qemu-VERSION, into `out`;
 * the emulators are its qemu/bin/. 0, or -1 where no cache is named. */
static inline int hull_esp_dir(char *out, size_t cap)
{
	const char *cache = getenv("XDG_CACHE_HOME");
	const char *home = getenv("HOME");

	if (cache != NULL && cache[0] == '/')
		snprintf(out, cap, "%s/hull/esp-qemu-%s", cache, HULL_ESP_VERSION);
	else if (home != NULL && home[0] == '/')
		snprintf(out, cap, "%s/.cache/hull/esp-qemu-%s", home, HULL_ESP_VERSION);
	else
		return -1;
	return 0;
}

#endif /* HULL_ENGINE_ESP_H */
