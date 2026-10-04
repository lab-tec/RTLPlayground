/*
 * The new-device log, after Cisco's MAC address notifications: the switch's
 * learned-address table is walked a few entries a second, and a device seen
 * for the first time, or seen on a different port, is reported:
 *
 *   mac: new 3c:22:fb:01:02:03 vlan 1 on port 7
 *   mac: 3c:22:fb:01:02:03 vlan 1 moved from port 7 to port 3
 *
 * - The walk uses the same next-entry table search as the web interface's
 *   MAC table (send_l2() in httpd/page_impl.c): ask for the first entry at
 *   or after an index, and the switch answers with the next one, wrapping
 *   round to the lowest. A walk starts at index 0 and is complete when the
 *   answer wraps (or the table is empty), so an entry ageing out mid-walk
 *   can't make it run forever. Static entries are skipped.
 * - The first complete walk after boot, or after "macwatch on", is the
 *   baseline: everything already there is remembered, and only a count is
 *   logged.
 * - Only ports being watched are reported ("macwatch ports 2 3 5 7"): a
 *   port behind an access point would otherwise log every phone that roams.
 *   A move is reported when either port is watched.
 * - Devices are remembered until reboot in a 512-slot table (open
 *   addressing on the MAC's last bytes). When it fills, that is logged
 *   once, and newcomers are no longer tracked.
 *
 * "macwatch" shows the settings and how many devices are known;
 * "macwatch on|off" and "macwatch ports <ports>" are startup-configuration
 * lines.
 */

#pragma codeseg BANK3
#pragma constseg BANK3

#include <stdint.h>
#include "rtl837x_common.h"
#include "rtl837x_sfr.h"
#include "rtl837x_regs.h"
#include "machine.h"
#include "kadam.h"

extern __code const struct machine machine;
extern __xdata uint8_t sfr_data[4];
extern __xdata uint8_t cmd_words_len;

#define MW_SLOTS	512	/* a power of two */
#define MW_STEPS	16	/* table reads per second */
#define MW_EMPTY	0xff	/* port value of an unused slot */

static __xdata uint8_t mw_mac[MW_SLOTS][6];
static __xdata uint16_t mw_vid[MW_SLOTS];
static __xdata uint8_t mw_port[MW_SLOTS];
static __xdata uint16_t mw_known;
static __xdata uint8_t mw_on;
static __xdata uint16_t mw_ports;	/* watched logical ports, a bit each */
static __xdata uint8_t mw_baseline;	/* 1 until the first walk completes */
static __xdata uint8_t mw_full;
static __xdata uint16_t mw_entry;	/* next table index to ask for */
static __xdata uint16_t mw_i, mw_slot;
static __xdata uint16_t mw_idx;		/* index of the entry just read */
static __xdata uint8_t mw_step, mw_n, mw_valid, mw_static;
static __xdata uint8_t mw_cur_mac[6];
static __xdata uint16_t mw_cur_vid;
static __xdata uint8_t mw_cur_port;

static void mw_forget(void)
{
	for (mw_i = 0; mw_i < MW_SLOTS; mw_i++)
		mw_port[mw_i] = MW_EMPTY;
	mw_known = 0;
	mw_full = 0;
	mw_baseline = 1;
	mw_entry = 0;
}

void macwatch_init(void)
{
	mw_on = 1;
	mw_ports = 0xffff;
	mw_forget();
}

static void mw_print_mac(void)
{
	for (mw_n = 0; mw_n < 6; mw_n++) {
		if (mw_n)
			write_char(':');
		print_byte(mw_cur_mac[mw_n]);
	}
	print_string(" vlan ");
	itoa_short(mw_cur_vid);
}

/* The slot holding mw_cur_mac/vid, or the free slot it would go in; 0xffff
 * when the table is full and it isn't there */
static void mw_find(void)
{
	mw_slot = ((uint16_t)(mw_cur_mac[4] & 1) << 8 | mw_cur_mac[5]) ^ (mw_cur_vid & 0x1ff);
	for (mw_i = 0; mw_i < MW_SLOTS; mw_i++) {
		if (mw_port[mw_slot] == MW_EMPTY)
			return;
		if (mw_vid[mw_slot] == mw_cur_vid) {
			for (mw_n = 0; mw_n < 6 && mw_mac[mw_slot][mw_n] == mw_cur_mac[mw_n]; mw_n++)
				;
			if (mw_n == 6)
				return;
		}
		mw_slot = (mw_slot + 1) & (MW_SLOTS - 1);
	}
	mw_slot = 0xffff;
}

