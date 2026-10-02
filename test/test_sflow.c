/*
 * test_sflow.c: the datagrams sflow.c builds, checked field by field.
 *
 * sflow.c is compiled unmodified against the register mock. Every MIB counter
 * of every port holds a value derived from its port and counter number (see
 * counter_value()), so each field of the datagram can be traced back to the
 * counter, and the half of it, that it was read from. Port 3 is down, the
 * others link at 2.5G.
 *
 * With an argument, the nine datagrams of one round are also written to that
 * file, one hex line each, for a collector's own tests.
 */
#include <stdio.h>
#include <stdlib.h>
#include "rtl837x_common.h"
#include "rtl837x_regs.h"
#include "machine.h"
#include "uip/uip.h"
#include "sflow.h"
#include "hw_mock.h"

static int fails, checks;
#define CHECK(c, msg) do { checks++; if (!(c)) { fails++; \
	printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, msg); } } while (0)

/* ---- what sflow.c links against ---- */
const struct machine machine = {
	.machine_name = "HOSTTEST",
	.min_port = 0,
	.max_port = 8,
	.log_to_phys_port = { 1, 2, 3, 4, 5, 6, 7, 8, 9 },
	.phys_to_log_port = { 0, 1, 2, 3, 4, 5, 6, 7, 8 },
};
volatile uint32_t ticks;
uip_ipaddr_t uip_hostaddr;
static uint8_t appbuf[UIP_CONF_BUFFER_SIZE];
void *uip_appdata = appbuf;
static struct uip_udp_conn conn = { .lport = 0x1234 };
static uint8_t sent[UIP_CONF_BUFFER_SIZE];
static int sent_len;

void uip_send(const void *data, uint16_t len) { memcpy(sent, data, len); sent_len = len; }
struct uip_udp_conn *uip_udp_new(uip_ipaddr_t *ripaddr, u16_t rport)
{
	(void)ripaddr; (void)rport; return &conn;
}
void print_string(const char *p) { (void)p; }

/* ---- helpers ---- */
static uint64_t counter_value(int port, int c)
{
	uint32_t lo = (port + 1) * 1000 + c * 2;
	return ((uint64_t)(lo + 1) << 32) | lo;
}
static uint32_t lo(int port, int c) { return (uint32_t)counter_value(port, c); }
static uint32_t hi(int port, int c) { return (uint32_t)(counter_value(port, c) >> 32); }

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
static uint64_t be64(const uint8_t *p) { return (uint64_t)be32(p) << 32 | be32(p + 4); }

