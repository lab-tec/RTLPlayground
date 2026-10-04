/*
 * test_kadam.c: image D's modules (kadam.h) through their hooks.
 *
 * kadam.c and the six modules are compiled unmodified, against the register
 * mock: temperatures, link registers, MIB counters and the L2 table are set
 * there, seconds pass by calling kadam_second(), and commands go through
 * kadam_cmd() after a small tokenizer has filled the words the way
 * cmd_tokenize() does. Everything printed is collected and checked.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "rtl837x_common.h"
#include "rtl837x_regs.h"
#include "rtl837x_sfr.h"
#include "machine.h"
#include "uip/uip.h"
#include "kadam.h"
#include "hw_mock.h"

static int fails, checks;
static char out[16384];
/* A failure shows the end of what was printed */
#define CHECK(c, msg) do { checks++; if (!(c)) { fails++; \
	printf("FAIL %s:%d: %s\n  output: [%s]\n", __FILE__, __LINE__, msg, \
	       strlen(out) > 300 ? out + strlen(out) - 300 : out); } } while (0)

/* ---- what the modules link against ---- */
const struct machine machine = {
	.machine_name = "HOSTTEST",
	.min_port = 0,
	.max_port = 8,
	.log_to_phys_port = { 1, 2, 3, 4, 5, 6, 7, 8, 9 },
	.phys_to_log_port = { 0, 1, 2, 3, 4, 5, 6, 7, 8 },
	.is_sfp = { 0, 0, 0, 0, 0, 0, 0, 0, 1 },
};
uint8_t cmd_buffer[CMD_BUF_SIZE];
uint8_t cmd_words_b[15];
uint8_t cmd_words_len;
uint8_t err_status;
volatile uint32_t ticks;
uint8_t uip_buf[UIP_BUFSIZE + 2];
uint16_t uip_len;
uip_ipaddr_t uip_hostaddr;
static int resets, arp_outs;
static uint8_t sent[UIP_BUFSIZE + 2];
static int sent_len;

void reset_chip(void) { resets++; }
void uip_arp_out(void) { arp_outs++; }
void tcpip_output(void) { memcpy(sent, uip_buf, sizeof(sent)); sent_len = uip_len; }

static void put(const char *s) { strncat(out, s, sizeof(out) - strlen(out) - 1); }
void print_string(const char *p) { put(p); }
void write_char(char c) { char s[2] = { c, 0 }; put(s); }
void print_byte(uint8_t a) { char s[3]; snprintf(s, sizeof(s), "%02x", a); put(s); }
void itoa_short(uint16_t v) { char s[6]; snprintf(s, sizeof(s), "%u", v); put(s); }

/* ---- helpers ---- */
static int has(const char *s) { return strstr(out, s) != NULL; }
static void clear(void) { out[0] = 0; }

/* Words as cmd_tokenize() leaves them: offsets into cmd_buffer */
static int cmd(const char *line)
{
	int i = 0, w = 0, in = 0;

	memset(cmd_buffer, 0, sizeof(cmd_buffer));
	strncpy((char *)cmd_buffer, line, sizeof(cmd_buffer) - 1);
	for (; cmd_buffer[i]; i++) {
		if (cmd_buffer[i] == ' ') {
			in = 0;
		} else if (!in) {
			in = 1;
			cmd_words_b[w++] = i;
		}
	}
	cmd_words_len = w;
	err_status = 0;
	return kadam_cmd();
}

static void seconds(int n)
{
	while (n--) {
		ticks += 200;
		kadam_second();
	}
}

static void set_temp(double c)
{
	int16_t v = (int16_t)(c * 128);
	hw_reg_set(RTL837X_TM_RESULT, (uint16_t)v);
}

