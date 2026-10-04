/*
 * The "cable" command: the PHY's own cable test (Realtek calls it RTCT). The
 * PHY sends test pulses down each of the four pairs and reports, per pair,
 * whether the cable is fine, open or shorted, and how far away the fault is.
 *
 * The register sequence is the RTL8224's, from the Linux driver
 * (drivers/net/phy/realtek/realtek_main.c, rtl8224_cable_test_*). It runs
 * only on a PHY that identifies itself as an RTL8224: a different PHY gets
 * its ID printed and nothing written, rather than registers that may mean
 * something else there.
 *
 * The test needs the link down: the PHY is forced to 1000/full with
 * auto-negotiation off for the duration, then put back as it was. So a port
 * with a link is refused unless "force" is given. The main loop pauses for
 * the few seconds it takes.
 */

#pragma codeseg BANK3
#pragma constseg BANK3

#include <stdint.h>
#include "rtl837x_common.h"
#include "rtl837x_sfr.h"
#include "rtl837x_regs.h"
#include "machine.h"
#include "phy.h"
#include "cable.h"

extern __code const struct machine machine;

/* Clause 22 registers, as the RTL822x family maps them into MMD 31 */
#define CABLE_C22(reg)		(0xa400 + 2 * (reg))
#define CABLE_BMCR		CABLE_C22(0)
#define CABLE_BMCR_SPEED1000	0x0040
#define CABLE_BMCR_FULLDPLX	0x0100
#define CABLE_BMCR_ANRESTART	0x0200
#define CABLE_BMCR_ANENABLE	0x1000
#define CABLE_BMCR_SPEED100	0x2000
#define CABLE_PHYID1		CABLE_C22(2)
#define CABLE_PHYID2		CABLE_C22(3)
#define CABLE_RTCT		CABLE_C22(0x11)
#define CABLE_RTCT_START	0x00f1	/* enable, all four pairs */
#define CABLE_RTCT_DONE		0x8000
#define CABLE_SRAM_ADDR		CABLE_C22(0x1b)
#define CABLE_SRAM_DATA		CABLE_C22(0x1c)
#define CABLE_SRAM_FAULT(pair)	(0x8026 + (pair) * 4)
#define CABLE_SRAM_LEN(pair)	(0x8028 + (pair) * 4)
#define CABLE_FAULT_BUSY	0x01
#define CABLE_FAULT_OPEN	0x08
#define CABLE_FAULT_SAME_SHORT	0x10
#define CABLE_FAULT_OK		0x20
#define CABLE_FAULT_DONE	0x40
#define CABLE_FAULT_CROSS_SHORT	0x80
/* The RTL8224's PHY identifier */
#define CABLE_ID1		0x001c
#define CABLE_ID2		0xcad0
/* The PHY's power-down bit, as phy_reset() uses it */
#define CABLE_PHY_CTRL		0xa610
#define CABLE_PHY_OFF		0x0800
/* Ticks of 5 ms: the settle before the test, and the most it may take */
#define CABLE_SETTLE		100
#define CABLE_POLL		20
#define CABLE_POLLS		100

static __xdata uint8_t cable_port;
static __xdata uint16_t cable_bmcr;
static __xdata uint16_t cable_v;
static __xdata uint16_t cable_len;
static __xdata uint8_t cable_pair;
static __xdata uint8_t cable_n;
static __xdata uint8_t cable_tenth;

/* One word of the PHY's result memory into cable_v */
static void cable_sram(uint16_t addr)
{
	phy_write(cable_port, PHY_MMD31, CABLE_SRAM_ADDR, addr);
	phy_read(cable_port, PHY_MMD31, CABLE_SRAM_DATA);
	cable_v = SFR_DATA_U16;
}

/* Distance to the fault on cable_pair: the PHY's count, less its 620 offset,
 * at 78 per metre (the Linux driver's conversion), printed as metres. */
static void cable_distance(void)
{
	cable_sram(CABLE_SRAM_LEN(cable_pair));
	cable_len = cable_v & 0xff00;
	cable_sram(CABLE_SRAM_LEN(cable_pair) + 1);
	cable_len |= cable_v >> 8;
	cable_len = cable_len > 620 ? cable_len - 620 : 0;

	/* By subtraction: division would pull library code wanting IRAM */
	cable_v = 0;
	while (cable_len >= 78) {
		cable_len -= 78;
		cable_v++;
	}
	cable_len *= 10;
	cable_tenth = 0;
	while (cable_len >= 78) {
		cable_len -= 78;
		cable_tenth++;
	}
	print_string(" at ");
	itoa_short(cable_v);
	write_char('.');
	write_char('0' + cable_tenth);
	print_string(" m");
}

