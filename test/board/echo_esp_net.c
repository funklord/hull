/*
 * echo_esp_net.c -- firmware for an ESP32-C3 on a network of hull's
 * (test/board_test.sh): echo_esp_rv.c's console, "ready" and each line
 * said back, and Ethernet by the open_eth device of Espressif's qemu, the
 * OpenCores MAC ESP-IDF drives as `openeth`. It answers ARP for its own
 * address, IP, and asks for PEER's until it is answered, then says "peer
 * answered"; "asked" once it has answered somebody. MAC is the last byte
 * of its own MAC, 02:00:00:00:00:MAC. Its frames are at BUFFER, RAM the
 * ROM leaves alone, as nothing but .text is loaded.
 */
#include <stdint.h>

#ifndef UART
#define UART 0x60000000u
#endif
#ifndef ETH
#define ETH 0x600cd000u
#endif
#ifndef BUFFER
#define BUFFER 0x3fca0000u
#endif
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
#define UART_FIFO    REG(UART + 0x00u)
#define UART_STATUS  REG(UART + 0x1cu)   /* rx count 9:0, tx count 25:16 */
#define ETH_MODER    REG(ETH + 0x00u)
#define ETH_TX_BD    REG(ETH + 0x20u)
#define ETH_MAC0     REG(ETH + 0x40u)
#define ETH_MAC1     REG(ETH + 0x44u)
#define TX_BD0       REG(ETH + 0x400u)
#define TX_BD1       REG(ETH + 0x404u)
#define RX_BD0       REG(ETH + 0x408u)
#define RX_BD1       REG(ETH + 0x40cu)

#define MODER_RXEN  (1u << 0)
#define MODER_TXEN  (1u << 1)
#define MODER_FULLD (1u << 10)
#define MODER_CRCEN (1u << 13)
#define MODER_PAD   (1u << 15)
#define BD_READY    (1u << 15)   /* tx: to be sent; rx: empty */
#define BD_WRAP     (1u << 13)
#define BD_PAD      (1u << 12)
#define BD_CRC      (1u << 11)

static volatile uint8_t *const tx = (volatile uint8_t *)BUFFER;
static volatile uint8_t *const rx = (volatile uint8_t *)(BUFFER + 0x800u);

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

static void put_ip(volatile uint8_t *at, uint32_t ip)
{
	at[0] = (uint8_t)(ip >> 24);
	at[1] = (uint8_t)(ip >> 16);
	at[2] = (uint8_t)(ip >> 8);
	at[3] = (uint8_t)ip;
}

static uint32_t get_ip(const volatile uint8_t *at)
{
	return (uint32_t)at[0] << 24 | (uint32_t)at[1] << 16 | (uint32_t)at[2] << 8 | at[3];
}

/* An ARP frame, OPERATION 1 asking or 2 answering, to TARGET at its MAC,
 * broadcast where NULL, sent and waited for. */
static void arp(unsigned operation, const volatile uint8_t *target_mac, uint32_t target)
{
	static const uint8_t head[] = { 0x08, 0x06, 0x00, 0x01, 0x08, 0x00, 6, 4, 0 };
	unsigned             i;

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
	TX_BD1 = BUFFER;
	TX_BD0 = 42u << 16 | BD_READY | BD_WRAP | BD_PAD | BD_CRC;
	while (TX_BD0 & BD_READY)
		;
}

void start(void);

void start(void)
{
	char     ready[] = { 'r', 'e', 'a', 'd', 'y', '\r', '\n', '\0' };
	char     said[] = { 's', 'a', 'i', 'd', ' ', '\0' };
	char     end[] = { '\r', '\n', '\0' };
	char     peer[] = { 'p', 'e', 'e', 'r', ' ', 'a', 'n', 's', 'w', 'e', 'r', 'e', 'd', '\r', '\n',
	                    '\0' };
	char     asked[] = { 'a', 's', 'k', 'e', 'd', '\r', '\n', '\0' };
	char     line[32];
	unsigned n = 0u;
	unsigned wait = 0u;
	int      heard = 0;
	int      answered = 0;

	ETH_MODER = 0u;
	ETH_TX_BD = 1u;
	ETH_MAC0 = (uint32_t)MAC;
	ETH_MAC1 = 0x0200u;
	RX_BD1 = BUFFER + 0x800u;
	RX_BD0 = BD_READY | BD_WRAP;
	ETH_MODER = MODER_RXEN | MODER_TXEN | MODER_FULLD | MODER_CRCEN | MODER_PAD;
	say(ready);
	for (;;) {
		if (!heard && wait-- == 0u) {
			arp(1u, 0, PEER);
			wait = 400000u;
		}
		if (!(RX_BD0 & BD_READY)) {
			/* An ARP frame: its operation, and whose it is. */
			if (rx[12] == 0x08u && rx[13] == 0x06u) {
				if (rx[21] == 2u && get_ip(rx + 28) == PEER && !heard) {
					heard = 1;
					say(peer);
				} else if (rx[21] == 1u && get_ip(rx + 38) == IP) {
					arp(2u, rx + 22, get_ip(rx + 28));
					if (!answered)
						say(asked);
					answered = 1;
				}
			}
			RX_BD0 = BD_READY | BD_WRAP;
		}
		if ((UART_STATUS & 0x3ffu) != 0u) {
			char c = (char)UART_FIFO;

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
}
