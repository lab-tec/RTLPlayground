#ifndef _ROLLBACK_H_
#define _ROLLBACK_H_

#include <stdbool.h>

/* A copy of the previous firmware: everything below CONFIG_START, i.e. code,
 * web interface and default configuration, but not the startup configuration.
 * Needs a 2 MB flash: it sits behind the 512 kB upload area. */
#define ROLLBACK_START		0x100000UL
#define ROLLBACK_LEN		CONFIG_START
/* One sector after the copy: magic, CRC16 of the copy, state, version */
#define ROLLBACK_HDR		(ROLLBACK_START + ROLLBACK_LEN)

void rollback_backup(void);
bool rollback_check(void);
void rollback_status(void);
void rollback_restore(void);

#endif
