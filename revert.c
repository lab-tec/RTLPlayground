/*
 * The safe-change timer: "revert in <minutes>" before a risky change, in the
 * manner of Juniper's "commit confirmed" or Cisco's "reload in".
 *
 * Changes made in the web interface or on the console take effect at once
 * but live only in RAM until the configuration is saved. If a change cuts
 * off the management path, nobody can save it or undo it; when the timer
 * runs out the switch reboots, and the boot replays the saved configuration,
 * so it comes back as it was before the change. Saving the configuration
 * (revert_saved(), from the web server) or "revert cancel" disarms it.
 *
 * In the last five minutes it says so once a minute, and the reboot waits
 * two seconds after its last message so the syslog line gets out.
 */

#pragma codeseg BANK3
#pragma constseg BANK3

#include <stdint.h>
#include "rtl837x_common.h"
#include "kadam.h"

#define REVERT_MAX_MIN	60
#define REVERT_GRACE	2	/* seconds between the last message and the reboot */

static __xdata uint16_t rv_left;	/* seconds to the reboot; 0: not armed */
static __xdata uint8_t rv_sec;		/* seconds into the current minute */
static __xdata uint8_t rv_fire;		/* counting down to the reset itself */
static __xdata uint16_t rv_s;
static __xdata uint8_t rv_m;

void revert_init(void)
{
	rv_left = 0;
	rv_fire = 0;
}

/* Minutes and seconds by subtraction, without the library's division */
static void rv_print_left(void)
{
	rv_s = rv_left;
	for (rv_m = 0; rv_s >= 60; rv_m++)
		rv_s -= 60;
	itoa_short(rv_m);
	print_string(" min ");
	itoa_short(rv_s);
	print_string(" s");
}

void revert_second(void)
{
	if (rv_fire) {
		if (!--rv_fire) {
			reboot_note(REBOOT_REVERT);
			reset_chip();
		}
		return;
	}
	if (!rv_left)
		return;
	if (!--rv_left) {
		print_string("revert: not confirmed in time; rebooting into the saved config\n");
		rv_fire = REVERT_GRACE;
		return;
	}
	if (++rv_sec == 60)
		rv_sec = 0;
	if (rv_left <= 300 && !rv_sec) {
		print_string("revert: rebooting into the saved config in ");
		rv_print_left();
		print_string(" unless the config is saved or \"revert cancel\"\n");
	}
}

void revert_saved(void) __banked
{
	if (!rv_left && !rv_fire)
		return;
	rv_left = 0;
	rv_fire = 0;
	print_string("revert: config saved; timer cancelled\n");
}

void revert_cmd(void)
{
	if (kc_is(1, "cancel")) {
		if (rv_left || rv_fire) {
			rv_left = 0;
			rv_fire = 0;
			print_string("revert: cancelled\n");
		} else {
			print_string("revert: not armed\n");
		}
		return;
	}
	if (kc_is(1, "in")) {
		if (!kc_num(2) || !kc_val || kc_val > REVERT_MAX_MIN) {
			kc_usage("Usage: revert in <1-60 minutes> | revert cancel | revert\n");
			return;
		}
		rv_left = (kc_val << 6) - (kc_val << 2);	/* minutes * 60 */
		rv_sec = 0;
		rv_fire = 0;
		print_string("revert: armed; rebooting into the saved config in ");
		rv_print_left();
		print_string(" unless the config is saved or \"revert cancel\"\n");
		return;
	}
	if (cmd_words_len > 1) {
		kc_usage("Usage: revert in <1-60 minutes> | revert cancel | revert\n");
		return;
	}
	if (rv_left) {
		print_string("revert: armed, ");
		rv_print_left();
		print_string(" left\n");
	} else {
		print_string("revert: not armed\n");
	}
}
