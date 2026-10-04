/*
 * Keeping the admin password out of what the web interface shows.
 *
 * The password lives in the startup configuration as a "passwd" line, which
 * execute_config() replays at boot like any other command. The format stays
 * as it is, so older images still read it. What changes is what the web
 * server hands out and takes in:
 *
 * - cfgpass_hide() drops every "passwd" line from a text about to be sent:
 *   the configuration (GET /config) and the command log (GET /cmd_log),
 *   which would otherwise carry a password set through the console or the
 *   System page.
 * - cfgpass_keep() runs on a configuration about to be saved (POST
 *   /config). The web interface never sees the password, so what it saves
 *   has no "passwd" line; the running password is put back as the first
 *   line. A text that does carry a "passwd" line sets that password, as
 *   before. The line goes first so that it is always in the part of the
 *   configuration a single reply covers.
 *
 * Both work on the text at cfgpass_buf, cfgpass_len bytes long, and update
 * the two in place. This sits in bank 3: the web server's own bank is full.
 */

#pragma codeseg BANK3
#pragma constseg BANK3

#include <stdint.h>
#include "rtl837x_common.h"
#include "cfgpass.h"

extern __xdata char passwd[21];

__xdata uint8_t * __xdata cfgpass_buf;
__xdata uint16_t cfgpass_len;
__xdata uint8_t * __xdata cfgpass_floor;

static __code const char cp_word[] = "passwd";
static __xdata uint16_t cp_in, cp_out, cp_line, cp_at;
static __xdata uint8_t cp_n;
static __xdata uint8_t cp_drop;

/* Whether the line at cfgpass_buf[cp_line] is a "passwd" command, leading
 * blanks allowed */
static uint8_t cp_is_passwd(void)
{
	cp_at = cp_line;
	while (cp_at < cfgpass_len && (cfgpass_buf[cp_at] == ' ' || cfgpass_buf[cp_at] == '\t'))
		cp_at++;
	for (cp_n = 0; cp_n < 6; cp_n++) {
		if (cp_at + cp_n >= cfgpass_len || cfgpass_buf[cp_at + cp_n] != cp_word[cp_n])
			return 0;
	}
	if (cp_at + 6 >= cfgpass_len)
		return 1;
	/* Tests as statements: a combined condition costs the compiler a bit
	 * variable in internal RAM */
	cp_n = cfgpass_buf[cp_at + 6];
	if (cp_n == ' ' || cp_n == '\t' || cp_n == '\n' || cp_n == '\r' || cp_n == 0)
		return 1;
	return 0;
}

void cfgpass_hide(void) __banked
{
	cp_out = 0;
	cp_line = 0;
	while (cp_line < cfgpass_len) {
		cp_drop = cp_is_passwd();
		for (cp_in = cp_line; cp_in < cfgpass_len; cp_in++) {
			if (!cp_drop)
				cfgpass_buf[cp_out++] = cfgpass_buf[cp_in];
			if (cfgpass_buf[cp_in] == '\n') {
				cp_in++;
				break;
			}
		}
		cp_line = cp_in;
	}
	cfgpass_len = cp_out;
}

uint8_t cfgpass_keep(void) __banked
{
	for (cp_line = 0; cp_line < cfgpass_len; ) {
		if (cp_is_passwd())
			return 1;
		while (cp_line < cfgpass_len && cfgpass_buf[cp_line] != '\n')
			cp_line++;
		cp_line++;
	}

	/* "passwd " + the password + '\n', written just ahead of the text */
	for (cp_n = 0; passwd[cp_n]; cp_n++)
		;
	cp_n += 8;
	if (cfgpass_buf - cfgpass_floor < cp_n)
		return 0;
	cfgpass_buf -= cp_n;
	cfgpass_len += cp_n;
	for (cp_n = 0; cp_n < 6; cp_n++)
		cfgpass_buf[cp_n] = cp_word[cp_n];
	cfgpass_buf[6] = ' ';
	for (cp_n = 0; passwd[cp_n]; cp_n++)
		cfgpass_buf[7 + cp_n] = passwd[cp_n];
	cfgpass_buf[7 + cp_n] = '\n';
	return 1;
}
