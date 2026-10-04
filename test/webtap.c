/*
 * webtap: the switch's own web stack on a Linux TAP interface.
 *
 * Compiles the UNMODIFIED uip/uip.c, uip/uip_arp.c, httpd/httpd.c and
 * httpd/page_impl.c (with rtl837x_port.c and the register mock beneath them)
 * and attaches them to a TAP device, so a real browser, curl or Playwright on
 * this machine talks to the firmware's TCP/IP stack and web server as built:
 * one TCP connection, one login session, uIP's timers on the 5 ms tick. The
 * web files are read out of a built firmware image, as the switch does.
 *
 *   make -C test webtap
 *   test/build/webtap <firmware.bin> [loss-percent]     (needs CAP_NET_ADMIN)
 *   then http://10.77.0.2/ from this machine, password 1234
 *
 * WEBTAP_CPU_US=<n> spends n microseconds after each received frame, standing
 * in for the 8051's processing time; WEBTAP_QUIET silences the console;
 * WEBTAP_STATS prints frame and uIP counters every 10 s; WEBTAP_DEBUG traces
 * frames.
 *
 * Frames cross the TAP the way they cross the switch's CPU port: received
 * frames get the RTL CPU tag and an 802.1Q tag inserted after the addresses,
 * as the ASIC delivers them; transmitted frames start after the 12-byte TX
 * descriptor. The ASIC fills in IP and TCP checksums on the switch, and uIP
 * leaves them zero, so they are computed here before a frame goes out.
 *
 * Not modelled: the 8051's speed (everything runs at host speed), the console
 * behind /cmd (a stub answers each command with one line), flash timing, and
 * a firmware upload's install (the reset it asks for re-initialises the stack
 * after two seconds of silence, like a reboot that keeps the old firmware).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <time.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <net/if.h>
#include <linux/if_tun.h>
#include <arpa/inet.h>

/* Some hosts ship these in <sys/_endian.h>; the firmware's own go first. */
#undef HTONS
#undef NTOHS

#include "rtl837x_common.h"
#include "rtl837x_port.h"
#include "rtl837x_flash.h"
#include "rtl837x_phy.h"
#include "rtl837x_stp.h"
#include "machine.h"
#include "syslog.h"
#include "httpd.h"
#include "uip.h"
#include "uip_arp.h"
#include "html_data.h"
#include "tcpstat.h"

#define TAP_NAME	"webtap0"
#define HOST_IP		"10.77.0.1"
#define TICK_NS		5000000L	/* SYS_TICK_HZ 200, as on the switch */
#define FLASH_SIZE	0x200000

/* ---- console ------------------------------------------------------------ */

uint8_t cmd_capture;
uint8_t err_status;
uint8_t cmd_buffer[CMD_BUF_SIZE];
uint8_t cmd_history[CMD_HISTORY_SIZE];
uint16_t cmd_history_ptr;
extern uint8_t outbuf[TCP_OUTBUF_SIZE];	/* httpd.c */
extern uint16_t slen;

static int quiet, debug, debug_rst, cpu_us;

/* As write_char_no_syslog() in rtlplayground.c: a /cmd answer is captured. */
void write_char_no_syslog(char c)
{
	if (cmd_capture) {
		if (slen < TCP_OUTBUF_SIZE - 32)
			outbuf[slen++] = c;
		else
			cmd_capture = 2;
	}
	if (!quiet)
		putchar(c);
}
void write_char(char c) { write_char_no_syslog(c); }
void print_string(const char *p) { while (*p) write_char(*p++); }
void print_string_x(char *p) { while (*p) write_char(*p++); }
void print_string_no_syslog(const char *p) { while (*p) write_char_no_syslog(*p++); }
void print_string_newline_no_syslog(const char *p) { write_char_no_syslog('\n'); print_string_no_syslog(p); }
void print_byte(uint8_t v)
{
	static const char h[] = "0123456789abcdef";
	write_char(h[(v >> 4) & 0xf]);
	write_char(h[v & 0xf]);
}
void print_short(uint16_t v) { print_byte(v >> 8); print_byte(v); }
void print_long(uint32_t v)  { print_short(v >> 16); print_short(v); }
void print_reg(uint16_t v)   { print_short(v); }
void itoa_short(uint16_t v)
{
	char b[6]; int n = 0;
	do { b[n++] = '0' + v % 10; v /= 10; } while (v);
	while (n) write_char(b[--n]);
}
void itoa(uint8_t v) { itoa_short(v); }
void dbg_string(char *p) { (void)p; }
void dbg_short(uint16_t v) { (void)v; }
void dbg_char(char c) { (void)c; }
void dbg_byte(uint8_t v) { (void)v; }
void print_cmd_prompt(void) { }
void cmd_parser(void) { }
void execute_config(void) { }
void clear_command_history(void) { }

