/*
 * echo_stm32.c -- firmware for an STM32VLDISCOVERY (STM32F100, Cortex-M3)
 * under a pc machine of hull's (test/board_test.sh): "ready" on USART1,
 * then each line typed, ended by a carriage return, said back as "said
 * LINE". Its vectors are the stack and the reset handler alone.
 */
#include <stdint.h>

#define REG(address) (*(volatile uint32_t *)(address))
#define RCC_APB2ENR  REG(0x40021018u)
#define USART1_SR    REG(0x40013800u)
#define USART1_DR    REG(0x40013804u)
#define USART1_BRR   REG(0x40013808u)
#define USART1_CR1   REG(0x4001380cu)

static void put(char c)
{
	while (!(USART1_SR & (1u << 7)))   /* TXE */
		;
	USART1_DR = (uint8_t)c;
}

static void say(const char *text)
{
	while (*text != '\0')
		put(*text++);
}

void reset(void);

void reset(void)
{
	char     line[32];
	unsigned n = 0u;

	RCC_APB2ENR |= (1u << 14) | (1u << 2);   /* USART1, port A */
	USART1_BRR = 0x341u;                     /* 9600 baud at 8 MHz */
	USART1_CR1 = (1u << 13) | (1u << 3) | (1u << 2);   /* UE, TE, RE */
	say("ready\r\n");
	for (;;) {
		char c;

		while (!(USART1_SR & (1u << 5)))   /* RXNE */
			;
		c = (char)USART1_DR;
		if (c == '\r') {
			line[n] = '\0';
			say("said ");
			say(line);
			say("\r\n");
			n = 0u;
		} else if (n < sizeof line - 1u) {
			line[n++] = c;
		}
	}
}

__attribute__((section(".vectors"), used)) void *const vectors[2] = {
	(void *)0x20002000u, (void *)reset,
};
