/*
 * echo_avr.c -- firmware for an Arduino Uno (ATmega328P) under a pc
 * machine of hull's (test/board_test.sh): "ready" on USART0, then each
 * line typed, ended by a carriage return, said back as "said LINE".
 */
#include <avr/io.h>

static void put(char c)
{
	while (!(UCSR0A & (1 << UDRE0)))
		;
	UDR0 = (uint8_t)c;
}

static void say(const char *text)
{
	while (*text != '\0')
		put(*text++);
}

int main(void)
{
	char     line[32];
	unsigned n = 0u;

	UBRR0  = 103u;  /* 9600 baud at 16 MHz, which qemu does not hold it to */
	UCSR0B = (1 << RXEN0) | (1 << TXEN0);
	say("ready\r\n");
	for (;;) {
		char c;

		while (!(UCSR0A & (1 << RXC0)))
			;
		c = (char)UDR0;
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
