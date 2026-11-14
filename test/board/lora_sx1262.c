/*
 * lora_sx1262.c -- firmware for a Heltec WiFi LoRa 32 V3, an ESP32-S3 with
 * an SX1262 on SPI2, under hull (test/board_test.sh): NSS GPIO 8, NRESET
 * 12, BUSY 13, DIO1 14. It reads the chip's version string and says
 * "radio" and its first six characters, sets LoRa at 868 MHz, SF7,
 * 125 kHz, 4/5, CRC on, the private sync word -- what the SX1276 boards'
 * firmware (lora_esp32.c) uses -- and listens; each packet heard it says
 * "got PAYLOAD" and answers "pong". Bare: SPI by GPSPI2's user command a
 * byte at a time, NSS by GPIO, DIO1 polled.
 */
#include <stdint.h>

#define REG(address) (*(volatile uint32_t *)(address))
#define UART        0x60000000u
#define GPIO        0x60004000u
#define SPI2        0x60024000u
#define UART_FIFO   REG(UART + 0x00u)
#define UART_STATUS REG(UART + 0x1cu)
#define GPIO_W1TS   REG(GPIO + 0x08u)
#define GPIO_W1TC   REG(GPIO + 0x0cu)
#define GPIO_EN_SET REG(GPIO + 0x24u)
#define GPIO_IN     REG(GPIO + 0x3cu)
#define SPI_CMD     REG(SPI2 + 0x00u)
#define SPI_USER    REG(SPI2 + 0x10u)
#define SPI_USER2   REG(SPI2 + 0x18u)
#define SPI_DLEN    REG(SPI2 + 0x1cu)
#define SPI_W0      REG(SPI2 + 0x98u)
#define SPI_USR     (1u << 24)

#define PIN_NSS   8u
#define PIN_RESET 12u
#define PIN_BUSY  13u
#define PIN_DIO1  14u

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

static void pause(unsigned count)
{
	for (volatile unsigned i = 0u; i < count; i++)
		;
}

static uint8_t spi(uint8_t value)
{
	SPI_W0 = value;
	SPI_DLEN = 7u;
	SPI_USER = (1u << 27) | (1u << 28) | 1u;   /* MOSI, MISO, full duplex */
	SPI_CMD = SPI_USR;
	while (SPI_CMD & SPI_USR)
		;
	return (uint8_t)SPI_W0;
}

/* A command: BUSY low first, NSS low around its bytes, `in` filled with
 * what the chip answered to each. */
static void command(const uint8_t *out, uint8_t *in, unsigned n)
{
	while (GPIO_IN & (1u << PIN_BUSY))
		;
	GPIO_W1TC = 1u << PIN_NSS;
	for (unsigned i = 0u; i < n; i++) {
		uint8_t got = spi(out[i]);

		if (in != 0)
			in[i] = got;
	}
	GPIO_W1TS = 1u << PIN_NSS;
}

static uint16_t irq_status(void)
{
	uint8_t out[4] = { 0x12, 0, 0, 0 };
	uint8_t in[4];

	command(out, in, 4u);
	return (uint16_t)(in[2] << 8 | in[3]);
}

static void clear_irq(void)
{
	uint8_t out[3] = { 0x02, 0xff, 0xff };

	command(out, 0, 3u);
}

static void packet_params(uint8_t length)
{
	uint8_t out[7] = { 0x8c, 0x00, 0x08, 0x00, length, 0x01, 0x00 };

	command(out, 0, 7u);
}

static void receive(void)
{
	uint8_t out[4] = { 0x82, 0xff, 0xff, 0xff };   /* continuous */

	packet_params(0xff);
	command(out, 0, 4u);
}

static void send(const char *payload)
{
	uint8_t  out[40];
	unsigned n = 0u;

	out[0] = 0x80;                             /* SetStandby, RC */
	out[1] = 0x00;
	command(out, 0, 2u);
	out[0] = 0x0e;                             /* WriteBuffer at 0 */
	out[1] = 0x00;
	while (payload[n] != '\0' && n < 32u) {
		out[2u + n] = (uint8_t)payload[n];
		n++;
	}
	command(out, 0, 2u + n);
	packet_params((uint8_t)n);
	out[0] = 0x83;                             /* SetTx, no timeout */
	out[1] = out[2] = out[3] = 0x00;
	command(out, 0, 4u);
	while (!(GPIO_IN & (1u << PIN_DIO1)))
		;
	clear_irq();
	receive();
}

void start(void);

void start(void)
{
	uint8_t  out[40];
	uint8_t  in[40];
	char     got[40];

	got[0] = 'g';
	got[1] = 'o';
	got[2] = 't';
	got[3] = ' ';
	SPI_USER2 = 0u;
	GPIO_EN_SET = (1u << PIN_NSS) | (1u << PIN_RESET);
	GPIO_W1TS = 1u << PIN_NSS;
	GPIO_W1TC = 1u << PIN_RESET;
	pause(1000u);
	GPIO_W1TS = 1u << PIN_RESET;
	pause(1000u);
	/* ReadRegister 0x0320: its version string, after the NOP. */
	out[0] = 0x1d;
	out[1] = 0x03;
	out[2] = 0x20;
	for (unsigned i = 3u; i < 3u + 1u + 6u; i++)
		out[i] = 0x00;
	command(out, in, 10u);
	say("radio ");
	for (unsigned i = 4u; i < 10u; i++)
		put((char)in[i]);
	say("\r\n");
	out[0] = 0x80;                             /* SetStandby, RC */
	out[1] = 0x00;
	command(out, 0, 2u);
	out[0] = 0x8a;                             /* SetPacketType: LoRa */
	out[1] = 0x01;
	command(out, 0, 2u);
	out[0] = 0x86;                             /* SetRfFrequency: 868 MHz */
	out[1] = 0x36;
	out[2] = 0x40;
	out[3] = 0x00;
	out[4] = 0x00;
	command(out, 0, 5u);
	out[0] = 0x8b;                             /* SF7, 125 kHz, 4/5, no LDRO */
	out[1] = 0x07;
	out[2] = 0x04;
	out[3] = 0x01;
	out[4] = 0x00;
	command(out, 0, 5u);
	out[0] = 0x8f;                             /* buffers at 0 */
	out[1] = 0x00;
	out[2] = 0x00;
	command(out, 0, 3u);
	out[0] = 0x08;                             /* TxDone, RxDone on DIO1 */
	out[1] = 0x00;
	out[2] = 0x03;
	out[3] = 0x00;
	out[4] = 0x03;
	out[5] = out[6] = out[7] = out[8] = 0x00;
	command(out, 0, 9u);
	receive();
	say("listening\r\n");
	for (;;) {
		uint16_t irq;
		unsigned length;

		if (!(GPIO_IN & (1u << PIN_DIO1)))
			continue;
		irq = irq_status();
		clear_irq();
		if (!(irq & 0x0002u))                  /* not RxDone */
			continue;
		out[0] = 0x13;                         /* GetRxBufferStatus */
		out[1] = out[2] = out[3] = 0x00;
		command(out, in, 4u);
		length = in[2] < 32u ? in[2] : 32u;
		out[0] = 0x1e;                         /* ReadBuffer from its start */
		out[1] = in[3];
		out[2] = 0x00;
		for (unsigned i = 0u; i < length; i++)
			out[3u + i] = 0x00;
		command(out, in, 3u + length);
		for (unsigned i = 0u; i < length; i++)
			got[4u + i] = (char)in[3u + i];
		got[4u + length] = '\0';
		say(got);
		say("\r\n");
		send("pong");
	}
}