/* /cmd: a stub console that answers every command with one line, except
 * "tcp", which runs the firmware's tcpstat.c. */
void execute_commands(uint8_t *p)
{
	err_status = ERR_OK;
	if (!strncmp((char *)p, "tcp", 3) && (!p[3] || p[3] == '\n')) {
		tcp_stats();
		return;
	}
	print_string("webtap: ");
	while (*p && *p != '\n' && *p != '\r')
		write_char(*p++);
	write_char('\n');
}

/* ---- copied from rtlplayground.c ---------------------------------------- */

uint16_t strtox(uint8_t *dst, const char *s)
{
	uint8_t *b = dst;
	while (*s)
		*dst++ = *s++;
	*dst = 0;
	return dst - b;
}
uint16_t strlen_x(const char *s) { return (uint16_t)strlen(s); }
void memcpyc(uint8_t *dst, const uint8_t *src, uint16_t len) { memcpy(dst, src, len); }
bool strstart(const uint8_t *a, const uint8_t *b)
{
	while (*b) { if (*a++ != *b++) return false; }
	return true;
}
bool strstart_x(const uint8_t *a, const uint8_t *b) { return strstart(a, b); }

const uint16_t bit_mask[16] = {
	0x0001, 0x0002, 0x0004, 0x0008, 0x0010, 0x0020, 0x0040, 0x0080,
	0x0100, 0x0200, 0x0400, 0x0800, 0x1000, 0x2000, 0x4000, 0x8000
};
static const uint8_t hex_chars[] = "0123456789abcdef";
uint8_t *hex = (uint8_t *)hex_chars;

/* ---- the machine and the state page_impl.c reports ---------------------- */

const struct machine machine = {
	.machine_name = "WEBTAP",
	.min_port = 0,
	.max_port = 8,
	.n_sfp = 0,
	.log_to_phys_port = { 1, 2, 3, 4, 5, 6, 7, 8, 9 },
	.phys_to_log_port = { 0, 1, 2, 3, 4, 5, 6, 7, 8 },
};
struct machine_runtime machine_detected = { .isRTL8373 = 1 };

uint16_t management_vlan = 1;
uint8_t  vlan_names[VLAN_NAMES_SIZE];
uint16_t vlan_ptr;
uint8_t  sfp_pins_last = 0xff;
uint8_t  sfp_options[2];
char     sfp_module_vendor[2][17];
char     sfp_module_model[2][17];
char     sfp_module_serial[2][17];
char     hostname[24] = "webtap";
char     port_names[9][PORT_NAME_SIZE];
struct syslog_state syslog_state;
bool     stp_enabled;
struct uip_eth_addr uip_ethaddr = { .addr = { 0x02, 0x00, 0x77, 0x00, 0x00, 0x02 } };

uint8_t  sfp_read_reg(uint8_t slot, uint8_t reg) { (void)slot; (void)reg; return 0; }
bool     gpio_pin_test(uint8_t pin) { (void)pin; return false; }
void     phy_read(uint8_t phy_id, uint8_t dev_id, uint16_t reg) { (void)phy_id; (void)dev_id; (void)reg; }
void     phy_write(uint8_t phy_id, uint8_t dev_id, uint16_t reg, uint16_t val) { (void)phy_id; (void)dev_id; (void)reg; (void)val; }
void     phy_reset(uint8_t port) { (void)port; }
uint8_t  stp_port_role(uint8_t port) { (void)port; return 0; }
uint8_t  stp_port_state(uint8_t port) { (void)port; return 3; }
struct bridge root_bridge;
struct bridge stp_dbridge[STP_ENTITIES];
uint16_t stp_bpdu_age[STP_ENTITIES];
uint16_t stp_dpid[STP_ENTITIES];
uint16_t stp_lag_mask[STP_LAG_COUNT];
uint16_t stp_tc_count;
uint32_t root_bridge_cost;
uint32_t stp_dcost[STP_ENTITIES];
uint32_t stp_pcost[STP_ENTITIES];
uint8_t  stp_ent_of[10] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9 };
uint8_t  stp_fwddelay_s = 15, stp_hello_s = 2, stp_maxage_s = 20;
uint8_t  stp_pflags[STP_ENTITIES];
uint8_t  stp_pp2p[STP_ENTITIES];
uint8_t  stp_pprio[STP_ENTITIES];
uint8_t  stp_prio = 0x80, stp_root_port = 0xff, stp_rstp = 1, stp_txhold = 6;
uint8_t sfp_buf[16];
bool    sfp_read_block(uint8_t slot, uint8_t reg, uint8_t len) { (void)slot; (void)reg; (void)len; return false; }
void    print_phys_port(uint8_t port) { itoa_short(machine.log_to_phys_port[port]); }

