/*
 * test_cable.c: the cable command against a model of the RTL8224's registers.
 *
 * cable.c is compiled unmodified. phy_read()/phy_write() land in a register
 * file per PHY, in MMD 31, with the parts of the RTL8224 the test touches made
 * live: the cable-test register reports done a set number of polls after it
 * is started, and the result memory answers through the SRAM address and data
 * registers with values the test chooses. Every write is logged, so the
 * sequence can be compared with the Linux driver's
 * (drivers/net/phy/realtek/realtek_main.c, rtl8224_cable_test_*).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "rtl837x_common.h"
#include "rtl837x_sfr.h"
#include "rtl837x_regs.h"
#include "machine.h"
#include "phy.h"
#include "cable.h"
#include "hw_mock.h"

static int fails, checks;
#define CHECK(c, msg) do { checks++; if (!(c)) { fails++; \
	printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, msg); } } while (0)

/* ---- what cable.c links against ---- */
const struct machine machine = {
	.machine_name = "HOSTTEST",
	.min_port = 0,
	.max_port = 8,
	.log_to_phys_port = { 1, 2, 3, 4, 5, 6, 7, 8, 9 },
	.phys_to_log_port = { 0, 1, 2, 3, 4, 5, 6, 7, 8 },
	.is_sfp = { 0, 0, 0, 0, 0, 0, 0, 0, 1 },
};

/* The PHY model: MMD 31 of eight PHYs, the result memory of each */
static uint16_t mmd31[8][0x10000];
static uint16_t sram[8][0x10000];
static int done_after;		/* polls until the test reports done; <0: never */
static int polls, ticks_waited;

#define MAXW 64
static struct { uint8_t phy; uint16_t reg, val; } wlog[MAXW];
static int nw;

void phy_write(uint8_t phy_id, uint8_t dev_id, uint16_t reg, uint16_t v)
{
	if (nw < MAXW) { wlog[nw].phy = phy_id; wlog[nw].reg = reg; wlog[nw].val = v; nw++; }
	CHECK(dev_id == PHY_MMD31 && phy_id < 8, "writes go to MMD 31 of a front-panel PHY");
	mmd31[phy_id][reg] = v;
	if (reg == 0xa422 && (v & 1))
		polls = 0;
}
void phy_read(uint8_t phy_id, uint8_t dev_id, uint16_t reg)
{
	CHECK(dev_id == PHY_MMD31 && phy_id < 8, "reads come from MMD 31 of a front-panel PHY");
	if (reg == 0xa438) {
		SFR_DATA_U16 = sram[phy_id][mmd31[phy_id][0xa436]];
		return;
	}
	if (reg == 0xa422 && (mmd31[phy_id][0xa422] & 1)) {
		if (done_after >= 0 && polls++ >= done_after)
			mmd31[phy_id][0xa422] |= 0x8000;
	}
	SFR_DATA_U16 = mmd31[phy_id][reg];
}
void delay(uint16_t t) { ticks_waited += t; }

static char out[4096];
static void put(const char *s) { strncat(out, s, sizeof(out) - strlen(out) - 1); }
void print_string(const char *p) { put(p); }
void write_char(char c) { char s[2] = { c, 0 }; put(s); }
void print_short(uint16_t a) { char s[7]; snprintf(s, sizeof(s), "0x%04x", a); put(s); }
void itoa_short(uint16_t v) { char s[6]; snprintf(s, sizeof(s), "%u", v); put(s); }

/* ---- helpers ---- */
#define BMCR_AN_2G5	0x1040		/* auto-negotiation on, as the switch runs a port */

static void reset(void)
{
	memset(mmd31, 0, sizeof(mmd31));
	memset(sram, 0, sizeof(sram));
	for (int p = 0; p < 8; p++) {
		mmd31[p][0xa404] = 0x001c;
		mmd31[p][0xa406] = 0xcad0;
		mmd31[p][0xa400] = BMCR_AN_2G5;
	}
	hw_reset();
	done_after = 3;
	ticks_waited = 0;
	nw = 0;
	out[0] = 0;
}

/* A pair's result: the fault byte, and the raw 16-bit length count, which
 * the PHY keeps as the high bytes of two words */
static void result(int phy, int pair, uint16_t fault, uint16_t count)
{
	sram[phy][0x8026 + pair * 4] = fault;
	sram[phy][0x8028 + pair * 4] = count & 0xff00;
	sram[phy][0x8028 + pair * 4 + 1] = (count & 0xff) << 8;
}

static int has(const char *s) { return strstr(out, s) != NULL; }
static int wrote(uint16_t reg) { for (int i = 0; i < nw; i++) if (wlog[i].reg == reg) return 1; return 0; }