int main(int argc, char **argv)
{
	FILE *dump = argc > 1 ? fopen(argv[1], "w") : NULL;

	hw_reset();
	for (int p = 0; p < 9; p++)
		for (int c = 0; c < 64; c++)
			hw_counter_set(p, c, counter_value(p, c));
	/* All up but logical port 3; every port at 2.5G (speed code 5) */
	hw_reg_set(RTL837X_REG_LINKS_STS, 0x00f70100);
	hw_reg_set(RTL837X_REG_LINKS, 0x55555555);
	hw_reg_set(RTL837X_REG_LINKS_89, 0x00000005);
	/* As the address sits in the switch's memory: network order. Not through
	 * uip_ipaddr(): uip-conf.h names the byte order LITTLE_ENDIAN, which SDCC
	 * leaves undefined (so HTONS swaps, correctly, on the 8051), while glibc
	 * defines it as 1234, uIP's value for big-endian, so on the host the
	 * macro would leave the halves unswapped. */
	memcpy(uip_hostaddr, "\xac\x10\x00\x06", 4);

	sflow_init();
	sflow_state.conn = &conn;
	sflow_state.next = machine.min_port;
	ticks = 1000000;

	for (int p = 0; p < 9; p++) {
		sflow_state.last = 0;
		sent_len = 0;
		sflow_callback(conn.lport);
		CHECK(sent_len == 204, "datagram is 28 + 8 + 168 bytes");
		if (sent_len != 204)
			continue;
		if (dump) {
			for (int i = 0; i < sent_len; i++)
				fprintf(dump, "%02x", sent[i]);
			fputc('\n', dump);
		}
		const uint8_t *d = sent;
		CHECK(be32(d) == 5, "sFlow version 5");
		CHECK(be32(d + 4) == 1, "agent address is IPv4");
		CHECK(!memcmp(d + 8, "\xac\x10\x00\x06", 4), "agent address is the switch's");
		CHECK(be32(d + 16) == (uint32_t)p + 1, "datagram sequence counts up");
		CHECK(be32(d + 20) == 1000000 * 5, "uptime in ms from the 200 Hz tick");
		CHECK(be32(d + 24) == 1, "one sample");
		const uint8_t *s = d + 28;
		CHECK(be32(s) == 2 && be32(s + 4) == 168, "counters sample, 168 bytes");
		CHECK(be32(s + 8) == 1, "per-port sample sequence starts at 1");
		CHECK(be32(s + 12) == (uint32_t)p + 1, "source id is the front-panel port");
		CHECK(be32(s + 16) == 2, "two records");
		const uint8_t *g = s + 20;
		CHECK(be32(g) == 1 && be32(g + 4) == 88, "generic interface record, 88 bytes");
		g += 8;
		CHECK(be32(g) == (uint32_t)p + 1, "ifIndex");
		CHECK(be32(g + 4) == 6, "ifType ethernetCsmacd");
		CHECK(be64(g + 8) == (p == 3 ? 0 : 2500000000ULL), "ifSpeed: 2.5G, or 0 when down");
		CHECK(be32(g + 20) == (p == 3 ? 1u : 3u), "ifStatus: admin up, oper up unless down");
		CHECK(be64(g + 24) == counter_value(p, 0), "ifInOctets, 64 bits of counter 0");
		CHECK(be32(g + 32) == lo(p, 2), "ifInUcastPkts");
		CHECK(be32(g + 36) == lo(p, 3), "ifInMulticastPkts");
		CHECK(be32(g + 40) == lo(p, 4), "ifInBroadcastPkts");
		CHECK(be32(g + 44) == lo(p, 8), "ifInDiscards, low half of counter 8");
		CHECK(be32(g + 48) == hi(p, 48), "ifInErrors, high half of counter 48");
		CHECK(be32(g + 52) == 0xffffffffu, "ifInUnknownProtos unknown");
		CHECK(be64(g + 56) == counter_value(p, 1), "ifOutOctets, 64 bits of counter 1");
		CHECK(be32(g + 64) == lo(p, 5), "ifOutUcastPkts");
		CHECK(be32(g + 68) == lo(p, 6), "ifOutMulticastPkts");
		CHECK(be32(g + 72) == lo(p, 7), "ifOutBroadcastPkts");
		CHECK(be32(g + 76) == hi(p, 8), "ifOutDiscards, high half of counter 8");
		CHECK(be32(g + 80) == lo(p, 48), "ifOutErrors, low half of counter 48");
		CHECK(be32(g + 84) == 1, "ifPromiscuousMode");
		const uint8_t *e = g + 88;
		CHECK(be32(e) == 2 && be32(e + 4) == 52, "ethernet record, 52 bytes");
		e += 8;
		CHECK(be32(e + 4) == lo(p, 15), "FCS errors, RX CRC/align");
		CHECK(be32(e + 8) == hi(p, 9) && be32(e + 12) == lo(p, 9), "single, multiple collisions");
		CHECK(be32(e + 20) == hi(p, 10) && be32(e + 24) == lo(p, 10), "deferred, late collisions");
		CHECK(be32(e + 28) == hi(p, 11), "excessive collisions");
		CHECK(be32(e + 40) == hi(p, 29), "frame too long, RX too large");
		CHECK(be32(e + 48) == lo(p, 11), "symbol errors");
	}
	if (dump)
		fclose(dump);

	printf("test_sflow: %d checks, %d failed\n", checks, fails);
	return fails ? 1 : 0;
}