static void cable_report(void)
{
	for (cable_pair = 0; cable_pair < 4; cable_pair++) {
		print_string("Pair ");
		write_char('A' + cable_pair);
		print_string(": ");
		cable_sram(CABLE_SRAM_FAULT(cable_pair));
		if (!(cable_v & CABLE_FAULT_DONE)) {
			print_string("no result\n");
			continue;
		}
		if (cable_v & CABLE_FAULT_OK) {
			print_string("OK\n");
			continue;
		}
		if (cable_v & CABLE_FAULT_OPEN)
			print_string("open");
		else if (cable_v & CABLE_FAULT_SAME_SHORT)
			print_string("short within the pair");
		else if (cable_v & CABLE_FAULT_BUSY) {
			print_string("unknown\n");
			continue;
		} else if (cable_v & CABLE_FAULT_CROSS_SHORT)
			print_string("short to another pair");
		else {
			print_string("unknown\n");
			continue;
		}
		cable_distance();
		write_char('\n');
	}
}

/* arg: the logical port, plus CABLE_FORCE to test a port with a link */
void cable_test(uint8_t arg) __banked
{
	cable_port = arg & ~CABLE_FORCE;

	if (machine.is_sfp[cable_port]) {
		print_string("cable: not on an SFP port\n");
		return;
	}

	phy_read(cable_port, PHY_MMD31, CABLE_PHYID1);
	cable_v = SFR_DATA_U16;
	phy_read(cable_port, PHY_MMD31, CABLE_PHYID2);
	if (cable_v != CABLE_ID1 || SFR_DATA_U16 != CABLE_ID2) {
		print_string("cable: this port's PHY is ");
		print_short(cable_v);
		write_char(' ');
		print_short(SFR_DATA_U16);
		print_string(", not an RTL8224 (0x001c 0xcad0); no documented cable test for it\n");
		return;
	}

	phy_read(cable_port, PHY_MMD31, CABLE_PHY_CTRL);
	if (SFR_DATA_U16 & CABLE_PHY_OFF) {
		print_string("cable: the port is switched off\n");
		return;
	}

	reg_read(RTL837X_REG_LINKS_STS);
	if ((((SFR_DATA_16 | ((uint16_t)SFR_DATA_8 << 8)) >> cable_port) & 1) && !(arg & CABLE_FORCE)) {
		print_string("cable: the port has a link, which the test takes down for a few seconds.\n"
			     "Use \"cable <port> force\" to test it anyway.\n");
		return;
	}

	print_string("cable: testing, a few seconds; management pauses meanwhile\n");

	/* Force 1000/full with auto-negotiation off, as the Linux driver does */
	phy_read(cable_port, PHY_MMD31, CABLE_BMCR);
	cable_bmcr = SFR_DATA_U16;
	phy_write(cable_port, PHY_MMD31, CABLE_BMCR,
		  (cable_bmcr & ~(CABLE_BMCR_ANENABLE | CABLE_BMCR_SPEED100))
		  | CABLE_BMCR_SPEED1000 | CABLE_BMCR_FULLDPLX);
	delay(CABLE_SETTLE);

	phy_read(cable_port, PHY_MMD31, CABLE_RTCT);
	phy_write(cable_port, PHY_MMD31, CABLE_RTCT,
		  (SFR_DATA_U16 & ~CABLE_RTCT_DONE) | CABLE_RTCT_START);

	for (cable_n = 0; cable_n < CABLE_POLLS; cable_n++) {
		delay(CABLE_POLL);
		phy_read(cable_port, PHY_MMD31, CABLE_RTCT);
		if (SFR_DATA_U16 & CABLE_RTCT_DONE)
			break;
	}
	if (cable_n == CABLE_POLLS)
		print_string("cable: the test did not finish\n");
	else
		cable_report();

	/* Put the port back as it was, renegotiating if it negotiated before */
	if (cable_bmcr & CABLE_BMCR_ANENABLE)
		cable_bmcr |= CABLE_BMCR_ANRESTART;
	phy_write(cable_port, PHY_MMD31, CABLE_BMCR, cable_bmcr);
}