int main(void)
{
	puts("--- all four pairs open (nothing plugged in), the Linux sequence");
	reset();
	for (int pair = 0; pair < 4; pair++)
		result(2, pair, 0x48, 620);	/* done + open, at the zero point */
	cable_test(2);
	printf("%s", out);
	CHECK(nw >= 3, "three writes at least");
	CHECK(wlog[0].phy == 2 && wlog[0].reg == 0xa400 && wlog[0].val == 0x0140,
	      "first: BMCR forced to 1000/full, auto-negotiation off (0x0140 from 0x1040)");
	CHECK(ticks_waited >= 100, "500 ms settle before the test, as Linux waits");
	CHECK(wlog[1].reg == 0xa422 && wlog[1].val == 0x00f1, "then RTCT: enable, pairs A-D, done cleared");
	CHECK(has("Pair A: open at 0.0 m\nPair B: open at 0.0 m\nPair C: open at 0.0 m\nPair D: open at 0.0 m\n"),
	      "four open pairs at 0 m");
	CHECK(wlog[nw - 1].reg == 0xa400 && wlog[nw - 1].val == (BMCR_AN_2G5 | 0x0200),
	      "last: BMCR back as it was, renegotiating");

	puts("--- distances, shorts and OK");
	reset();
	result(4, 0, 0x60, 0);			/* OK: no distance read */
	result(4, 1, 0x48, 620 + 78 * 23 + 39);	/* open at 23.5 m */
	result(4, 2, 0x50, 620 + 78 * 60);	/* short within the pair at 60 m */
	result(4, 3, 0xc0, 620 + 7);		/* cross short, 0.0 m (7/78 m rounds down) */
	cable_test(4);
	printf("%s", out);
	CHECK(has("Pair A: OK\n"), "OK pair");
	CHECK(has("Pair B: open at 23.5 m\n"), "open with distance in tenths of a metre");
	CHECK(has("Pair C: short within the pair at 60.0 m\n"), "same-pair short");
	CHECK(has("Pair D: short to another pair at 0.0 m\n"), "cross short");

	puts("--- a count below the 620 offset reads as 0 m");
	reset();
	result(1, 0, 0x48, 100);
	result(1, 1, 0x60, 0); result(1, 2, 0x60, 0); result(1, 3, 0x60, 0);
	cable_test(1);
	CHECK(has("Pair A: open at 0.0 m\n"), "clamped at zero");

	puts("--- a pair without a result, and an unknown code");
	reset();
	result(3, 0, 0x08, 0);			/* not done */
	result(3, 1, 0x41, 0);			/* done + busy */
	result(3, 2, 0x40, 0);			/* done, nothing else */
	result(3, 3, 0x60, 0);
	cable_test(3);
	printf("%s", out);
	CHECK(has("Pair A: no result\n"), "no done bit");
	CHECK(has("Pair B: unknown\nPair C: unknown\nPair D: OK\n"), "busy and empty codes are unknown");

	puts("--- the test never finishes: reported, the port still restored");
	reset();
	done_after = -1;
	cable_test(0);
	printf("%s", out);
	CHECK(has("cable: the test did not finish\n"), "timeout reported");
	CHECK(!has("Pair"), "no results printed");
	CHECK(ticks_waited >= 100 + 1000 && ticks_waited <= 100 + 2100, "gives up after about 10 s");
	CHECK(wlog[nw - 1].reg == 0xa400 && wlog[nw - 1].val == (BMCR_AN_2G5 | 0x0200), "restored anyway");

	puts("--- a port with a link is refused, and nothing is written");
	reset();
	hw_reg_set(RTL837X_REG_LINKS_STS, 1u << (16 + 5));	/* logical port 5 up */
	cable_test(5);
	printf("%s", out);
	CHECK(has("the port has a link") && has("cable <port> force"), "refused with the way round it");
	CHECK(nw == 0, "nothing written");

	puts("--- ...unless forced");
	reset();
	hw_reg_set(RTL837X_REG_LINKS_STS, 1u << (16 + 5));
	for (int pair = 0; pair < 4; pair++)
		result(5, pair, 0x60, 0);
	cable_test(5 | CABLE_FORCE);
	CHECK(has("Pair A: OK\nPair B: OK\nPair C: OK\nPair D: OK\n"), "forced test runs");
	CHECK(wrote(0xa422), "the test was started");

	puts("--- a forced-speed port is put back without renegotiating");
	reset();
	mmd31[6][0xa400] = 0x0140;
	for (int pair = 0; pair < 4; pair++)
		result(6, pair, 0x60, 0);
	cable_test(6);
	CHECK(wlog[nw - 1].reg == 0xa400 && wlog[nw - 1].val == 0x0140, "BMCR restored unchanged");

	puts("--- a PHY that isn't an RTL8224: its ID shown, nothing written");
	reset();
	mmd31[7][0xa406] = 0xc840;
	cable_test(7);
	printf("%s", out);
	CHECK(has("this port's PHY is 0x001c 0xc840, not an RTL8224"), "the foreign PHY's ID is printed");
	CHECK(nw == 0, "nothing written");

	puts("--- a port switched off");
	reset();
	mmd31[1][0xa610] = 0x0800;
	cable_test(1);
	CHECK(has("cable: the port is switched off\n") && nw == 0, "refused, nothing written");

	puts("--- the SFP port");
	reset();
	cable_test(8);
	CHECK(has("cable: not on an SFP port\n") && nw == 0, "refused, nothing written");

	printf("test_cable: %d checks, %d failed\n", checks, fails);
	return fails ? 1 : 0;
}
