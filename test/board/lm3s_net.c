/*
 * lm3s_net.c -- firmware for an LM3S6965 evaluation board on a network of
 * hull's (test/board_test.sh): "ready" on UART0, each line typed said back
 * as "said LINE", and Ethernet by the chip's own MAC, qemu's
 * stellaris_enet. It answers ARP for its own address, IP, and asks for
 * PEER's until it is answered, saying "peer answered"; "asked" once it has
 * answered somebody. MAC is the last byte of 02:00:00:00:00:MAC. The
 * MAC's FIFO takes a frame's length less its header first, beside the
 * frame's first two bytes, and gives a received one's length with its
 * own two bytes and the CRC counted.
 */
#include <stdint.h>

#ifndef IP
#define IP 0x0a00020fu      /* 10.0.2.15 */
#endif
#ifndef PEER
#define PEER 0x0a000202u    /* 10.0.2.2, slirp's gateway */
#endif
#ifndef MAC
#define MAC 0x10u
#endif

#define REG(address) (*(volatile uint32_t *)(address))
#define UART_DR   REG(0x4000c000u)
#define UART_FR   REG(0x4000c018u)
#define UART_CR   REG(0x4000c030u)
#define ETH_RIS   REG(0x40048000u)
#define ETH_RCTL  REG(0x40048008u)
#define ETH_TCTL  REG(0x4004800cu)
#define ETH_DATA  REG(0x40048010u)
#define ETH_IA0   REG(0x40048014u)
#define ETH_IA1   REG(0x40048018u)
#define ETH_NP    REG(0x40048034u)
#define ETH_TR    REG(0x40048038u)

static uint8_t tx[64];
static uint8_t rx[1600];

static void put(char c)
{
	while (UART_FR & (1u << 5))   /* TXFF */
		;
	UART_DR = (uint8_t)c;
}

static void say(const char *text)
{
	while (*text != '\0')
		put(*text++);
}

static void put_ip(uint8_t *at, uint32_t ip)
{
	at[0] = (uint8_t)(ip >> 24);
	at[1] = (uint8_t)(ip >> 16);
	at[2] = (uint8_t)(ip >> 8);
	at[3] = (uint8_t)ip;
}

static uint32_t get_ip(const uint8_t *at)
{
	return (uint32_t)at[0] << 24 | (uint32_t)at[1] << 16 | (uint32_t)at[2] << 8 | at[3];
}

/* An ARP frame, OPERATION 1 asking or 2 answering, to TARGET at its MAC,
 * broadcast where NULL, sent. */
static void arp(unsigned operation, const uint8_t *target_mac, uint32_t target)
{
	static const uint8_t head[] = { 0x08, 0x06, 0x00, 0x01, 0x08, 0x00, 6, 4, 0 };
	unsigned             i;

	for (i = 0u; i < sizeof tx; i++)
		tx[i] = 0u;
	for (i = 0u; i < 6u; i++) {
		tx[i] = target_mac != 0 ? target_mac[i] : 0xffu;
		tx[6u + i] = i == 0u ? 0x02u : i == 5u ? (uint8_t)MAC : 0u;
		tx[22u + i] = tx[6u + i];
		tx[32u + i] = target_mac != 0 ? target_mac[i] : 0u;
	}
	for (i = 0u; i < sizeof head; i++)
		tx[12u + i] = head[i];
	tx[21] = (uint8_t)operation;
	put_ip(tx + 28, IP);
	put_ip(tx + 38, target);
	/* 60 bytes: its length less the header, then the frame from byte 2. */
	ETH_DATA = (60u - 14u) | (uint32_t)tx[0] << 16 | (uint32_t)tx[1] << 24;
	for (i = 2u; i < 60u; i += 4u)
		ETH_DATA = (uint32_t)tx[i] | (uint32_t)tx[i + 1u] << 8 |
		           (uint32_t)tx[i + 2u] << 16 | (uint32_t)tx[i + 3u] << 24;
	ETH_TR = 1u;
}

/* The next frame received into rx; its length. */
static unsigned receive(void)
{
	uint32_t word = ETH_DATA;
	unsigned length = word & 0xffffu;   /* its two bytes, the frame, the CRC */
	unsigned i;

	rx[0] = (uint8_t)(word >> 16);
	rx[1] = (uint8_t)(word >> 24);
	for (i = 2u; i + 2u < length; i += 4u) {
		word = ETH_DATA;
		if (i + 3u < sizeof rx) {
			rx[i] = (uint8_t)word;
			rx[i + 1u] = (uint8_t)(word >> 8);
			rx[i + 2u] = (uint8_t)(word >> 16);
			rx[i + 3u] = (uint8_t)(word >> 24);
		}
	}
	return length - 6u;
}

void reset(void);

void reset(void)
{
	char     line[32];
	unsigned n = 0u;
	unsigned wait = 0u;
	int      heard = 0;
	int      answered = 0;

	UART_CR = 0x301u;                    /* UARTEN, TXE, RXE */
	ETH_IA0 = 0x02u;
	ETH_IA1 = (uint32_t)MAC << 8;
	ETH_TCTL = 0x7u;                     /* TXEN, PADEN, CRC */
	ETH_RCTL = 0x1u;                     /* RXEN */
	say("ready\r\n");
	for (;;) {
		if (!heard && wait-- == 0u) {
			arp(1u, 0, PEER);
			wait = 400000u;
		}
		if (ETH_NP != 0u) {
			unsigned length = receive();

			if (length >= 42u && rx[12] == 0x08u && rx[13] == 0x06u) {
				if (rx[21] == 2u && get_ip(rx + 28) == PEER && !heard) {
					heard = 1;
					say("peer answered\r\n");
				} else if (rx[21] == 1u && get_ip(rx + 38) == IP) {
					arp(2u, rx + 22, get_ip(rx + 28));
					if (!answered)
						say("asked\r\n");
					answered = 1;
				}
			}
		}
		if (!(UART_FR & (1u << 4))) {        /* RXFE clear */
			char c = (char)UART_DR;

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
}

__attribute__((section(".vectors"), used)) void *const vectors[2] = {
	(void *)0x20010000u, (void *)reset,
};