/* ---- flash: the firmware image, plus room for an upload ------------------ */

static uint8_t flash[FLASH_SIZE];
uint32_t flash_size = FLASH_SIZE;
uint8_t flash_buf[FLASH_BUF_SIZE];
struct flash_region_t flash_region;
uint16_t crc_value;

void flash_read_bulk(uint8_t *dst)
{
	if (flash_region.addr + flash_region.len <= FLASH_SIZE)
		memcpy(dst, &flash[flash_region.addr], flash_region.len);
}
void flash_init(uint8_t enable_dio) { (void)enable_dio; }
void flash_sector_erase(void)
{
	memset(&flash[flash_region.addr & (FLASH_SIZE - 1) & ~0xfffu], 0xff, 0x1000);
}
void flash_write_bytes(uint8_t *ptr)
{
	for (uint16_t i = 0; i < flash_region.len; i++)
		flash[(flash_region.addr + i) & (FLASH_SIZE - 1)] &= ptr[i];
}
const char *get_flash_size_str(void) { return "2M"; }

/* tools/crc_calculator.c's CRC16, which crc16_bank1() implements in 8051 code */
void crc16_bank1(uint8_t *v)
{
	crc_value ^= *v;
	for (int i = 0; i < 8; i++)
		crc_value = (crc_value & 1) ? (crc_value >> 1) ^ 0xA001 : crc_value >> 1;
}

/* ---- time ---------------------------------------------------------------- */

volatile uint32_t ticks;
extern volatile uint8_t sfr_data[4];	/* hw_mock.c */

void read_reg_timer(uint32_t *tmr) { *tmr = ticks / 200; }
void get_random_32(void)
{
	for (int i = 0; i < 4; i++)
		sfr_data[i] = (uint8_t)rand();
}
void delay(uint16_t t) { (void)t; }
void set_sys_led_state(uint8_t state) { (void)state; }
void udp_callbacks(void) { }

/* ---- the CPU port -------------------------------------------------------- */

u8_t uip_buf[UIP_BUFSIZE + 2];
uint8_t rx_headers[16];

static int tapfd = -1;
static int loss_pct;
static unsigned long frames_in, frames_out, frames_lost;
static int reset_pending;

/* The Internet checksum: uIP's own helpers aren't built for the switch
 * (UIP_ARCH_CHKSUM), whose ASIC computes them. */
static uint32_t csum_add(uint32_t sum, const uint8_t *d, int n)
{
	for (int i = 0; i + 1 < n; i += 2)
		sum += (uint32_t)(d[i] << 8 | d[i + 1]);
	if (n & 1)
		sum += (uint32_t)(d[n - 1] << 8);
	return sum;
}

static uint16_t csum_fold(uint32_t sum)
{
	while (sum >> 16)
		sum = (sum & 0xffff) + (sum >> 16);
	return (uint16_t)~sum;
}

static long blackout_at, blackout_ms;	/* WEBTAP_BLACKOUT=<s>,<ms> */

static int lose(void)
{
	long t = (long)(ticks * 5);
	if (blackout_ms && t >= blackout_at && t < blackout_at + blackout_ms) {
		frames_lost++;
		return 1;
	}
	if (loss_pct && rand() % 100 < loss_pct) {
		frames_lost++;
		return 1;
	}
	return 0;
}

