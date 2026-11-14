/*
 * lora_esp32.c -- firmware for two ESP32 boards with an SX1276 on VSPI, a
 * Heltec WiFi LoRa 32's wiring, under hull (test/board_test.sh): the
 * radio reset by GPIO 14, selected by GPIO 18, its DIO0 read on GPIO 26.
 * It says "radio VERSION" once it has read the chip, then FIRST sends
 * "ping" and the other answers what it hears with "pong", each saying
 * "got PAYLOAD" for a packet heard. Bare: SPI by the controller's user
 * command, a byte at a time as Arduino's SPI does, and DIO0 polled -- or,
 * built with IRQ, a packet waited for by DIO0's interrupt, GPIO 26 rising
 * routed by the ROM's own calls to CPU interrupt 13, as an Arduino
 * sketch's attachInterrupt() would; "heard by interrupt" the first time.
 */
#include <stdint.h>

#define REG(address) (*(volatile uint32_t *)(address))
#ifdef S3
/* An ESP32-S3 with an SX1276 on SPI2, GPSPI2, as LilyGO's T3-S3 wires one:
 * NSS 7, NRESET 8, DIO0 9; its DIO0 polled. */
#define UART        0x60000000u
#define GPIO        0x60004000u
#define SPI2        0x60024000u
#define SPI_CMD     REG(SPI2 + 0x00u)
#define SPI_USER    REG(SPI2 + 0x10u)
#define SPI_USER2   REG(SPI2 + 0x18u)
#define SPI_DLEN    REG(SPI2 + 0x1cu)
#define SPI_W0      REG(SPI2 + 0x98u)
#define SPI_USR     (1u << 24)
#else
#define UART        0x3ff40000u
#define GPIO        0x3ff44000u
#define SPI3        0x3ff65000u
#endif
#define UART_FIFO   REG(UART + 0x00u)
#define UART_STATUS REG(UART + 0x1cu)
#define GPIO_W1TS   REG(GPIO + 0x08u)
#define GPIO_W1TC   REG(GPIO + 0x0cu)
#define GPIO_EN_SET REG(GPIO + 0x24u)
#define GPIO_IN     REG(GPIO + 0x3cu)
#ifndef S3
#define SPI_CMD     REG(SPI3 + 0x00u)
#define SPI_USER    REG(SPI3 + 0x1cu)
#define SPI_USER2   REG(SPI3 + 0x24u)
#define SPI_MOSI    REG(SPI3 + 0x28u)
#define SPI_MISO    REG(SPI3 + 0x2cu)
#define SPI_W0      REG(SPI3 + 0x80u)
#define SPI_USR     (1u << 18)
#endif

#define GPIO_W1TC_STATUS REG(GPIO + 0x4cu)
#define GPIO_PCPU_INT    REG(GPIO + 0x68u)
#define GPIO_PIN26       REG(GPIO + 0x88u + 4u * 26u)

/* The ESP32 ROM's interrupt calls, by their addresses in ESP-IDF's
 * esp32.rom.ld. */
#define ets_isr_attach  ((void (*)(int, void (*)(void *), void *))0x400067ecu)
#define intr_matrix_set ((void (*)(int, unsigned, unsigned))0x4000681cu)
#define xtos_ints_on    ((unsigned (*)(unsigned))0x4000bf88u)
#define GPIO_SOURCE     22u   /* ETS_GPIO_INTR_SOURCE */
#define CPU_INTERRUPT   13u   /* a level-1 interrupt the ROM leaves free */

#ifdef S3
#define PIN_NSS   7u
#define PIN_RESET 8u
#define PIN_DIO0  9u
#else
#define PIN_NSS   18u
#define PIN_RESET 14u
#define PIN_DIO0  26u
#endif

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

#ifdef IRQ
static volatile unsigned fired;

/* DIO0 rose: its status cleared, and counted. */
static void dio0_rose(void *arg)
{
	(void)arg;
	GPIO_W1TC_STATUS = GPIO_PCPU_INT;
	fired++;
}
#endif

static uint8_t spi(uint8_t value)
{
	SPI_W0 = value;
#ifdef S3
	SPI_DLEN = 7u;                             /* eight bits */
#else
	SPI_MOSI = 7u;
	SPI_MISO = 7u;
#endif
	SPI_USER = (1u << 27) | (1u << 28) | 1u;   /* MOSI, MISO, full duplex */
	SPI_CMD = SPI_USR;
	while (SPI_CMD & SPI_USR)
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
#ifdef IRQ
	int      by_interrupt = 0;
#endif

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
#ifdef IRQ
	GPIO_PIN26 = (1u << 7) | (4u << 13);   /* rising, for the PRO CPU */
	ets_isr_attach((int)CPU_INTERRUPT, dio0_rose, 0);
	intr_matrix_set(0, GPIO_SOURCE, CPU_INTERRUPT);
	xtos_ints_on(1u << CPU_INTERRUPT);
#endif
	say("listening\r\n");
#ifdef FIRST
	pause(3000000u);
	send("ping");
#endif
	for (;;) {
#ifdef IRQ
		if (fired == 0u)
			continue;
		fired = 0u;
		if (!(radio_read(0x12) & 0x40u))   /* not RxDone */
			continue;
		if (!by_interrupt)
			say("heard by interrupt\r\n");
		by_interrupt = 1;
#else
		if (!(GPIO_IN & (1u << PIN_DIO0)))
			continue;
#endif
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
