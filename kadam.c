/*
 * Image D's hooks and the command-line helpers its modules share.
 *
 * kadam_cmd() is tried by cmd_parser() for any command it doesn't know, and
 * matches the words itself, so the parser's full bank 2 grows by one call.
 * kadam_second() runs from the main loop once a second; kadam_init() sets
 * every module's defaults before the startup configuration is replayed (the
 * startup code leaves external RAM as it was, so nothing starts at zero).
 *
 * Everything here keeps its state in external RAM and passes at most one
 * argument to a function that calls another: internal RAM has no room left.
 * Multiplications by ten are shifts, for the same reason: the library's
 * 16-bit multiply takes its second argument in internal RAM.
 */

#pragma codeseg BANK3
#pragma constseg BANK3

#include <stdint.h>
#include "rtl837x_common.h"
#include "machine.h"
#include "cmd_parser.h"
#include "kadam.h"

extern __xdata uint8_t cmd_buffer[CMD_BUF_SIZE];
extern __xdata uint8_t cmd_words_b[];
extern __xdata uint8_t cmd_words_len;
extern __code const struct machine machine;

__xdata uint16_t kc_val;
__xdata uint8_t kc_addr[4];
__xdata uint8_t kc_out_port;
__xdata uint8_t kc_out_ip[4];
__xdata uint32_t kc_out_u32;

static __xdata uint8_t kc_i, kc_c, kc_n, kc_part;
static __xdata uint32_t kc_p;

static __code const uint32_t kc_pow10[] = {
	1000000000UL, 100000000UL, 10000000UL, 1000000UL, 100000UL,
	10000UL, 1000UL, 100UL, 10UL, 1UL
};

void kadam_init(void) __banked
{
	bootmsg_init();
	revert_init();
	thermal_init();
	linkwatch_init();
	macwatch_init();
	ping_init();
}

void kadam_second(void) __banked
{
	revert_second();
	thermal_second();
	linkwatch_second();
	macwatch_second();
	ping_second();
	bootmsg_second();
}

uint8_t kadam_cmd(void) __banked
{
	if (kc_is(0, "revert"))
		revert_cmd();
	else if (kc_is(0, "thermal"))
		thermal_cmd();
	else if (kc_is(0, "linkwatch"))
		linkwatch_cmd();
	else if (kc_is(0, "macwatch"))
		macwatch_cmd();
	else if (kc_is(0, "ping"))
		ping_cmd();
	else if (kc_is(0, "boot"))
		bootmsg_cmd();
	else
		return 0;
	return 1;
}

/* Whether word `word` is exactly s */
uint8_t kc_is(uint8_t word, __code const char *s)
{
	if (word >= cmd_words_len)
		return 0;
	kc_i = cmd_words_b[word];
	for (kc_n = 0; s[kc_n]; kc_n++, kc_i++) {
		if (cmd_buffer[kc_i] != s[kc_n])
			return 0;
	}
	kc_c = cmd_buffer[kc_i];
	if (kc_c == ' ' || kc_c == 0)
		return 1;
	return 0;
}

/* Decimal 0-65535 into kc_val */
uint8_t kc_num(uint8_t word)
{
	if (word >= cmd_words_len)
		return 0;
	kc_i = cmd_words_b[word];
	kc_val = 0;
	for (kc_n = 0; ; kc_n++, kc_i++) {
		kc_c = cmd_buffer[kc_i];
		if (kc_c == ' ' || kc_c == 0)
			break;
		if (kc_c < '0' || kc_c > '9' || kc_n == 5)
			return 0;
		if (kc_val > 6553 || (kc_val == 6553 && kc_c > '5'))
			return 0;
		kc_val = (kc_val << 3) + (kc_val << 1) + (kc_c - '0');
	}
	if (!kc_n)
		return 0;
	return 1;
}

/* a.b.c.d into kc_addr */
uint8_t kc_ip(uint8_t word)
{
	if (word >= cmd_words_len)
		return 0;
	kc_i = cmd_words_b[word];
	for (kc_part = 0; kc_part < 4; kc_part++) {
		kc_val = 0;
		for (kc_n = 0; ; kc_n++, kc_i++) {
			kc_c = cmd_buffer[kc_i];
			if (kc_c < '0' || kc_c > '9')
				break;
			if (kc_n == 3)
				return 0;
			kc_val = (kc_val << 3) + (kc_val << 1) + (kc_c - '0');
		}
		if (!kc_n || kc_val > 255)
			return 0;
		kc_addr[kc_part] = kc_val;
		if (kc_part < 3) {
			if (kc_c != '.')
				return 0;
			kc_i++;
		}
	}
	if (kc_c == ' ' || kc_c == 0)
		return 1;
	return 0;
}

/* A front-panel port number into kc_val, as a logical port */
uint8_t kc_port(uint8_t word)
{
	if (!kc_num(word) || kc_val < 1 || kc_val > 9)
		return 0;
	kc_val = machine.phys_to_log_port[kc_val - 1];
	if (kc_val < machine.min_port || kc_val > machine.max_port)
		return 0;
	return 1;
}

/* usage is printed as it is ("Usage: ..."): one call, so the pointer needn't
 * survive another and be kept in internal RAM */
void kc_usage(__code const char *usage)
{
	err_status = ERR_INVALID_ARGUMENT;
	print_string(usage);
}

void kc_print_port(void)
{
	itoa_short(machine.log_to_phys_port[kc_out_port]);
}

void kc_print_ip(void)
{
	for (kc_part = 0; kc_part < 4; kc_part++) {
		if (kc_part)
			write_char('.');
		itoa_short(kc_out_ip[kc_part]);
	}
}

/* Decimal by subtraction: a 32-bit division would pull library code whose
 * arguments need internal RAM */
void kc_print_u32(void)
{
	kc_part = 0;
	for (kc_i = 0; kc_i < 10; kc_i++) {
		kc_p = kc_pow10[kc_i];
		kc_c = '0';
		while (kc_out_u32 >= kc_p) {
			kc_out_u32 -= kc_p;
			kc_c++;
		}
		if (kc_c != '0' || kc_part || kc_i == 9) {
			write_char(kc_c);
			kc_part = 1;
		}
	}
}
