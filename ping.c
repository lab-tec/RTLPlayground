/*
 * ping from the switch: "ping <ip>" sends four ICMP echo requests, one a
 * second, and "ping" shows how it went:
 *
 *   ping: 172.16.0.254: 4 sent, 4 received, rtt min/avg/max 0/1/5 ms
 *
 * It runs in the background, from kadam_second(), because the web console
 * shows only what a command prints while it runs; each reply and the
 * summary also go to the console and syslog. Times are in ticks of 5 ms.
 *
 * - Sending: the request is built in uip_buf (checksums computed here; uIP's
 *   own helpers sit in bank 1, out of reach), then uip_arp_out() adds the
 *   Ethernet header and tcpip_output() sends it, as the main loop does for
 *   UDP. If the next hop's address isn't known yet, uip_arp_out() sends an
 *   ARP request in its place, so the first request can go missing.
 * - Receiving: uip.c drops ICMP other than echo requests; for an echo reply
 *   it first calls ping_reply(), which matches identifier, sender and
 *   sequence number.
 */

#pragma codeseg BANK3
#pragma constseg BANK3

#include <stdint.h>
#include "rtl837x_common.h"
#include "uip/uip.h"
#include "uip/uip_arp.h"
#include "kadam.h"

extern volatile __xdata uint32_t ticks;

#define PING_COUNT	4
#define PING_WAIT	3	/* seconds after the last request before the summary */
#define PING_DATA	32
#define PING_IP_LEN	(20 + 8 + PING_DATA)
#define PING_IDENT	0x4b44	/* "KD" */

#define PING_IP		(&uip_buf[UIP_LLH_LEN])
#define PING_ICMP	(&uip_buf[UIP_LLH_LEN + 20])

static __xdata uint8_t pg_addr[4];
static __xdata uint8_t pg_used;		/* a ping has been started since boot */
static __xdata uint8_t pg_to_send, pg_wait;
static __xdata uint8_t pg_sent, pg_recv;
static __xdata uint8_t pg_got;		/* replies seen, a bit per sequence number */
static __xdata uint32_t pg_at[PING_COUNT];	/* ticks when each was sent */
static __xdata uint16_t pg_min, pg_max, pg_sum, pg_rtt, pg_rest;
static __xdata uint16_t pg_ipid;
static __xdata uint8_t pg_i, pg_seq;
static __xdata uint16_t pg_ck, pg_w;

void ping_init(void)
{
	pg_used = 0;
	pg_to_send = 0;
	pg_wait = 0;
	pg_ipid = 0x4b00;
}

/* The Internet checksum of pg_len bytes at uip_buf[pg_from] */
static __xdata uint16_t pg_from, pg_len;
/* 16-bit sums with the carry added back as it happens: 32-bit temporaries
 * would take internal RAM */
static uint16_t pg_checksum(void)
{
	pg_ck = 0;
	for (pg_i = 0; pg_i < pg_len; pg_i += 2) {
		pg_w = uip_buf[pg_from + pg_i];
		pg_w <<= 8;
		if (pg_i + 1 < pg_len)
			pg_w |= uip_buf[pg_from + pg_i + 1];
		pg_ck += pg_w;
		if (pg_ck < pg_w)
			pg_ck++;
	}
	return ~pg_ck;
}

static void pg_send(void)
{
	pg_seq = pg_sent;

	/* IPv4 header */
	PING_IP[0] = 0x45;
	PING_IP[1] = 0;
	PING_IP[2] = 0;
	PING_IP[3] = PING_IP_LEN;
	pg_ipid++;
	PING_IP[4] = pg_ipid >> 8;
	PING_IP[5] = pg_ipid;
	PING_IP[6] = 0;
	PING_IP[7] = 0;
	PING_IP[8] = 64;
	PING_IP[9] = 1;		/* ICMP */
	PING_IP[10] = 0;
	PING_IP[11] = 0;
	memcpy(&PING_IP[12], uip_hostaddr, 4);
	for (pg_i = 0; pg_i < 4; pg_i++)
		PING_IP[16 + pg_i] = pg_addr[pg_i];
	pg_from = UIP_LLH_LEN;
	pg_len = 20;
	pg_ck = pg_checksum();
	PING_IP[10] = pg_ck >> 8;
	PING_IP[11] = pg_ck;

	/* ICMP echo request */
	PING_ICMP[0] = 8;
	PING_ICMP[1] = 0;
	PING_ICMP[2] = 0;
	PING_ICMP[3] = 0;
	PING_ICMP[4] = PING_IDENT >> 8;
	PING_ICMP[5] = PING_IDENT & 0xff;
	PING_ICMP[6] = 0;
	PING_ICMP[7] = pg_seq;
	for (pg_i = 0; pg_i < PING_DATA; pg_i++)
		PING_ICMP[8 + pg_i] = 'a' + pg_i;
	pg_from = UIP_LLH_LEN + 20;
	pg_len = 8 + PING_DATA;
	pg_ck = pg_checksum();
	PING_ICMP[2] = pg_ck >> 8;
	PING_ICMP[3] = pg_ck;

	pg_at[pg_seq] = ticks;
	pg_sent++;
	uip_len = PING_IP_LEN;
	uip_arp_out();
	tcpip_output();
	uip_len = 0;
}

