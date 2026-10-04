/*
 * Link health: the ports' link state and error counters, watched once a
 * second, with every event worth knowing sent to the console and so to
 * syslog, where Grafana can alert on it. It only reports; it never shuts a
 * port.
 *
 *   link: port 6 up at 2.5G
 *   link: port 6 up at 1G, below its best since boot, 2.5G
 *   link: port 6 down
 *   link: port 6 flapping, 6 changes in 10 min
 *   link: port 6, 12 CRC errors in the last minute
 *
 * - Up and down are taken from the link status register, the speed from
 *   the link-speed register, decoded as sflow.c does.
 * - A downshift is a link at a lower speed than the best that port has had
 *   since boot: a cable that should do 2.5G coming up at 1G.
 * - Flapping is six or more changes within a ten-minute window, logged once
 *   per window.
 * - CRC errors: the RX CRC/alignment counter (MIB counter 15) of every port
 *   is read every ten seconds and its increase added up, reported once a
 *   minute when non-zero.
 * - For the first 20 seconds after boot it only records the state, so the
 *   ports coming up at boot aren't logged as events.
 *
 * "linkwatch" prints the state of every port.
 */

#pragma codeseg BANK3
#pragma constseg BANK3

#include <stdint.h>
#include "rtl837x_common.h"
#include "rtl837x_sfr.h"
#include "rtl837x_regs.h"
#include "rtl837x_port.h"
#include "machine.h"
#include "kadam.h"

extern __code const struct machine machine;
extern __xdata uint8_t sfr_data[4];

#define LW_PORTS	9
#define LW_WINDOW	600	/* seconds per flap-counting window */
#define LW_FLAPS	6	/* changes within a window that count as flapping */
#define LW_CRC_EVERY	10	/* seconds between counter reads */
#define LW_CRC_REPORT	60	/* seconds between CRC reports */
#define LW_CRC_COUNTER	15	/* MIB: RX CRC and alignment errors */
#define LW_QUIET	20	/* seconds after boot that are only recorded */

static __xdata uint8_t lw_quiet;	/* seconds of recording left */
static __xdata uint8_t lw_up[LW_PORTS];
static __xdata uint8_t lw_speed[LW_PORTS];	/* speed nibble, & 7 */
static __xdata uint8_t lw_best[LW_PORTS];	/* rank, 0: never up */
static __xdata uint8_t lw_changes[LW_PORTS];
static __xdata uint8_t lw_flagged[LW_PORTS];
static __xdata uint32_t lw_crc_last[LW_PORTS];
static __xdata uint32_t lw_crc_minute[LW_PORTS];
static __xdata uint32_t lw_crc_total[LW_PORTS];
static __xdata uint16_t lw_window;
static __xdata uint8_t lw_crc_timer, lw_report_timer, lw_crc_ready;
static __xdata uint8_t lw_p, lw_u, lw_s, lw_r;
static __xdata uint16_t lw_upbits;
static __xdata uint8_t lw_links[4], lw_link89;
static __xdata uint32_t lw_v, lw_d;

/* Speed nibble (& 7): 10M, 100M, 1G, -, 10G, 2.5G, 5G */
static __code const char * __code const lw_name[8] = {
	"10M", "100M", "1G", "?", "10G", "2.5G", "5G", "?"
};
static __code const uint8_t lw_rank[8] = { 1, 2, 3, 0, 6, 4, 5, 0 };
/* ...and back: the nibble of each rank */
static __code const uint8_t lw_nibble[7] = { 7, 0, 1, 2, 5, 6, 4 };

void linkwatch_init(void)
{
	lw_quiet = LW_QUIET;
	lw_crc_ready = 0;
	lw_window = LW_WINDOW;
	lw_crc_timer = LW_CRC_EVERY;
	lw_report_timer = LW_CRC_REPORT;
	for (lw_p = 0; lw_p < LW_PORTS; lw_p++) {
		lw_up[lw_p] = 0;
		lw_speed[lw_p] = 0;
		lw_best[lw_p] = 0;
		lw_changes[lw_p] = 0;
		lw_flagged[lw_p] = 0;
		lw_crc_minute[lw_p] = 0;
		lw_crc_total[lw_p] = 0;
	}
}

/* Link state and speed nibble of port lw_p into lw_u, lw_s */
static void lw_port_state(void)
{
	lw_u = (lw_upbits >> lw_p) & 1;
	if (lw_p >= 8)
		lw_s = lw_link89;
	else
		lw_s = lw_links[3 - (lw_p >> 1)];
	lw_s = (lw_p & 1) ? lw_s >> 4 : lw_s & 0xf;
	lw_s &= 7;
}

static void lw_port_name(void)
{
	print_string("link: port ");
	kc_out_port = lw_p;
	kc_print_port();
}