/* As tcpip_output() on the switch: the frame starts after the TX descriptor. */
void tcpip_output(void)
{
	uint8_t *f = &uip_buf[RTL_FRAME_DESC_SIZE];

	if (f[12] == 0x08 && f[13] == 0x00) {	/* the ASIC's job on the switch */
		uint8_t *ip = &uip_buf[UIP_LLH_LEN];
		int iplen = ip[2] << 8 | ip[3], hl = (ip[0] & 0x0f) * 4;
		uint16_t c;

		ip[10] = ip[11] = 0;
		c = csum_fold(csum_add(0, ip, hl));
		ip[10] = c >> 8; ip[11] = c;
		if (ip[9] == UIP_PROTO_TCP && iplen > hl) {
			uint8_t *tcp = ip + hl;
			uint32_t sum = csum_add(0, ip + 12, 8);	/* addresses */

			sum += UIP_PROTO_TCP + (uint32_t)(iplen - hl);
			tcp[16] = tcp[17] = 0;
			c = csum_fold(csum_add(sum, tcp, iplen - hl));
			tcp[16] = c >> 8; tcp[17] = c;
		}
	}
	if (debug_rst && f[12] == 0x08 && f[13] == 0x00 && f[14 + 9] == UIP_PROTO_TCP) {
		uint8_t *t = f + 14 + (f[14] & 0x0f) * 4;
		if (t[13] & 0x04)	/* RST */
			printf("webtap: %6.2fs RST to port %u, flags %02x; takeover %u idle %u timeout %u\n",
			       ticks / 200.0, t[2] << 8 | t[3], t[13],
			       uip_stat.tcp.takeover, uip_stat.tcp.idle, uip_stat.tcp.timeout);
	}
	if (lose())
		return;
	frames_out++;
	if (write(tapfd, f, uip_len) < 0)
		perror("tap write");
}

/* As the ASIC delivers to the CPU: RTL tag and 802.1Q tag after the addresses */
static void rx_frame(const uint8_t *f, int n)
{
	if (n < 14 || n - 14 + UIP_LLH_LEN > UIP_BUFSIZE)
		return;
	if (lose())
		return;
	frames_in++;
	memcpy(uip_buf, f, 12);
	uip_buf[12] = 0x88; uip_buf[13] = 0x99;	/* RTL_FRAME_TAG_ID */
	uip_buf[14] = RTL_FRAME_TAG_VERSION; uip_buf[15] = 0;
	uip_buf[16] = 0; uip_buf[17] = 0;
	uip_buf[18] = 0; uip_buf[19] = 4;		/* from port 4 */
	uip_buf[20] = 0x81; uip_buf[21] = 0x00;
	uip_buf[22] = 0x00; uip_buf[23] = 0x01;	/* VLAN 1 */
	uip_buf[24] = f[12]; uip_buf[25] = f[13];
	memcpy(&uip_buf[UIP_LLH_LEN], f + 14, n - 14);
	uip_len = UIP_LLH_LEN + n - 14;

	if (f[12] == 0x08 && f[13] == 0x06) {
		if (debug) {
			uint8_t *h = (uint8_t *)uip_hostaddr;
			printf("webtap: ARP uip_len %u, op %02x%02x, target %u.%u.%u.%u, us %u.%u.%u.%u\n",
			       uip_len, uip_buf[32], uip_buf[33], uip_buf[50], uip_buf[51], uip_buf[52], uip_buf[53],
			       h[0], h[1], h[2], h[3]);
		}
		uip_arp_arpin();
		if (debug)
			printf("webtap: ARP in, %s\n", uip_len ? "reply" : "no reply");
		if (uip_len)
			tcpip_output();
	} else if (f[12] == 0x08 && f[13] == 0x00) {
		uip_arp_ipin();
		uip_input();
		if (uip_len) {
			uip_arp_out();
			tcpip_output();
		}
	}
}

/* httpd asks for this once an upload's verdict has been acknowledged */
void reset_chip(void)
{
	printf("\nwebtap: reset requested\n");
	reset_pending = 1;
}

extern char passwd[];

static void boot(void)
{
	uip_init();
	/* Byte by byte, as the "ip" command sets them on the switch */
	memcpy(uip_hostaddr, (uint8_t []){ 10, 77, 0, 2 }, 4);
	memcpy(uip_draddr, (uint8_t []){ 10, 77, 0, 1 }, 4);
	memcpy(uip_netmask, (uint8_t []){ 255, 255, 255, 0 }, 4);
	uip_arp_init();
	httpd_init();
	strcpy(passwd, "1234");
}

