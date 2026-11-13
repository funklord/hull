/*
 * lora_esp32.c -- firmware for two ESP32 boards with an SX1276 on VSPI, a
 * Heltec WiFi LoRa 32's wiring, under hull (test/board_test.sh): the
 * radio reset by GPIO 14, selected by GPIO 18, its DIO0 read on GPIO 26.
 * It says "radio VERSION" once it has read the chip, then FIRST sends
 * "ping" and the other answers what it hears with "pong", each saying
 * "got PAYLOAD" for a packet heard. Bare: SPI by the controller's user
 * command, a byte at a time as Arduino's SPI does, and DIO0 polled.
 */
#include <stdint.h>

#define REG(address) (*(volatile uint32_t *)(address))
#define UART        0x3ff40000u
#define GPIO        0x3ff44000u
#define SPI3        0x3ff65000u
#define UART_FIFO   REG(UART + 0x00u)
#define UART_STATUS REG(UART + 0x1cu)
#define GPIO_W1TS   REG(GPIO + 0x08u)
#define GPIO_W1TC   REG(GPIO + 0x0cu)
#define GPIO_EN_SET REG(GPIO + 0x24u)
#define GPIO_IN     REG(GPIO + 0x3cu)
#define SPI_CMD     REG(SPI3 + 0x00u)
#define SPI_USER    REG(SPI3 + 0x1cu)
#define SPI_USER2   REG(SPI3 + 0x24u)
#define SPI_MOSI    REG(SPI3 + 0x28u)
#define SPI_MISO    REG(SPI3 + 0x2cu)
#define SPI_W0      REG(SPI3 + 0x80u)

#define PIN_NSS   18u
#define PIN_RESET 14u
#define PIN_DIO0  26u

static void put(char c)
{
	while (((UART_STATUS >> 16) & 0xffu) >= 100u)
		;
	UART_FIFO = (uint8_t)c;
}

static void say(const char *text)
{
	while (*text != '\0')
		put(*text++);
}

static void hex(uint8_t value)
{
	put("0123456789abcdef"[value >> 4]);
	put("0123456789abcdef"[value & 15u]);
}

static void pause(unsigned count)
{
	for (volatile unsigned i = 0u; i < count; i++)
		;
}

static uint8_t spi(uint8_t value)
{
	SPI_W0 = value;
	SPI_MOSI = 7u;
	SPI_MISO = 7u;
	SPI_USER = (1u << 27) | (1u << 28) | 1u;   /* MOSI, MISO, full duplex */
	SPI_CMD = 1u << 18;                        /* USR */
	while (SPI_CMD & (1u << 18))
		;
	return (uint8_t)SPI_W0;
}

static uint8_t radio_read(uint8_t address)
{
	uint8_t value;

	GPIO_W1TC = 1u << PIN_NSS;
	spi(address & 0x7fu);
	value = spi(0u);
	GPIO_W1TS = 1u << PIN_NSS;
	return value;
}

static void radio_write(uint8_t address, uint8_t value)
{
	GPIO_W1TC = 1u << PIN_NSS;
	spi(address | 0x80u);
	spi(value);
	GPIO_W1TS = 1u << PIN_NSS;
}

static void send(const char *payload)
{
	unsigned n = 0u;

	radio_write(0x01, 0x81);                   /* standby */
	radio_write(0x0d, 0x80);                   /* FIFO pointer at the TX base */
	while (payload[n] != '\0')
		radio_write(0x00, (uint8_t)payload[n++]);
	radio_write(0x22, (uint8_t)n);
	radio_write(0x40, 0x40);                   /* DIO0 on TxDone */
	radio_write(0x01, 0x83);                   /* transmit */
	while (!(GPIO_IN & (1u << PIN_DIO0)))
		;
	radio_write(0x12, 0xff);
	radio_write(0x40, 0x00);                   /* DIO0 on RxDone */
	radio_write(0x01, 0x85);                   /* receive, continuously */
}

void start(void);

void start(void)
{
	char     got[40];
	unsigned length;

	/* By hand: an initialiser would call memset, and there is no libc. */
	got[0] = 'g';
	got[1] = 'o';
	got[2] = 't';
	got[3] = ' ';
	SPI_USER2 = 0u;   /* no command phase, as Arduino's SPI starts its bus */
	GPIO_EN_SET = (1u << PIN_NSS) | (1u << PIN_RESET);
	GPIO_W1TS = 1u << PIN_NSS;
	GPIO_W1TC = 1u << PIN_RESET;
	pause(1000u);
	GPIO_W1TS = 1u << PIN_RESET;
	pause(1000u);
	say("radio ");
	hex(radio_read(0x42));
	say("\r\n");
	radio_write(0x01, 0x80);                   /* sleep, LoRa */
	radio_write(0x06, 0xd9);                   /* 868 MHz */
	radio_write(0x07, 0x00);
	radio_write(0x08, 0x00);
	radio_write(0x0e, 0x80);
	radio_write(0x0f, 0x00);
	radio_write(0x1d, 0x72);                   /* 125 kHz, 4/5, explicit */
	radio_write(0x1e, 0x74);                   /* SF7, CRC */
	radio_write(0x40, 0x00);
	radio_write(0x01, 0x85);
	say("listening\r\n");
#ifdef FIRST
	pause(3000000u);
	send("ping");
#endif
	for (;;) {
		if (!(GPIO_IN & (1u << PIN_DIO0)))
			continue;
		length = radio_read(0x13);
		radio_write(0x0d, radio_read(0x10));
		for (unsigned i = 0u; i < length && i < 32u; i++)
			got[4u + i] = (char)radio_read(0x00);
		got[4u + (length < 32u ? length : 32u)] = '\0';
		radio_write(0x12, 0xff);
		say(got);
		say("\r\n");
#ifndef FIRST
		send("pong");
#endif
	}
}
