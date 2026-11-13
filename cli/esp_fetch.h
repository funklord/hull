/*
 * esp_fetch.h -- `hull esp fetch`: Espressif's qemu, engine/esp.h's pins,
 * downloaded once and unpacked into the user's cache.
 */

#ifndef HULL_ESP_FETCH_H
#define HULL_ESP_FETCH_H

#include <stddef.h>

/* Fetch and unpack it where it is not yet, printing where it is. 0, or
 * -1 with `why`. */
int hull_esp_fetch(char *why, size_t cap);

#endif