static int tap_open(void)
{
	struct ifreq ifr;
	struct sockaddr_in *sin = (struct sockaddr_in *)&ifr.ifr_addr;
	int fd = open("/dev/net/tun", O_RDWR), s;

	if (fd < 0)
		return -1;
	memset(&ifr, 0, sizeof(ifr));
	ifr.ifr_flags = IFF_TAP | IFF_NO_PI;
	strncpy(ifr.ifr_name, TAP_NAME, IFNAMSIZ - 1);
	if (ioctl(fd, TUNSETIFF, &ifr) < 0)
		return -1;
	s = socket(AF_INET, SOCK_DGRAM, 0);
	sin->sin_family = AF_INET;
	inet_pton(AF_INET, HOST_IP, &sin->sin_addr);
	if (ioctl(s, SIOCSIFADDR, &ifr) < 0)
		return -1;
	inet_pton(AF_INET, "255.255.255.0", &sin->sin_addr);
	if (ioctl(s, SIOCSIFNETMASK, &ifr) < 0)
		return -1;
	if (ioctl(s, SIOCGIFFLAGS, &ifr) < 0)
		return -1;
	ifr.ifr_flags |= IFF_UP | IFF_RUNNING;
	if (ioctl(s, SIOCSIFFLAGS, &ifr) < 0)
		return -1;
	close(s);
	return fd;
}

static long long now_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (long long)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

int main(int argc, char **argv)
{
	FILE *img;
	size_t got;
	long long next_tick;
	uint8_t frame[2048];

	if (argc < 2) {
		fprintf(stderr, "usage: %s <firmware.bin> [loss-percent]\n", argv[0]);
		return 2;
	}
	memset(flash, 0xff, sizeof(flash));
	img = fopen(argv[1], "rb");
	if (!img) {
		perror(argv[1]);
		return 1;
	}
	got = fread(flash, 1, FLASH_SIZE, img);
	fclose(img);
	if (argc > 2)
		loss_pct = atoi(argv[2]);
	quiet = getenv("WEBTAP_QUIET") != NULL;
	if (getenv("WEBTAP_BLACKOUT"))	/* drop every frame for <ms> from <s> after start */
		sscanf(getenv("WEBTAP_BLACKOUT"), "%ld,%ld", &blackout_at, &blackout_ms),
		blackout_at *= 1000;
	debug = getenv("WEBTAP_DEBUG") != NULL;
	debug_rst = getenv("WEBTAP_RST") != NULL;	/* log every reset sent */
	if (getenv("WEBTAP_CPU_US"))	/* microseconds of "CPU" per frame handled */
		cpu_us = atoi(getenv("WEBTAP_CPU_US"));
	srand((unsigned)now_ns());

	tapfd = tap_open();
	if (tapfd < 0) {
		perror("tap");
		return 1;
	}
	boot();
	printf("webtap: %zu bytes of firmware, http://10.77.0.2/ on %s, loss %d%%\n",
	       got, TAP_NAME, loss_pct);
	fflush(stdout);

	next_tick = now_ns() + TICK_NS;
	for (;;) {
		struct pollfd p = { .fd = tapfd, .events = POLLIN };
		long long wait = (next_tick - now_ns()) / 1000000;
		int n;

		if (poll(&p, 1, wait > 0 ? (int)wait : 0) > 0) {
			n = read(tapfd, frame, sizeof(frame));
			if (debug)
				printf("webtap: frame in, %d bytes, type %02x%02x\n", n, frame[12], frame[13]);
			if (n > 0 && !reset_pending) {
				rx_frame(frame, n);
				if (cpu_us)	/* the 8051 is far slower than this host */
					usleep(cpu_us);
			}
		}
		while (now_ns() >= next_tick) {
			next_tick += TICK_NS;
			ticks++;
			/* handle_tx(), called from handle_tick() on every tick */
			for (uint8_t i = 0; i < UIP_CONNS; i++) {
				uip_periodic(i);
				if (uip_len > 0) {
					uip_arp_out();
					tcpip_output();
				}
			}
			if (ticks % 2000 == 0)
				uip_arp_timer();
			if (reset_pending && ++reset_pending > 400) {
				reset_pending = 0;
				boot();
				printf("webtap: back up after the reset\n");
			}
			if (ticks % 2000 == 0 && getenv("WEBTAP_STATS"))
				printf("webtap: frames in %lu out %lu lost %lu; syndrop %u rexmit %u\n",
				       frames_in, frames_out, frames_lost,
				       (unsigned)uip_stat.tcp.syndrop, (unsigned)uip_stat.tcp.rexmit);
			fflush(stdout);
		}
	}
}