static void lw_links_read(void)
{
	reg_read(RTL837X_REG_LINKS_STS);
	lw_upbits = SFR_DATA_16 | ((uint16_t)SFR_DATA_8 << 8);
	reg_read_m(RTL837X_REG_LINKS);
	lw_links[0] = sfr_data[0];
	lw_links[1] = sfr_data[1];
	lw_links[2] = sfr_data[2];
	lw_links[3] = sfr_data[3];
	reg_read_m(RTL837X_REG_LINKS_89);
	lw_link89 = sfr_data[3];
}

static void lw_links_check(void)
{
	lw_links_read();
	for (lw_p = machine.min_port; lw_p <= machine.max_port && lw_p < LW_PORTS; lw_p++) {
		lw_port_state();
		if (lw_quiet) {
			lw_up[lw_p] = lw_u;
			lw_speed[lw_p] = lw_s;
			if (lw_u)
				lw_best[lw_p] = lw_rank[lw_s];
			continue;
		}
		if (lw_u == lw_up[lw_p] && (!lw_u || lw_s == lw_speed[lw_p]))
			continue;

		if (lw_u != lw_up[lw_p] && lw_changes[lw_p] != 0xff)
			lw_changes[lw_p]++;
		lw_up[lw_p] = lw_u;
		lw_speed[lw_p] = lw_s;
		lw_port_name();
		if (!lw_u) {
			print_string(" down\n");
		} else {
			print_string(" up at ");
			print_string(lw_name[lw_s]);
			lw_r = lw_rank[lw_s];
			if (lw_r < lw_best[lw_p]) {
				print_string(", below its best since boot, ");
				print_string(lw_name[lw_nibble[lw_best[lw_p]]]);
			} else {
				lw_best[lw_p] = lw_r;
			}
			write_char('\n');
		}
		if (lw_changes[lw_p] >= LW_FLAPS && !lw_flagged[lw_p]) {
			lw_flagged[lw_p] = 1;
			lw_port_name();
			print_string(" flapping, ");
			itoa_short(lw_changes[lw_p]);
			print_string(" changes in 10 min\n");
		}
	}
	if (lw_quiet)
		lw_quiet--;

	if (!--lw_window) {
		lw_window = LW_WINDOW;
		for (lw_p = 0; lw_p < LW_PORTS; lw_p++) {
			lw_changes[lw_p] = 0;
			lw_flagged[lw_p] = 0;
		}
	}
}

/*
 * Adds the counter's increase since the last read (lw_v, port lw_p). A leaf
 * on purpose: the compiler's scratch for the 32-bit arithmetic on these
 * arrays then shares the overlay area instead of claiming internal RAM of
 * its own.
 */
static void lw_crc_add(void)
{
	lw_d = lw_crc_last[lw_p];
	lw_crc_last[lw_p] = lw_v;
	if (!lw_crc_ready)
		return;
	lw_d = lw_v - lw_d;
	lw_crc_minute[lw_p] += lw_d;
	lw_crc_total[lw_p] += lw_d;
}

static void lw_crc_read(void)
{
	for (lw_p = machine.min_port; lw_p <= machine.max_port && lw_p < LW_PORTS; lw_p++) {
		STAT_GET(LW_CRC_COUNTER, lw_p);
		reg_read_m(RTL837X_STAT_V_LOW);
		lw_v = sfr_data[0];
		lw_v <<= 8;
		lw_v |= sfr_data[1];
		lw_v <<= 8;
		lw_v |= sfr_data[2];
		lw_v <<= 8;
		lw_v |= sfr_data[3];
		lw_crc_add();
	}
	lw_crc_ready = 1;
}

static void lw_crc_report(void)
{
	for (lw_p = machine.min_port; lw_p <= machine.max_port && lw_p < LW_PORTS; lw_p++) {
		if (!lw_crc_minute[lw_p])
			continue;
		lw_port_name();
		print_string(", ");
		kc_out_u32 = lw_crc_minute[lw_p];
		kc_print_u32();
		print_string(" CRC errors in the last minute\n");
		lw_crc_minute[lw_p] = 0;
	}
}

void linkwatch_second(void)
{
	lw_links_check();
	if (!--lw_crc_timer) {
		lw_crc_timer = LW_CRC_EVERY;
		lw_crc_read();
	}
	if (!--lw_report_timer) {
		lw_report_timer = LW_CRC_REPORT;
		lw_crc_report();
	}
}

void linkwatch_cmd(void)
{
	lw_links_read();
	for (lw_p = machine.min_port; lw_p <= machine.max_port && lw_p < LW_PORTS; lw_p++) {
		lw_port_state();
		print_string("port ");
		kc_out_port = lw_p;
		kc_print_port();
		if (lw_u) {
			print_string(": up at ");
			print_string(lw_name[lw_s]);
		} else {
			print_string(": down");
		}
		print_string(", changes this window ");
		itoa_short(lw_changes[lw_p]);
		print_string(", CRC errors since boot ");
		kc_out_u32 = lw_crc_total[lw_p];
		kc_print_u32();
		write_char('\n');
	}
}