static void mw_seen(void)
{
	mw_find();
	if (mw_slot == 0xffff) {
		if (!mw_full) {
			mw_full = 1;
			print_string("mac: 512 devices known; new ones are no longer tracked\n");
		}
		return;
	}
	if (mw_port[mw_slot] == MW_EMPTY) {
		for (mw_n = 0; mw_n < 6; mw_n++)
			mw_mac[mw_slot][mw_n] = mw_cur_mac[mw_n];
		mw_vid[mw_slot] = mw_cur_vid;
		mw_port[mw_slot] = mw_cur_port;
		mw_known++;
		if (!mw_baseline && ((mw_ports >> mw_cur_port) & 1)) {
			print_string("mac: new ");
			mw_print_mac();
			print_string(" on port ");
			kc_out_port = mw_cur_port;
			kc_print_port();
			write_char('\n');
		}
		return;
	}
	if (mw_port[mw_slot] == mw_cur_port)
		return;
	if (!mw_baseline && (((mw_ports >> mw_cur_port) & 1) || ((mw_ports >> mw_port[mw_slot]) & 1))) {
		print_string("mac: ");
		mw_print_mac();
		print_string(" moved from port ");
		kc_out_port = mw_port[mw_slot];
		kc_print_port();
		print_string(" to port ");
		kc_out_port = mw_cur_port;
		kc_print_port();
		write_char('\n');
	}
	mw_port[mw_slot] = mw_cur_port;
}

/* One next-entry read of the L2 table; 0 when the walk is complete */
static uint8_t mw_read_next(void)
{
	do {
		reg_read_m(RTL837X_TBL_CTRL);
	} while (sfr_data[3] & TBL_EXECUTE);

	reg_read_m(RTL837x_TBL_DATA_0);
	REG_WRITE(RTL837x_TBL_DATA_0, sfr_data[0], sfr_data[1] & 0xfc,
		  sfr_data[2] | (TBL_LUTREAD_NEXT_L2UC << 6), sfr_data[3]);
	REG_WRITE(RTL837X_TBL_CTRL, mw_entry >> 8, mw_entry, TBL_L2_UNICAST, TBL_EXECUTE);
	do {
		reg_read_m(RTL837X_TBL_CTRL);
	} while (sfr_data[3] & TBL_EXECUTE);

	reg_read_m(RTL837x_L2_DATA_OUT_B);
	mw_valid = sfr_data[0] & 0x20;
	if (mw_valid) {
		mw_cur_vid = ((uint16_t)(sfr_data[0] & 0x0f) << 8) | sfr_data[1];
		mw_cur_mac[0] = sfr_data[2];
		mw_cur_mac[1] = sfr_data[3];
		mw_cur_port = (sfr_data[0] >> 6) & 0x3;
		reg_read_m(RTL837x_L2_DATA_OUT_A);
		mw_cur_mac[2] = sfr_data[0];
		mw_cur_mac[3] = sfr_data[1];
		mw_cur_mac[4] = sfr_data[2];
		mw_cur_mac[5] = sfr_data[3];
		reg_read_m(RTL837x_L2_DATA_OUT_C);
		mw_static = sfr_data[1] & 0x1;
		mw_cur_port |= (sfr_data[3] & 0x3) << 2;
	}

	reg_read_m(RTL837x_TBL_DATA_0);
	mw_idx = (((uint16_t)sfr_data[2] & 0x0f) << 8) | sfr_data[3];
	if (!mw_valid || mw_idx < mw_entry)
		return 0;	/* an empty table, or the answer wrapped: done */
	if (!mw_static && mw_cur_port >= machine.min_port && mw_cur_port <= machine.max_port)
		mw_seen();
	if (mw_idx == 0xfff)
		return 0;	/* the last index: done */
	mw_entry = mw_idx + 1;
	return 1;
}

void macwatch_second(void)
{
	if (!mw_on)
		return;
	for (mw_step = 0; mw_step < MW_STEPS; mw_step++) {
		if (mw_read_next())
			continue;
		/* A walk is complete */
		if (mw_baseline) {
			mw_baseline = 0;
			print_string("mac: baseline, ");
			itoa_short(mw_known);
			print_string(" devices known\n");
		}
		mw_entry = 0;
		break;
	}
}

static void mw_print_ports(void)
{
	for (mw_n = machine.min_port; mw_n <= machine.max_port; mw_n++) {
		if ((mw_ports >> mw_n) & 1) {
			write_char(' ');
			kc_out_port = mw_n;
			kc_print_port();
		}
	}
}

void macwatch_cmd(void)
{
	if (kc_is(1, "on")) {
		if (!mw_on)
			mw_forget();
		mw_on = 1;
	} else if (kc_is(1, "off")) {
		mw_on = 0;
	} else if (kc_is(1, "ports")) {
		if (cmd_words_len < 3) {
			kc_usage("Usage: macwatch ports <port> [<port> ...]\n");
			return;
		}
		mw_slot = 0;
		for (mw_step = 2; mw_step < cmd_words_len; mw_step++) {
			if (!kc_port(mw_step)) {
				kc_usage("Usage: macwatch ports <port> [<port> ...], ports 1-9\n");
				return;
			}
			mw_slot |= 1 << kc_val;
		}
		mw_ports = mw_slot;
	} else if (cmd_words_len > 1) {
		kc_usage("Usage: macwatch [on|off|ports <port> ...]\n");
		return;
	}
	print_string(mw_on ? "macwatch: on, watching ports" : "macwatch: off, watching ports");
	mw_print_ports();
	print_string("; ");
	itoa_short(mw_known);
	print_string(mw_baseline && mw_on ? " devices known (baseline still being read)\n"
					  : " devices known\n");
}
