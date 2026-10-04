# webtap: the web interface against real browsers

`test/webtap.c` runs the firmware's own TCP/IP stack and web server, compiled
for the host from the unmodified `uip/uip.c`, `uip/uip_arp.c`,
`httpd/httpd.c` and `httpd/page_impl.c`, on a Linux TAP interface. A browser,
curl or Playwright on the same machine then talks to it exactly as it would to
the switch:
- one TCP connection;
- the login sessions;
- uIP's timers on the 5 ms tick;
- the web files read out of a built firmware image.

```
make MACHINE=...                      # a firmware build provides html_data.c
make -C test webtap
test/build/webtap output/<board>/rtlplayground-*.bin [loss-percent] &   # needs CAP_NET_ADMIN
node test/webtap-scenarios.js output/<board>/rtlplayground-*.bin        # Playwright
```

It answers on `http://10.77.0.2/` (the host side is `10.77.0.1`), password
`1234`. Frames get the CPU tag and VLAN tag the switch's ASIC adds, and the
IP and TCP checksums the ASIC computes.

**Knobs:**
- **`WEBTAP_CPU_US=<n>`:** a delay per received frame, standing in for the
  8051's speed.
- **The second argument:** random loss, in percent.
- **`WEBTAP_BLACKOUT=<s>,<ms>`:** drops everything for `<ms>`, starting `<s>`
  after start.
- **Logging:** `WEBTAP_STATS` prints counters every 10 s, `WEBTAP_RST`
  prints every reset sent, and `WEBTAP_DEBUG` traces frames.

**Not modelled:**
- the console behind `/cmd`: a stub answers each command with one line,
  except `tcp`;
- flash timing;
- installing an upload: the reset it asks for re-initialises the stack after
  2 s.

## What it showed (2026-10-04)

The scenarios in `test/webtap-scenarios.js` use two separate browsers (two
machines), with a 3 ms per-frame delay standing in for the 8051. On the code
before these changes, the same as upstream's web server and uIP:

| Scenario | Before | After |
|---|---|---|
| A second browser logs in | the first is logged out within seconds (one session) | both stay logged in (four sessions) |
| A browser's unused spare connection holds the only connection | other logins hang up to 30 s | handed over once it has been silent 2 s; closing connections at once |
| An upload from a browser that was logged out | "rejected (bad checksum)" | "not logged in any more ... (HTTP 401)", and the login page says why the session ended |
| Downloads across a 2 s total outage | given up after about 1.25 s (a stall or a cut-off page) | completed, in about 2–3 s |
| Fresh logins over 5% or 10% random loss | a few in five fail | the same: a single connection with one segment in flight suffers either way |

**One flaw in the idle reaping (#390)** only showed once its timeout was
shortened.
- On completing the handshake, `timer` kept the SYNACK's retransmission
  timeout, so a fresh connection started its idle count at about 3 s.
- Harmless at 30 s; with a short timeout it reset connections before their
  first request arrived.
- Fixed by zeroing the count when the handshake completes.

**The settings and the reasoning** are in `uip/uip-conf.h` and the comment in
`uip/uip.c`'s `found_listen`. On the switch, `tcp` prints the counters that
show how often each case happens.
