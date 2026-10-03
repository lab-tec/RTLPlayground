/*
 * test_memtest.c: memtest.c against models of the external RAM.
 *
 * memtest.c is compiled unmodified; it reaches the RAM through
 * memtest_byte(), which this file supplies. The models: the 48 kB it expects,
 * 64 kB, RAM that repeats every 16 or 32 kB, RAM that ends at 40 kB, and a
 * stuck bit. In every one, the part in use must come out unchanged, and no
 * byte of it may be touched while interrupts are enabled.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "rtl837x_common.h"
#include "rtl837x_flash.h"
#include "memtest.h"

static int fails, checks;
#define CHECK(c, msg) do { checks++; if (!(c)) { fails++; \
	printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, msg); } } while (0)

/* ---- the RAM model ---- */
static uint8_t ram[0x10000];
static uint8_t live[0x10000];	/* what the part in use held before */
static uint8_t none;		/* where a byte with no RAM behind it goes */
static uint32_t size;		/* addresses that have RAM behind them */
static uint32_t repeat;		/* the RAM repeats every this many bytes */
static uint32_t stuck;		/* a byte whose bit 0 reads 0, or 0 */
static uint16_t xseg_end, xiseg_end, in_use;
static int touched_with_ea;	/* accesses to the part in use, interrupts on */

bool EA;

__xdata uint8_t *memtest_byte(uint16_t a)
{
	uint32_t cell = a % repeat;

	if (a >= size) {
		none = 0;
		return &none;
	}
	if (cell < in_use && EA)
		touched_with_ea++;
	if (stuck && cell == stuck)
		ram[cell] &= 0xfe;
	return &ram[cell];
}
uint16_t memtest_xseg_end(void) { return xseg_end; }
uint16_t memtest_xiseg_end(void) { return xiseg_end; }

/* ---- what memtest.c prints through ---- */
static char out[4096];
static void put(const char *s) { strncat(out, s, sizeof(out) - strlen(out) - 1); }
void print_string(const char *p) { put(p); }
void write_char(char c) { char s[2] = { c, 0 }; put(s); }
void print_byte(uint8_t a) { char s[3]; snprintf(s, sizeof(s), "%02x", a); put(s); }
void print_short(uint16_t a) { char s[7]; snprintf(s, sizeof(s), "0x%04x", a); put(s); }
const char *get_flash_size_str(void) { return "2 MB"; }

/* ---- helpers ---- */
static void model(uint32_t sz, uint32_t rep, uint32_t stuck_at)
{
	size = sz;
	repeat = rep;
	stuck = stuck_at;
	xseg_end = 0x3ed9;
	xiseg_end = 0x3eeb;
	in_use = 0x3eeb;
	for (uint32_t i = 0; i < sizeof(ram); i++)
		ram[i] = i < in_use ? (uint8_t)(i * 7 + 3) : 0xee;
	memcpy(live, ram, sizeof(ram));
	touched_with_ea = 0;
	out[0] = 0;
	EA = 1;
}

static void run(void)
{
	memtest_run();
	printf("%s", out);
}

static int has(const char *s) { return strstr(out, s) != NULL; }

static void common_checks(const char *name)
{
	char msg[128];

	snprintf(msg, sizeof(msg), "%s: the part in use is unchanged", name);
	CHECK(!memcmp(ram, live, in_use), msg);
	snprintf(msg, sizeof(msg), "%s: nothing in use touched with interrupts on", name);
	CHECK(touched_with_ea == 0, msg);
	snprintf(msg, sizeof(msg), "%s: interrupts restored", name);
	CHECK(EA == 1, msg);
	snprintf(msg, sizeof(msg), "%s: says what it tested", name);
	CHECK(has("XRAM:     0x0000-0x3eea in use, testing 0x3eeb-0xbfff\n"), msg);
	snprintf(msg, sizeof(msg), "%s: reports the flash", name);
	CHECK(has("Flash:    2 MB"), msg);
}

static int all_zero(uint32_t from, uint32_t to)
{
	for (uint32_t i = from; i < to; i++)
		if (ram[i])
			return 0;
	return 1;
}

int main(void)
{
	puts("--- 48 kB, as expected");
	model(0xc000, 0x10000, 0);
	run();
	common_checks("48 kB");
	CHECK(has("Copies:   none\n"), "48 kB: no copies");
	CHECK(has("Patterns: OK\n"), "48 kB: patterns read back");
	CHECK(has("memtest: PASS"), "48 kB: passes");
	CHECK(all_zero(0x3eeb, 0xc000), "48 kB: the free part is left zeroed");

	puts("--- 64 kB: the top 16 kB is left alone");
	model(0x10000, 0x10000, 0);
	run();
	common_checks("64 kB");
	CHECK(has("memtest: PASS"), "64 kB: passes");
	for (uint32_t i = 0xc000; i < 0x10000; i++)
		if (ram[i] != 0xee) {
			CHECK(0, "64 kB: nothing above 0xbfff written");
			break;
		}

	puts("--- 32 kB, repeating");
	model(0x10000, 0x8000, 0);
	run();
	common_checks("32 kB");
	CHECK(has("Copies:   0x8000 is the same byte as 0x0000\n"), "32 kB: the copy is named");
	CHECK(has("memtest: FAIL, stopped"), "32 kB: fails before the patterns");
	CHECK(!has("Patterns"), "32 kB: no patterns written");

	puts("--- 16 kB, repeating");
	model(0x10000, 0x4000, 0);
	run();
	common_checks("16 kB");
	CHECK(has("Copies:   0x4000 is the same byte as 0x0000\n"), "16 kB: the copy is named");
	CHECK(has("memtest: FAIL, stopped"), "16 kB: fails before the patterns");

	puts("--- 40 kB: nothing answers from 0xa000");
	model(0xa000, 0x10000, 0);
	run();
	common_checks("40 kB");
	CHECK(has("Copies:   none\n"), "40 kB: missing RAM is not a copy");
	/* 0x2000 missing bytes per pattern, less the 32 where it is 0x00 */
	CHECK(has("Patterns: 0x3fc0 bytes wrong, the first at 0xa000 (wrote 0xa0, read 0x00)\n"),
	      "40 kB: count and first address");
	CHECK(has("memtest: FAIL\n"), "40 kB: fails");

	puts("--- bit 0 stuck at 0 at 0x9000");
	model(0xc000, 0x10000, 0x9000);
	run();
	common_checks("stuck bit");
	CHECK(has("Patterns: 0x0001 bytes wrong, the first at 0x9000 (wrote 0x6f, read 0x6e)\n"),
	      "stuck bit: found by the inverse pattern");

	puts("--- interrupts off to begin with stay off");
	model(0xc000, 0x10000, 0);
	EA = 0;
	memtest_run();
	CHECK(EA == 0, "EA left as found");

	puts("--- initialised variables below the others: the higher end counts");
	model(0xc000, 0x10000, 0);
	xseg_end = 0x3f00;
	xiseg_end = 0x3e00;
	in_use = 0x3f00;
	run();
	CHECK(has("testing 0x3f00-0xbfff"), "starts past XSEG when it ends higher");

	printf("test_memtest: %d checks, %d failed\n", checks, fails);
	return fails ? 1 : 0;
}
