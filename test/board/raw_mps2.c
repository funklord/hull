/*
 * raw_mps2.c -- firmware for an MPS2 board's AN385 (Cortex-M3) under a
 * pc machine of hull's (test/board_test.sh): "ready" on UART0, then
 * every byte UART0 receives sent back unchanged, any of the 256 values,
 * so a test can hold the console to being 8-bit clean both ways; and
 * "uart1 says hello" on UART1, the machine's second serial port, as
 * fuzznet's board says its result there, and every byte UART1 receives
 * sent back too. CMSDK UARTs: DATA, STATE (bit 0 TX full, bit 1 RX
 * full), CTRL (bit 0 TX, bit 1 RX enable).
 */
#include <stdint.h>

#define REG(address) (*(volatile uint32_t *)(address))
#define UART0         0x40004000u
#define UART1         0x40005000u
#define DATA(uart)    REG((uart) + 0x0u)
#define STATE(uart)   REG((uart) + 0x4u)
#define CTRL(uart)    REG((uart) + 0x8u)
#define BAUDDIV(uart) REG((uart) + 0x10u)

static void put(uint32_t uart, uint8_t c)
{
	while (STATE(uart) & 1u)
		;
	DATA(uart) = c;
}

static void say(uint32_t uart, const char *text)
{
	while (*text != '\0')
		put(uart, (uint8_t)*text++);
}

void reset(void);

void reset(void)
{
	BAUDDIV(UART0) = 16u;
	CTRL(UART0) = 3u;
	BAUDDIV(UART1) = 16u;
	CTRL(UART1) = 3u;
	say(UART1, "uart1 says hello\r\n");
	say(UART0, "ready\r\n");
	for (;;) {
		if (STATE(UART0) & 2u)
			put(UART0, (uint8_t)DATA(UART0));
		if (STATE(UART1) & 2u)
			put(UART1, (uint8_t)DATA(UART1));
	}
}

__attribute__((section(".vectors"), used)) void *const vectors[2] = {
	(void *)0x20002000u, (void *)reset,
};
