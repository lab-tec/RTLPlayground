# The bench command

`bench` measures, on the device, how fast the 8051 core and its access to the
switch registers are. Each test repeats one piece of work for one second, timed
by the system tick, and prints how many times it ran:

```
> bench
bench: about 5 s, management pauses meanwhile
DJNZ instructions:   ... /s  (x.y clocks each at 125 MHz)
32-bit mul + add:    ... /s
256 B xdata copies:  ... /s
switch register reads: ... /s
port counter reads:  ... /s
```

- **DJNZ instructions** come from a loop written in assembler, so the count is
  exact: 125 MHz divided by it gives the clocks one instruction takes, which
  says what kind of 8051 core this is (a classic one takes 24 for a DJNZ, a
  single-cycle one 2 to 4).
- **32-bit mul + add** and **256 B xdata copies** are compiled C, the cost of
  ordinary arithmetic and buffer handling.
- **Switch register reads** and **port counter reads** (one MIB counter
  fetched through `STAT_GET`) are the path any statistics feature uses.

While it runs (about five seconds) the main loop does not: the console and web
interface pause. Forwarding is done by the switch hardware and carries on.
