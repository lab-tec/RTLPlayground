/*
 * The boot announcement: thirty seconds after boot, when links are up and
 * syslog has its destination, one line saying what is running, why the
 * switch restarted and how warm it is:
 *
 *   boot: v0.1.0-kadam-D-1a2b3c4 up, reason: upgrade; temp 71.9 C
 *
 * The reason comes from the marker the previous image left behind
 * (reboot.c); it is read once, at kadam_init(), and cleared. "boot" on the
 * console prints the line again.
 */

#pragma codeseg BANK3
#pragma constseg BANK3

#include <stdint.h>
#include "rtl837x_common.h"
#include "version.h"
#include "kadam.h"

#define BOOTMSG_DELAY	30	/* seconds after boot */

extern __xdata uint8_t reboot_marker[6];

static __xdata uint8_t bm_reason;
static __xdata uint8_t bm_wait;

static __code const char * __code const bm_text[] = {
	"power-on (or a crash)",
	"reset",
	"the reset command",
	"the web interface's reboot",
	"a firmware upload",
	"upgrade",
	"rollback to the firmware backup",
	"the revert timer",
	"the reset button: default config restored",
};

void bootmsg_init(void)
{
	bm_reason = reboot_why();
	if (bm_reason >= sizeof(bm_text) / sizeof(bm_text[0]))
		bm_reason = REBOOT_NONE;
	reboot_marker[0] = 0;
	bm_wait = BOOTMSG_DELAY;
}

static void bm_print(void)
{
	print_string("boot: " VERSION_SW " up, reason: ");
	print_string(bm_text[bm_reason]);
	print_string("; temp ");
	thermal_print_now();
	write_char('\n');
}

void bootmsg_second(void)
{
	if (bm_wait && !--bm_wait)
		bm_print();
}

void bootmsg_cmd(void)
{
	bm_print();
}
