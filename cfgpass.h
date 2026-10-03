#ifndef _CFGPASS_H_
#define _CFGPASS_H_

#include <stdint.h>

/* The text the two work on, updated in place */
extern __xdata uint8_t * __xdata cfgpass_buf;
extern __xdata uint16_t cfgpass_len;
/* cfgpass_keep() may write as far back as this, no further */
extern __xdata uint8_t * __xdata cfgpass_floor;

void cfgpass_hide(void) __banked;
/* 1: the text now sets a password; 0: no room to add the line */
uint8_t cfgpass_keep(void) __banked;

#endif
