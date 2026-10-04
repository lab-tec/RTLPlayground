/*
 * "tcp": uIP's TCP counters since boot, to judge the web interface's
 * connection from the switch's side.
 *
 *   tcp: 3 connection requests dropped while busy, 1 handed a quiet client's
 *   connection, 0 reset when idle, 0 given up; 12 retransmissions
 *
 * - Dropped while busy: the one connection was in use (UIP_CONNS is 1), so a
 *   browser's SYN went unanswered and it retried a second or more later.
 * - Handed a quiet client's connection: the SYN got the connection of a
 *   client silent for over a second (a browser's unused spare connection).
 * - Reset when idle: UIP_IDLE_TIMEOUT seconds without a segment.
 * - Given up: UIP_MAXRTX retransmissions without an acknowledgement.
 *
 * The counters are 16 bits and wrap.
 */

#pragma codeseg BANK3
#pragma constseg BANK3

#include <stdint.h>
#include "rtl837x_common.h"
#include "uip/uip.h"
#include "tcpstat.h"

void tcp_stats(void) __banked
{
	print_string("tcp: ");
	itoa_short(uip_stat.tcp.syndrop);
	print_string(" connection requests dropped while busy, ");
	itoa_short(uip_stat.tcp.takeover);
	print_string(" handed a quiet client's connection, ");
	itoa_short(uip_stat.tcp.idle);
	print_string(" reset when idle, ");
	itoa_short(uip_stat.tcp.timeout);
	print_string(" given up; ");
	itoa_short(uip_stat.tcp.rexmit);
	print_string(" retransmissions\n");
}
