#ifndef _KADAM_H_
#define _KADAM_H_

/*
 * Image D's features (KadamCloud): the safe-change timer, link health, the
 * boot announcement, the thermal guard, the new-device log and ping. All of
 * them live in bank 3 behind three hooks: kadam_init() before the startup
 * configuration is replayed, kadam_second() once a second from the main
 * loop, and kadam_cmd() for the console commands nothing else knows.
 */

#include <stdint.h>

/* Why the switch last restarted, kept across a software reset (reboot.c) */
#define REBOOT_NONE		0	/* no marker: power-on, or a crash */
#define REBOOT_SOFT		1	/* a reset with no reason given */
#define REBOOT_CMD		2	/* the reset command */
#define REBOOT_WEB		3	/* the web interface's reboot */
#define REBOOT_FWUPLOAD		4	/* a firmware upload, about to be installed */
#define REBOOT_UPGRADE		5	/* a new firmware installed */
#define REBOOT_ROLLBACK		6	/* the firmware backup installed */
#define REBOOT_REVERT		7	/* the safe-change timer ran out */
#define REBOOT_DEFAULTS		8	/* the button restored the default config */

/* Common area: callable from every bank, before and during a reset */
void reboot_note(uint8_t why);
uint8_t reboot_why(void);

/* The hooks */
void kadam_init(void) __banked;
void kadam_second(void) __banked;
uint8_t kadam_cmd(void) __banked;
/* The web interface saved the configuration (cancels the revert timer) */
void revert_saved(void) __banked;
/* uip.c hands over every ICMP echo reply */
void ping_reply(void) __banked;

/* ---- between the modules, all in bank 3 ---- */
void revert_init(void);
void revert_second(void);
void revert_cmd(void);
void thermal_init(void);
void thermal_second(void);
void thermal_cmd(void);
void thermal_print_now(void);
void bootmsg_init(void);
void bootmsg_second(void);
void bootmsg_cmd(void);
void linkwatch_init(void);
void linkwatch_second(void);
void linkwatch_cmd(void);
void macwatch_init(void);
void macwatch_second(void);
void macwatch_cmd(void);
void ping_init(void);
void ping_second(void);
void ping_cmd(void);

/* Command-line helpers (kadam.c), on the words cmd_tokenize() found */
uint8_t kc_is(uint8_t word, __code const char *s);
uint8_t kc_num(uint8_t word);		/* decimal, into kc_val */
uint8_t kc_ip(uint8_t word);		/* dotted quad, into kc_addr */
uint8_t kc_port(uint8_t word);		/* front-panel port, into kc_val as a logical port */
void kc_usage(__code const char *usage);
void kc_print_port(void);		/* kc_out_port as its front-panel number */
void kc_print_ip(void);			/* kc_out_ip, dotted */
void kc_print_u32(void);		/* kc_out_u32 in decimal */
extern __xdata uint8_t cmd_words_len;	/* cmd_parser.c */
extern __xdata uint16_t kc_val;
extern __xdata uint8_t kc_addr[4];
extern __xdata uint8_t kc_out_port;
extern __xdata uint8_t kc_out_ip[4];
extern __xdata uint32_t kc_out_u32;

#endif