/* Links: a bit per logical port up, and the speed nibble of each */
static void set_links(uint16_t up, const uint8_t speed[9])
{
	uint32_t l = 0;
	for (int p = 0; p < 8; p++)
		l |= (uint32_t)(speed[p] & 0xf) << (4 * p);
	hw_reg_set(RTL837X_REG_LINKS_STS, ((uint32_t)(up & 0xff) << 16) | ((uint32_t)(up >> 8) << 8));
	hw_reg_set(RTL837X_REG_LINKS, l);
	hw_reg_set(RTL837X_REG_LINKS_89, speed[8] & 0xf);
}

static uint16_t csum(const uint8_t *p, int n)
{
	uint32_t s = 0;
	for (int i = 0; i < n; i += 2)
		s += (p[i] << 8) | (i + 1 < n ? p[i + 1] : 0);
	while (s >> 16)
		s = (s & 0xffff) + (s >> 16);
	return s;
}

static void boot(void)
{
	hw_reset();
	set_temp(70);
	kadam_init();
	clear();
}

int main(void)
{
	static const uint8_t all_2g5[9] = { 5, 5, 5, 5, 5, 5, 5, 5, 4 };
	uint8_t speeds[9];

	/* ================= the dispatcher ================= */
	puts("--- dispatcher");
	boot();
	CHECK(!cmd("frobnicate"), "unknown words go back to cmd_parser");
	CHECK(cmd("revert"), "revert is ours");
	CHECK(cmd("thermal") && cmd("linkwatch") && cmd("macwatch") && cmd("ping") && cmd("boot"),
	      "the other five are ours");
	CHECK(!cmd("reverting"), "a longer word isn't a match");

	/* ================= revert ================= */
	puts("--- revert");
	boot();
	cmd("revert");
	CHECK(has("revert: not armed\n"), "starts disarmed");
	clear(); cmd("revert in 0");
	CHECK(has("Usage: revert in <1-60 minutes>") && err_status == ERR_INVALID_ARGUMENT, "0 minutes refused");
	clear(); cmd("revert in 61");
	CHECK(has("Usage:"), "61 minutes refused");
	clear(); cmd("revert in 2");
	CHECK(has("revert: armed; rebooting into the saved config in 2 min 0 s"), "armed for 2 minutes");
	clear(); seconds(60);
	CHECK(has("revert: rebooting into the saved config in 1 min 0 s unless"), "a reminder a minute in");
	clear(); cmd("revert");
	CHECK(has("revert: armed, 1 min 0 s left\n"), "status shows the time left");
	clear(); resets = 0; seconds(60);
	CHECK(has("revert: not confirmed in time; rebooting into the saved config\n"), "runs out");
	CHECK(resets == 0, "...but waits for syslog");
	seconds(2);
	CHECK(resets == 1, "then resets");
	CHECK(reboot_why() == REBOOT_REVERT, "and tells the next boot why");

	boot(); resets = 0;
	cmd("revert in 1");
	clear(); revert_saved();
	CHECK(has("revert: config saved; timer cancelled\n"), "saving disarms it");
	seconds(70);
	CHECK(resets == 0, "no reset after saving");
	clear(); revert_saved();
	CHECK(!has("revert"), "saving with nothing armed says nothing");

	cmd("revert in 1");
	clear(); cmd("revert cancel");
	CHECK(has("revert: cancelled\n"), "cancel");
	seconds(70);
	CHECK(resets == 0, "no reset after cancelling");
	clear(); cmd("revert cancel");
	CHECK(has("revert: not armed\n"), "cancel with nothing armed");
	clear(); cmd("revert later");
	CHECK(has("Usage:"), "unknown subcommand");

	/* ================= thermal ================= */
	puts("--- thermal");
	boot();
	seconds(5);
	CHECK(!has("temp:"), "70 C: nothing to say");
	cmd("thermal");
	CHECK(has("thermal: 70.0 C now; warn 85 C, crit 95 C; state ok\n"), "status");
	clear(); set_temp(86.5); seconds(1);
	CHECK(has("temp: 86.5 C, above the warning threshold 85 C\n"), "warning crossing");
	clear(); seconds(599);
	CHECK(!has("temp:"), "quiet for ten minutes");
	seconds(1);
	CHECK(has("temp: 86.5 C, above the warning threshold"), "then a reminder");
	clear(); set_temp(83); seconds(5);
	CHECK(!has("temp:"), "83 C: inside the 3 C hysteresis, still warning");
	set_temp(81.9); seconds(1);
	CHECK(has("temp: back to 81.8 C, below the warning threshold\n") || has("temp: back to 81.9 C, below the warning threshold\n"),
	      "back under once 3 C clear");
	clear(); set_temp(96); seconds(1);
	CHECK(has("temp: 96.0 C, above the critical threshold 95 C\n"), "critical crossing");
	clear(); set_temp(93); seconds(3);
	CHECK(!has("temp:"), "93 C: still critical");
	set_temp(91); seconds(1);
	CHECK(has("temp: back to 91.0 C, below the critical threshold\n"), "down to warning");
	clear(); cmd("thermal");
	CHECK(has("state WARNING"), "status shows the level");
	clear(); cmd("thermal warn 60 crit 61");
	CHECK(has("Usage: thermal warn <40-124> crit <warn+3-125>") && err_status, "thresholds too close");
	clear(); cmd("thermal warn 85");
	CHECK(has("Usage: thermal [warn <C> crit <C>]"), "incomplete");
	clear(); cmd("thermal warn 60 crit 70");
	CHECK(has("warn 60 C, crit 70 C"), "new thresholds");
	clear(); set_temp(65); seconds(1);
	CHECK(has("above the warning threshold 60 C"), "and they apply");
	clear(); set_temp(-5.5); cmd("thermal");
	CHECK(has("thermal: -5.5 C now"), "below zero prints");

	/* ================= boot announcement ================= */
	puts("--- boot");
	hw_reset(); set_temp(71.9375);
	reboot_note(REBOOT_UPGRADE);
	kadam_init(); clear();
	CHECK(reboot_why() == REBOOT_NONE, "the marker is cleared at boot");
	seconds(29);
	CHECK(!has("boot:"), "not before 30 s");
	seconds(1);
	/* bootmsg.c includes the firmware's own generated version.h */
	CHECK(has("boot: v") && has(" up, reason: upgrade; temp 71.9 C\n"), "the announcement");
	clear(); seconds(60);
	CHECK(!has("boot:"), "only once");
	cmd("boot");
	CHECK(has(" up, reason: upgrade"), "boot repeats it");

	hw_reset(); reboot_note(REBOOT_CMD);
	{ extern uint8_t reboot_marker[6]; reboot_marker[5] ^= 1; }
	kadam_init(); clear(); seconds(30);
	CHECK(has("reason: power-on (or a crash)"), "a damaged marker is no marker");
	hw_reset(); reboot_note(200);
	kadam_init(); clear(); seconds(30);
	CHECK(has("reason: power-on (or a crash)"), "an unknown reason is no reason");
	hw_reset(); reboot_note(REBOOT_ROLLBACK);
	kadam_init(); clear(); seconds(30);
	CHECK(has("reason: rollback to the firmware backup"), "rollback");

	/* ================= link health ================= */
	puts("--- linkwatch");
	boot();
	memcpy(speeds, all_2g5, 9);
	set_links(0x0f6, speeds);		/* logical 1, 2, 4, 5, 6, 7 up */
	seconds(25);
	CHECK(!has("link:"), "the first 20 s only record");
	clear(); set_links(0x0d6, speeds); seconds(1);	/* logical 5 down */
	CHECK(has("link: port 6 down\n"), "down");
	clear(); speeds[5] = 2; set_links(0x0f6, speeds); seconds(1);
	CHECK(has("link: port 6 up at 1G, below its best since boot, 2.5G\n"), "a downshift");
	clear(); speeds[5] = 5; set_links(0x0f6, speeds); seconds(1);
	CHECK(has("link: port 6 up at 2.5G\n"), "a speed change on a live link");
	clear();
	for (int i = 0; i < 2; i++) {
		set_links(0x0d6, speeds); seconds(1);
		set_links(0x0f6, speeds); seconds(1);
	}
	CHECK(has("link: port 6 flapping, 6 changes in 10 min\n"), "six changes: flapping");
	clear(); set_links(0x0d6, speeds); seconds(1); set_links(0x0f6, speeds); seconds(1);
	CHECK(!has("flapping"), "flapping logged once a window");
	clear(); seconds(600);
	set_links(0x0d6, speeds); seconds(1);
	CHECK(!has("flapping"), "a fresh window starts at zero");
	set_links(0x0f6, speeds); seconds(1);
	clear(); cmd("linkwatch");
	CHECK(has("port 6: up at 2.5G, changes this window 2, CRC errors since boot 0\n"), "status line");
	CHECK(has("port 1: down") && has("port 9: down"), "every port listed");

	puts("--- CRC errors");
	boot();
	set_links(0x0f6, all_2g5);
	hw_counter_set(5, 15, 1000);
	seconds(60);			/* baseline at 10 s, nothing new */
	CHECK(!has("CRC"), "no errors, no report");
	clear(); hw_counter_set(5, 15, 1012); seconds(60);
	CHECK(has("link: port 6, 12 CRC errors in the last minute\n"), "errors reported");
	clear(); seconds(60);
	CHECK(!has("CRC"), "and only while they grow");
	clear(); hw_counter_set(5, 15, 0xffffffffULL); seconds(10);
	hw_counter_set(5, 15, 0x100000003ULL); seconds(50);
	CHECK(has("link: port 6, 4294966295 CRC errors") || has("CRC errors in the last minute"),
	      "a 32-bit wrap still adds up");
	clear(); cmd("linkwatch");
	CHECK(has("port 6: up at 2.5G, changes this window 0, CRC errors since boot "), "total kept");

	/* ================= new-device log ================= */
	puts("--- macwatch");
	boot();
	static const uint8_t m1[6] = { 0x3c, 0x22, 0xfb, 0x01, 0x02, 0x03 };
	static const uint8_t m2[6] = { 0x00, 0x11, 0x22, 0x33, 0x44, 0x55 };
	static const uint8_t m3[6] = { 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0x01 };
	static const uint8_t m4[6] = { 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0x02 };
	static const uint8_t ms[6] = { 0x02, 0x00, 0x00, 0x00, 0x00, 0x99 };
	hw_l2_put(0x100, m1, 1, 6, false);	/* logical 6 = port 7 */
	hw_l2_put(0x200, m2, 200, 1, false);
	hw_l2_put(0x050, ms, 1, 0, true);	/* static: skipped */
	cmd("macwatch ports 2 3 5 7 8");
	clear(); seconds(1);
	CHECK(has("mac: baseline, 2 devices known\n"), "baseline counts the learned entries");
	CHECK(!has("mac: new"), "nothing new during the baseline");
	clear(); hw_l2_put(0x300, m3, 1, 6, false); seconds(1);
	CHECK(has("mac: new aa:bb:cc:dd:ee:01 vlan 1 on port 7\n"), "a new device on a watched port");
	clear(); hw_l2_put(0x310, m4, 52, 3, false); seconds(1);
	CHECK(!has("mac:"), "port 4 isn't watched");
	clear(); seconds(3);
	CHECK(!has("mac:"), "known devices aren't reported again");
	clear(); hw_l2_put(0x100, m1, 1, 2, false); seconds(1);	/* m1 to logical 2 = port 3 */
	CHECK(has("mac: 3c:22:fb:01:02:03 vlan 1 moved from port 7 to port 3\n"), "a move");
	clear(); hw_l2_put(0x310, m4, 52, 5, false); seconds(1);	/* port 4 -> port 6: neither watched */
	CHECK(!has("moved"), "a move between unwatched ports is quiet");
	clear(); cmd("macwatch");
	CHECK(has("macwatch: on, watching ports 2 3 5 7 8; 4 devices known\n"), "status");
	clear(); cmd("macwatch ports 2 10");
	CHECK(has("Usage: macwatch ports") && err_status, "a bad port");
	clear(); cmd("macwatch off"); hw_l2_put(0x400, ms, 1, 6, false); seconds(2);
	CHECK(!has("mac: new") && has("macwatch: off"), "off is off");
	clear(); cmd("macwatch on"); seconds(1);
	CHECK(has("mac: baseline, "), "on starts a fresh baseline");
	hw_reset(); boot(); seconds(1);
	CHECK(has("mac: baseline, 0 devices known\n"), "an empty table completes");

	/* ================= ping ================= */
	puts("--- ping");
	boot();
	uip_hostaddr[0] = 0; uip_hostaddr[1] = 0;
	memcpy(uip_hostaddr, "\xac\x10\x00\x06", 4);
	cmd("ping");
	CHECK(has("ping: none yet. Usage: ping <ip>\n"), "before any ping");
	clear(); cmd("ping 172.16.0");
	CHECK(has("Usage: ping <ip>") && err_status, "a bad address");
	clear(); cmd("ping 172.16.0.256");
	CHECK(has("Usage: ping <ip>"), "an octet over 255");
	clear(); cmd("ping 172.16.0.254");
	CHECK(has("ping: 4 echo requests to 172.16.0.254, one a second"), "started");
	sent_len = 0; arp_outs = 0;
	seconds(1);
	CHECK(sent_len == 60 && arp_outs == 1, "a 60-byte IP packet went out through uip_arp_out()");
	const uint8_t *ip = sent + UIP_LLH_LEN, *icmp = ip + 20;
	CHECK(ip[0] == 0x45 && ip[9] == 1 && ip[3] == 60 && ip[8] == 64, "IPv4, ICMP, 60 bytes, TTL 64");
	CHECK(!memcmp(ip + 12, "\xac\x10\x00\x06", 4) && !memcmp(ip + 16, "\xac\x10\x00\xfe", 4), "addresses");
	CHECK(csum(ip, 20) == 0xffff, "IP header checksum");
	CHECK(icmp[0] == 8 && icmp[4] == 'K' && icmp[5] == 'D' && icmp[6] == 0 && icmp[7] == 0, "echo request, KD, seq 0");
	CHECK(csum(icmp, 40) == 0xffff, "ICMP checksum");

	/* the reply, 10 ms later */
	ticks += 2;
	memcpy(uip_buf, sent, sizeof(uip_buf));
	{
		uint8_t *rip = uip_buf + UIP_LLH_LEN;
		memcpy(rip + 12, "\xac\x10\x00\xfe", 4);
		memcpy(rip + 16, "\xac\x10\x00\x06", 4);
		rip[20] = 0;
	}
	clear(); ping_reply();
	CHECK(has("ping: reply from 172.16.0.254 seq 1: 10 ms\n"), "the reply, with its time");
	clear(); ping_reply();
	CHECK(!has("ping:"), "a duplicate is ignored");
	uip_buf[UIP_LLH_LEN + 24] = 'X';
	clear(); ping_reply();
	CHECK(!has("ping:"), "someone else's identifier is ignored");
	clear(); cmd("ping");
	CHECK(has("ping: 172.16.0.254: 1 sent, 1 received, rtt min/avg/max 10/10/10 ms (still running)\n"), "progress");
	clear(); seconds(3);
	CHECK(!has("ping: 172.16.0.254:"), "the summary waits");
	clear(); seconds(3);
	CHECK(has("ping: 172.16.0.254: 4 sent, 1 received, rtt min/avg/max 10/10/10 ms\n"), "the summary");
	sent_len = 0; seconds(5);
	CHECK(sent_len == 0, "and then it stops");
	clear(); ping_reply();
	CHECK(!has("ping:"), "replies after the end are ignored");

	printf("test_kadam: %d checks, %d failed\n", checks, fails);
	return fails ? 1 : 0;
}
