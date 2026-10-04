/*
 * The thermal guard: the chip's temperature against two thresholds, checked
 * once a second, with every crossing sent to the console and so to syslog.
 *
 * "thermal warn <C> crit <C>" sets them (85 and 95 by default) and is a
 * startup-configuration line. Crossing a threshold upwards logs at once;
 * while above, the reading is logged again every ten minutes; coming back
 * down logs once the temperature is 3 C below the threshold it crossed, so
 * a reading hovering at the line doesn't log every second. It only reports:
 * nothing is switched off.
 *
 * The sensor reads in 1/128 C (RTL837X_TM_RESULT), as the "temp" command
 * prints it.
 */

#pragma codeseg BANK3
#pragma constseg BANK3

#include <stdint.h>
#include "rtl837x_common.h"
#include "rtl837x_sfr.h"
#include "rtl837x_regs.h"
#include "kadam.h"

#define TH_WARN_DEFAULT	85
#define TH_CRIT_DEFAULT	95
#define TH_HYST		3	/* C below a threshold to count as back under it */
#define TH_REPEAT	600	/* seconds between reminders while above */

static __xdata uint8_t th_warn, th_crit;
static __xdata uint8_t th_level;	/* 0: fine, 1: above warn, 2: above crit */
static __xdata uint8_t th_new;
static __xdata uint16_t th_repeat;
static __xdata int16_t th_t;		/* 1/128 C */
static __xdata int16_t th_v;

void thermal_init(void)
{
	th_warn = TH_WARN_DEFAULT;
	th_crit = TH_CRIT_DEFAULT;
	th_level = 0;
}

static void th_read(void)
{
	reg_read(RTL837X_TM_RESULT);
	/* SFR_DATA_U16 is these two bytes; spelt out so the host's register
	 * model, which keeps them apart, reads the same */
	th_t = ((uint16_t)SFR_DATA_8 << 8) | SFR_DATA_0;
}

/* th_t as "73.7 C" */
static void th_print(void)
{
	th_v = th_t;
	if (th_v < 0) {
		write_char('-');
		th_v = -th_v;
	}
	itoa_short(th_v >> 7);
	write_char('.');
	itoa_short(((th_v & 0x7f) * 10) >> 7);
	print_string(" C");
}

/* The current reading, for the boot announcement */
void thermal_print_now(void)
{
	th_read();
	th_print();
}

static void th_report(void)
{
	print_string("temp: ");
	th_print();
	if (th_level == 2) {
		print_string(", above the critical threshold ");
		itoa_short(th_crit);
	} else {
		print_string(", above the warning threshold ");
		itoa_short(th_warn);
	}
	print_string(" C\n");
}

void thermal_second(void)
{
	th_read();

	/* Upwards: the thresholds themselves */
	th_new = 0;
	if (th_t >= (int16_t)th_warn << 7)
		th_new = 1;
	if (th_t >= (int16_t)th_crit << 7)
		th_new = 2;
	/* Downwards only once clear of the line by TH_HYST */
	if (th_new < th_level) {
		if (th_level == 2 && th_t >= (int16_t)(th_crit - TH_HYST) << 7)
			th_new = 2;
		else if (th_new == 0 && th_t >= (int16_t)(th_warn - TH_HYST) << 7)
			th_new = 1;
	}

	if (th_new > th_level) {
		th_level = th_new;
		th_repeat = TH_REPEAT;
		th_report();
	} else if (th_new < th_level) {
		th_level = th_new;
		th_repeat = TH_REPEAT;
		print_string("temp: back to ");
		th_print();
		print_string(th_level ? ", below the critical threshold\n" : ", below the warning threshold\n");
	} else if (th_level && !--th_repeat) {
		th_repeat = TH_REPEAT;
		th_report();
	}
}

void thermal_cmd(void)
{
	if (cmd_words_len > 1) {
		if (cmd_words_len != 5 || !kc_is(1, "warn") || !kc_is(3, "crit")
		    || !kc_num(2)) {
			kc_usage("Usage: thermal [warn <C> crit <C>]\n");
			return;
		}
		th_new = kc_val;
		if (!kc_num(4) || th_new < 40 || kc_val > 125 || kc_val <= th_new || kc_val - th_new < TH_HYST) {
			kc_usage("Usage: thermal warn <40-124> crit <warn+3-125>\n");
			return;
		}
		th_warn = th_new;
		th_crit = kc_val;
		th_level = 0;
	}
	th_read();
	print_string("thermal: ");
	th_print();
	print_string(" now; warn ");
	itoa_short(th_warn);
	print_string(" C, crit ");
	itoa_short(th_crit);
	print_string(th_level == 2 ? " C; state CRITICAL\n" : th_level ? " C; state WARNING\n" : " C; state ok\n");
}
