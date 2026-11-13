/*
 * mps2_net.c -- firmware for an MPS2 board's AN385 (Cortex-M3) on a
 * network of hull's (test/board_test.sh): "ready" on UART0, each line
 * typed said back as "said LINE", and Ethernet by the board's SMSC
 * LAN9118, qemu's lan9118, the same protocol as lm3s_net.c: it answers
 * ARP for its own address, IP, and asks for PEER's until it is answered,
 * saying "peer answered"; "asked" once it has answered somebody. MAC is
 * the last byte of 02:00:00:00:00:MAC. A frame goes out as TX command A
 * and B and its words; one comes in as a status word, its length with
 * the CRC in bits 29:16, then its words.
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
#define UART_DATA     REG(0x40004000u)
#define UART_STATE    REG(0x40004004u)   /* bit 0 TX full, bit 1 RX full */
#define UART_CTRL     REG(0x40004008u)
#define UART_BAUDDIV  REG(0x40004010u)
#define ETH           0x40200000u
#define ETH_RX_DATA   REG(ETH + 0x00u)
#define ETH_TX_DATA   REG(ETH + 0x20u)
#define ETH_RX_STATUS REG(ETH + 0x40u)
#define ETH_TX_CFG    REG(ETH + 0x70u)
#define ETH_RX_INF    REG(ETH + 0x7cu)   /* RX status words used, 23:16 */
#define ETH_MAC_CMD   REG(ETH + 0xa4u)
#define ETH_MAC_DATA  REG(ETH + 0xa8u)

static uint8_t tx[64];
static uint8_t rx[1600];

static void put(char c)
{
	while (UART_STATE & 1u)
		;
	UART_DATA = (uint8_t)c;
}

static void say(const char *text)
{
	while (*text != '\0')
		put(*text++);
}

static void mac_write(unsigned index, uint32_t value)
{
	ETH_MAC_DATA = value;
	ETH_MAC_CMD = 0x80000000u | index;
	while (ETH_MAC_CMD & 0x80000000u)
		;
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
 * broadcast where NULL, sent: 60 bytes, the first and last segment. */
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
	ETH_TX_DATA = (1u << 13) | (1u << 12) | 60u;   /* A: first, last, 60 bytes */
	ETH_TX_DATA = 60u;                             /* B: the packet's length */
	for (i = 0u; i < 60u; i += 4u)
		ETH_TX_DATA = (uint32_t)tx[i] | (uint32_t)tx[i + 1u] << 8 |
		              (uint32_t)tx[i + 2u] << 16 | (uint32_t)tx[i + 3u] << 24;
}

/* The next frame received into rx; its length, the CRC not counted. */
static unsigned receive(void)
{
	unsigned length = (ETH_RX_STATUS >> 16) & 0x3fffu;
	unsigned i;

	for (i = 0u; i < length; i += 4u) {
		uint32_t word = ETH_RX_DATA;

		if (i + 3u < sizeof rx) {
			rx[i] = (uint8_t)word;
			rx[i + 1u] = (uint8_t)(word >> 8);
			rx[i + 2u] = (uint8_t)(word >> 16);
			rx[i + 3u] = (uint8_t)(word >> 24);
		}
	}
	return length >= 4u ? length - 4u : 0u;
}

void reset(void);

void reset(void)
{
	char     line[32];
	unsigned n = 0u;
	unsigned wait = 0u;
	int      heard = 0;
	int      answered = 0;

	UART_BAUDDIV = 16u;
	UART_CTRL = 3u;                       /* TX and RX enabled */
	mac_write(3u, 0x00000002u);           /* ADDRL: 02:00:00:00 */
	mac_write(2u, (uint32_t)MAC << 8);    /* ADDRH: 00:MAC */
	mac_write(1u, 0x0000000cu);           /* MAC_CR: TXEN, RXEN */
	ETH_TX_CFG = 2u;                      /* TX_ON */
	say("ready\r\n");
	for (;;) {
		if (!heard && wait-- == 0u) {
			arp(1u, 0, PEER);
			wait = 400000u;
		}
		if ((ETH_RX_INF >> 16) & 0xffu) {
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
		if (UART_STATE & 2u) {
			char c = (char)UART_DATA;

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
