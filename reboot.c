/*
 * Why the switch restarted: a marker written just before a software reset
 * and read back by the next boot (bootmsg.c).
 *
 * It sits at the top of the external RAM the build leaves free, at the same
 * address in every build, so an image reads what the one before it wrote.
 * Nothing clears external RAM at startup; whether the chip's reset does is
 * what the boot announcement shows on the device. A power cut loses it, so
 * no marker means "power-on, or a crash".
 *
 * Common area: reset paths in every bank call it, some before any bank
 * switching is set up.
 */

#include <stdint.h>
#include "rtl837x_common.h"
#include "kadam.h"

#define REBOOT_AT	0xbff0

__xdata __at(REBOOT_AT) uint8_t reboot_marker[6];

void reboot_note(uint8_t why)
{
	reboot_marker[0] = 'K';
	reboot_marker[1] = 'D';
	reboot_marker[2] = 'R';
	reboot_marker[3] = 'B';
	reboot_marker[4] = why;
	reboot_marker[5] = ~why;
}

/* The reason a marker holds, or REBOOT_NONE */
uint8_t reboot_why(void)
{
	if (reboot_marker[0] != 'K' || reboot_marker[1] != 'D' || reboot_marker[2] != 'R'
	    || reboot_marker[3] != 'B' || (uint8_t)~reboot_marker[4] != reboot_marker[5])
		return REBOOT_NONE;
	return reboot_marker[4];
}
