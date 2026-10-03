/*
 * The "memtest" command: checks the external RAM (XRAM) the firmware leaves
 * free, up to the 48 kB the linker is told about (--xram-size 49151).
 *
 * The build allocates XRAM from the bottom up; everything from the end of
 * that allocation to 0xbfff is free, and that free part is what the test
 * covers. Upstream measured the 48 kB on another RTL837x switch (commit
 * eb4bae2); this measures it on the switch at hand, without disturbing what
 * is in use.
 *
 * First, copies. On a chip with less RAM than that, an address in the free
 * part can be a second name for one 16 or 32 kB below it, which is in use.
 * Each free byte is written while the byte below is watched, with interrupts
 * held off and the byte below put back before they resume. A copy stops the
 * test there, before anything else is written.
 *
 * Then patterns: two, one the inverse of the other and different at
 * neighbouring addresses, are written across the free part and read back,
 * so every bit is seen holding both a 0 and a 1. The free part is left
 * zeroed.
 *
 * Takes a second or two, during which the main loop does not run: the
 * console and web interface pause, forwarding does not.
 */

#pragma codeseg BANK3
#pragma constseg BANK3

#include <stdint.h>
#include <8051.h>
#include "rtl837x_common.h"
#include "rtl837x_flash.h"
#include "memtest.h"

#ifdef RTLP_HOST_TEST
/* The host test supplies the RAM and the linker's figures */
__xdata uint8_t *memtest_byte(uint16_t a);
uint16_t memtest_xseg_end(void);
uint16_t memtest_xiseg_end(void);
#define MT(a)	(*memtest_byte(a))
#else
#define MT(a)	(*(volatile __xdata uint8_t *)(a))

/* The first address past the build's variables (XSEG) and past its
 * initialised ones (XISEG); the linker defines s_ and l_ for both. */
static uint16_t memtest_xseg_end(void) __naked
{
	__asm
	mov	a, #s_XSEG
	add	a, #l_XSEG
	mov	dpl, a
	mov	a, #(s_XSEG >> 8)
	addc	a, #(l_XSEG >> 8)
	mov	dph, a
	ret
	__endasm;
}

static uint16_t memtest_xiseg_end(void) __naked
{
	__asm
	mov	a, #s_XISEG
	add	a, #l_XISEG
	mov	dpl, a
	mov	a, #(s_XISEG >> 8)
	addc	a, #(l_XISEG >> 8)
	mov	dph, a
	ret
	__endasm;
}
#endif

/* The first address past the 48 kB */
#define MT_END		0xc000U
/* Differs between neighbours, and between addresses 16 or 32 kB apart */
#define MT_PATTERN(a)	((uint8_t)((a) >> 8) ^ (uint8_t)(a))

static __xdata uint16_t mt_start;
static __xdata uint16_t mt_a;
static __xdata uint16_t mt_b;
static __xdata uint8_t mt_found;
static __xdata uint8_t mt_inv;
static __xdata uint8_t mt_v;
static __xdata uint16_t mt_bad;		/* bytes read back wrong */
static __xdata uint16_t mt_first;	/* the first of them */
static __xdata uint8_t mt_wrote;	/* what it should have held */
static __xdata uint8_t mt_got;		/* and what it did */
static __xdata uint8_t mt_read;

/*
 * Writes a while watching b, and puts b back: true if b changed with a.
 * Everything stays in registers, so watching one of this file's own
 * variables cannot fool it, and interrupts are held off, so no handler sees
 * b while it holds the wrong value.
 */
static uint8_t mt_copy_of(uint16_t a, uint16_t b)
{
	uint8_t ea = EA;
	uint8_t save, v;

	EA = 0;
	save = MT(b);
	v = ~save;
	MT(a) = v;
	v = (MT(b) == v);
	MT(b) = save;
	EA = ea;
	return v;
}

static void mt_copies(void)
{
	mt_found = 0;
	for (mt_a = mt_start < 0x4000 ? 0x4000 : mt_start; mt_a != MT_END; mt_a++) {
		mt_b = mt_a - 0x4000;
		if (mt_copy_of(mt_a, mt_b))
			break;
		if (mt_a >= 0x8000) {
			mt_b = mt_a - 0x8000;
			if (mt_copy_of(mt_a, mt_b))
				break;
		}
	}
	if (mt_a != MT_END)
		mt_found = 1;
}

static void mt_fill(void)
{
	for (mt_a = mt_start; mt_a != MT_END; mt_a++)
		MT(mt_a) = MT_PATTERN(mt_a) ^ mt_inv;
}

static void mt_check(void)
{
	for (mt_a = mt_start; mt_a != MT_END; mt_a++) {
		mt_v = MT_PATTERN(mt_a) ^ mt_inv;
		mt_read = MT(mt_a);
		if (mt_read != mt_v) {
			if (!mt_bad) {
				mt_first = mt_a;
				mt_wrote = mt_v;
				mt_got = mt_read;
			}
			if (mt_bad != 0xffff)
				mt_bad++;
		}
	}
}

void memtest_run(void) __banked
{
	print_string("memtest: free external RAM, a second or two; management pauses meanwhile\n");
	print_string("Flash:    ");
	print_string(get_flash_size_str());
	print_string(", from the flash chip's ID\n");

	mt_start = memtest_xseg_end();
	mt_a = memtest_xiseg_end();
	if (mt_a > mt_start)
		mt_start = mt_a;
	print_string("XRAM:     0x0000-");
	print_short(mt_start - 1);
	print_string(" in use, testing ");
	print_short(mt_start);
	print_string("-0xbfff\n");

	mt_copies();
	if (mt_found) {
		print_string("Copies:   ");
		print_short(mt_a);
		print_string(" is the same byte as ");
		print_short(mt_b);
		print_string("\nmemtest: FAIL, stopped before writing anything else\n");
		return;
	}
	print_string("Copies:   none\n");

	mt_bad = 0;
	mt_inv = 0x00;
	mt_fill();
	mt_check();
	mt_inv = 0xff;
	mt_fill();
	mt_check();
	for (mt_a = mt_start; mt_a != MT_END; mt_a++)
		MT(mt_a) = 0;

	if (mt_bad) {
		print_string("Patterns: ");
		print_short(mt_bad);
		print_string(" bytes wrong, the first at ");
		print_short(mt_first);
		print_string(" (wrote 0x");
		print_byte(mt_wrote);
		print_string(", read 0x");
		print_byte(mt_got);
		print_string(")\nmemtest: FAIL\n");
		return;
	}
	print_string("Patterns: OK\nmemtest: PASS, XRAM works to 0xbfff\n");
}
