# The memtest command

`memtest` checks, on the device, the external RAM (XRAM) that the firmware
leaves free. The linker is told there are 48 kB (`--xram-size 49151`), a
figure measured on one RTL837x switch (commit eb4bae2); every new buffer a
feature adds comes out of the free part, so this confirms it on the switch at
hand:

```
> memtest
memtest: free external RAM, a second or two; management pauses meanwhile
Flash:    2 MB, from the flash chip's ID
XRAM:     0x0000-0x3eea in use, testing 0x3eeb-0xbfff
Copies:   none
Patterns: OK
memtest: PASS, XRAM works to 0xbfff
```

- **Flash** is the size the flash chip reports about itself (its JEDEC
  capacity code), read at boot.
- **XRAM** gives the part the build uses, from the linker's own symbols
  (`s_XSEG`/`l_XSEG`, `s_XISEG`/`l_XISEG`), and the free part above it that
  is tested.
- **Copies** looks for RAM smaller than it seems: an address in the free part
  that is a second name for one 16 or 32 kB below it. Each free byte is
  written while the byte below is watched, with interrupts held off, and the
  byte below is put back before they resume, so a copy is found without
  corrupting the variable that lives there. The first copy ends the test,
  before anything else is written.
- **Patterns** writes two patterns across the free part, the second the
  inverse of the first, and reads each back, so every bit is seen holding a 0
  and a 1. The pattern differs between neighbouring addresses. A failure names
  the number of wrong bytes and the first one, with what was written and
  read. The free part is left zeroed.

Nothing outside the free part is written, and nothing above 0xbfff is touched.
The main loop does not run during the test (a second or two): the console and
web interface pause, forwarding does not.

The stack lives in internal RAM, not here. How deep it gets is what the
`health` command reports in a `HEALTH=1` build (`doc/health.md`).

`test/test_memtest.c` runs the command against RAM models on the host: the
48 kB expected, 64 kB, RAM repeating every 16 or 32 kB, RAM ending at 40 kB
and a stuck bit, checking each time that the part in use comes out unchanged
and is never touched with interrupts enabled.