static void pg_summary(void)
{
	print_string("ping: ");
	for (pg_i = 0; pg_i < 4; pg_i++)
		kc_out_ip[pg_i] = pg_addr[pg_i];
	kc_print_ip();
	print_string(": ");
	itoa_short(pg_sent);
	print_string(" sent, ");
	itoa_short(pg_recv);
	print_string(" received");
	if (pg_recv) {
		print_string(", rtt min/avg/max ");
		itoa_short(pg_min);
		write_char('/');
		/* the average by subtraction: at most four replies */
		pg_rtt = 0;
		for (pg_rest = pg_sum; pg_rest >= pg_recv; pg_rest -= pg_recv)
			pg_rtt++;
		itoa_short(pg_rtt);
		write_char('/');
		itoa_short(pg_max);
		print_string(" ms");
	}
	if (pg_to_send || pg_wait)
		print_string(" (still running)");
	write_char('\n');
}

void ping_second(void)
{
	if (pg_to_send) {
		pg_to_send--;
		pg_send();
		if (!pg_to_send)
			pg_wait = PING_WAIT;
		return;
	}
	if (pg_wait && !--pg_wait)
		pg_summary();
}

void ping_reply(void) __banked
{
	if (!pg_to_send && !pg_wait)
		return;
	if ((PING_IP[0] & 0x0f) != 5 || PING_ICMP[4] != (PING_IDENT >> 8)
	    || PING_ICMP[5] != (PING_IDENT & 0xff) || PING_ICMP[6] != 0)
		return;
	for (pg_i = 0; pg_i < 4; pg_i++) {
		if (PING_IP[12 + pg_i] != pg_addr[pg_i])
			return;
	}
	pg_seq = PING_ICMP[7];
	if (pg_seq >= pg_sent || ((pg_got >> pg_seq) & 1))
		return;
	pg_got |= 1 << pg_seq;
	pg_rtt = (uint16_t)(ticks - pg_at[pg_seq]) * 5;
	pg_recv++;
	if (pg_recv == 1 || pg_rtt < pg_min)
		pg_min = pg_rtt;
	if (pg_rtt > pg_max)
		pg_max = pg_rtt;
	pg_sum += pg_rtt;
	print_string("ping: reply from ");
	for (pg_i = 0; pg_i < 4; pg_i++)
		kc_out_ip[pg_i] = pg_addr[pg_i];
	kc_print_ip();
	print_string(" seq ");
	itoa_short(pg_seq + 1);
	print_string(": ");
	itoa_short(pg_rtt);
	print_string(" ms\n");
}

void ping_cmd(void)
{
	if (cmd_words_len == 1) {
		if (!pg_used)
			print_string("ping: none yet. Usage: ping <ip>\n");
		else
			pg_summary();
		return;
	}
	if (cmd_words_len != 2 || !kc_ip(1)) {
		kc_usage("Usage: ping <ip>\n");
		return;
	}
	for (pg_i = 0; pg_i < 4; pg_i++)
		pg_addr[pg_i] = kc_addr[pg_i];
	pg_used = 1;
	pg_to_send = PING_COUNT;
	pg_wait = 0;
	pg_sent = 0;
	pg_recv = 0;
	pg_got = 0;
	pg_min = 0;
	pg_max = 0;
	pg_sum = 0;
	print_string("ping: 4 echo requests to ");
	for (pg_i = 0; pg_i < 4; pg_i++)
		kc_out_ip[pg_i] = kc_addr[pg_i];
	kc_print_ip();
	print_string(", one a second; \"ping\" shows the results\n");
}
