/*
 * The "bench" command: how fast the 8051 core and its path to the switch
 * registers actually are, measured on the device.
 *
 * Each test repeats one piece of work for one second, timed by the system
 * tick, and prints how many times it ran. The first is a DJNZ loop of known
 * length, which gives the core's clocks per instruction directly; the others
 * are the kinds of work features put on the CPU. Altogether the command takes
 * about five seconds, during which the main loop does not run: the web
 * interface and console pause, forwarding does not.
 */

#pragma codeseg BANK3
#pragma constseg BANK3

#include <stdint.h>
#include "rtl837x_sfr.h"
#include "rtl837x_regs.h"
#include "rtl837x_common.h"
#include "rtl837x_port.h"
#include "bench.h"

/* Below 256, so sleep_ticks never has a high byte to tear while it is read */
#define BENCH_TICKS	SYS_TICK_HZ
#define BENCH_CLOCK_HZ	125000000UL
/* DJNZ instructions per bench_djnz() call: 256 x 256 inner, 256 outer */
#define BENCH_DJNZ	(256UL * 256UL + 256UL)

extern volatile __xdata uint16_t sleep_ticks;
extern __xdata uint8_t sfr_data[4];

static __xdata uint32_t bench_count;
static __xdata uint32_t bench_val;
static __xdata uint8_t bench_src[256];
static __xdata uint8_t bench_dst[256];
static __xdata uint32_t bench_num;	/* argument of bench_dec() */
static __xdata uint8_t bench_i;
static __xdata uint32_t bench_p;

/* Decimal printing without 32-bit division: it would pull library routines
 * whose arguments need internal RAM, of which there is none left. */
static __code const uint32_t bench_pow10[] = {
	1000000000UL, 100000000UL, 10000000UL, 1000000UL, 100000UL,
	10000UL, 1000UL, 100UL, 10UL, 1UL
};

static void bench_djnz(void) __naked
{
	__asm
	mov	r6, #0
00002$:
	mov	r7, #0
00001$:
	djnz	r7, 00001$
	djnz	r6, 00002$
	ret
	__endasm;
}

/* Waits for a tick edge, then arms one second */
static void bench_start(void)
{
	bench_count = 0;
	sleep_ticks = 1;
	while (sleep_ticks)
		;
	sleep_ticks = BENCH_TICKS;
}

/*
 * One decimal digit of bench_num: how many bench_p fit, which are taken off.
 * A leaf on purpose, so the compiler's 32-bit temporary lands in the shared
 * overlay area instead of claiming internal RAM of its own.
 */
static uint8_t bench_digit(void)
{
	__xdata uint8_t digit = '0';

	while (bench_num >= bench_p) {
		bench_num -= bench_p;
		digit++;
	}
	return digit;
}

/* Prints bench_num in decimal */
static void bench_dec(void)
{
	__xdata uint8_t digit;
	__xdata uint8_t started = 0;

	for (bench_i = 0; bench_i < sizeof(bench_pow10) / sizeof(bench_pow10[0]); bench_i++) {
		bench_p = bench_pow10[bench_i];
		digit = bench_digit();
		if (digit != '0' || started || bench_i == 9) {
			write_char(digit);
			started = 1;
		}
	}
}

static void bench_line(__code const char *what)
{
	print_string(what);
	bench_num = bench_count;
	bench_dec();
	print_string(" /s");
}

void bench_run(void) __banked
{
	print_string("bench: about 5 s, management pauses meanwhile\n");

	bench_start();
	while (sleep_ticks) {
		bench_djnz();
		bench_count++;
	}
	bench_count *= BENCH_DJNZ;
	bench_line("DJNZ instructions:   ");
	/* Clocks per DJNZ in tenths, by subtraction: a few hundred at most */
	bench_val = BENCH_CLOCK_HZ * 10;
	bench_num = 0;
	while (bench_count && bench_val >= bench_count) {
		bench_val -= bench_count;
		bench_num++;
	}
	/* Split into whole clocks and the tenth */
	bench_val = 0;
	while (bench_num >= 10) {
		bench_num -= 10;
		bench_val++;
	}
	bench_i = bench_num;
	bench_num = bench_val;
	print_string("  (");
	bench_dec();
	write_char('.');
	write_char('0' + bench_i);
	print_string(" clocks each at 125 MHz)\n");

	bench_start();
	bench_val = 1;
	while (sleep_ticks) {
		bench_val = bench_val * 1664525UL + 1013904223UL;
		bench_count++;
	}
	bench_line("32-bit mul + add:    ");
	write_char('\n');

	bench_start();
	while (sleep_ticks) {
		memcpy(bench_dst, bench_src, sizeof(bench_dst));
		bench_count++;
	}
	bench_line("256 B xdata copies:  ");
	write_char('\n');

	bench_start();
	while (sleep_ticks) {
		reg_read_m(RTL837X_REG_LINKS);
		bench_count++;
	}
	bench_line("switch register reads: ");
	write_char('\n');

	bench_start();
	bench_i = STAT_COUNTER_RX_PKTS;
	while (sleep_ticks) {
		STAT_GET(bench_i, 0);
		reg_read_m(RTL837X_STAT_V_LOW);
		bench_count++;
	}
	bench_line("port counter reads:  ");
	write_char('\n');
}
