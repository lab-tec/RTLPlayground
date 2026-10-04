#ifndef _CABLE_H_
#define _CABLE_H_

#include <stdint.h>

/* Or'ed into cable_test()'s port to test a port that has a link */
#define CABLE_FORCE	0x80

void cable_test(uint8_t arg) __banked;

#endif
