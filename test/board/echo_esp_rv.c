/*
 * echo_esp_rv.c -- firmware for Espressif's RISC-V chips, the ESP32-C3,
 * C5, C6, C61, H2 and P4, under a pc machine of hull's
 * (test/board_test.sh): "ready" on UART0, then each line typed, ended by
 * a carriage return, said back as "said LINE". It is loaded by the chip's
 * ROM as its bootloader, one segment at the address it is linked for,
 * test/board/esp_image.py making the flash image. UART is UART0's
 * address, the P4's being elsewhere. Its strings are built in place, as
 * nothing but .text is loaded.
 */
#include <stdint.h>

#ifndef UART
#define UART 0x60000000u
#endif
#define REG(address) (*(volatile uint32_t *)(address))
#define UART_FIFO    REG(UART + 0x00u)
#define UART_STATUS  REG(UART + 0x1cu)   /* rx count 9:0, tx count 25:16 */

static void put(char c)
{
	while (((UART_STATUS >> 16) & 0x3ffu) >= 100u)
		;
	UART_FIFO = (uint8_t)c;
}

static void say(const char *text)
{
	while (*text != '\0')
		put(*text++);
}

void start(void);

void start(void)
{
	char     ready[] = { 'r', 'e', 'a', 'd', 'y', '\r', '\n', '\0' };
	char     said[] = { 's', 'a', 'i', 'd', ' ', '\0' };
	char     end[] = { '\r', '\n', '\0' };
	char     line[32];
	unsigned n = 0u;

	say(ready);
	for (;;) {
		char c;

		while ((UART_STATUS & 0x3ffu) == 0u)
			;
		c = (char)UART_FIFO;
		if (c == '\r') {
			line[n] = '\0';
			say(said);
			say(line);
			say(end);
			n = 0u;
		} else if (n < sizeof line - 1u) {
			line[n++] = c;
		}
	}
}
