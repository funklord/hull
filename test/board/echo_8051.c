/*
 * echo_8051.c -- firmware for an 8051 (the AT89 parts' 8052 core) under a
 * pc machine of hull's, SDCC's s51 (test/board_test.sh): "ready" on its
 * UART, then each line typed, ended by a carriage return, said back as
 * "said LINE". Timer 1 in mode 2 gives 9600 baud from 11.0592 MHz.
 */
#include <8052.h>

static void put(char c)
{
	TI = 0;
	SBUF = c;
	while (!TI)
		;
}

static void say(const char *text)
{
	while (*text != '\0')
		put(*text++);
}

void main(void)
{
	char          line[16];
	unsigned char n = 0;

	SCON = 0x50;   /* mode 1, receiver on */
	TMOD = 0x20;   /* timer 1, mode 2, auto-reload */
	TH1  = 0xfd;
	TR1  = 1;
	say("ready\r\n");
	for (;;) {
		char c;

		while (!RI)
			;
		RI = 0;
		c = SBUF;
		if (c == '\r') {
			line[n] = '\0';
			say("said ");
			say(line);
			say("\r\n");
			n = 0;
		} else if (n < sizeof line - 1) {
			line[n++] = c;
		}
	}
}
