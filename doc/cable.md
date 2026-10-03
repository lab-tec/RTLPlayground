# The cable command

`cable <port>` runs the PHY's built-in cable test (Realtek calls it RTCT). The
PHY sends test pulses down each pair and reports, per pair, whether the cable
is fine, open or shorted, and how far away the fault is:

```
> cable 3
cable: testing, a few seconds; management pauses meanwhile
Pair A: open at 0.0 m
Pair B: open at 0.0 m
Pair C: open at 0.0 m
Pair D: open at 0.0 m
```

- **OK**: the pair is terminated by a link partner or a good cable run.
- **open**: the pair ends unterminated at that distance. Every pair of an
  empty port reads open at about 0 m; a cable with nothing at the far end
  reads open at its length.
- **short within the pair** / **short to another pair**: a fault at that
  distance.
- **unknown** / **no result**: the PHY gave no usable answer for that pair.

Distances come from the PHY's count, less a 620 offset, at 78 per metre, as
the Linux driver converts them. Expect them to be approximate.

## Where it runs

The register sequence is the RTL8224's, taken from the Linux driver
(`drivers/net/phy/realtek/realtek_main.c`, `rtl8224_cable_test_*`). The
command first reads the port's PHY identifier and runs only on an RTL8224
(`0x001c 0xcad0`). Any other PHY gets its identifier printed and nothing
written, because the same registers may mean something else there. On an
RTL8373 switch that means the ports behind the RTL8224; whether the RTL8373's
own PHYs have the same test is unknown until someone documents it.

The SFP port and a port switched off (`port <n> off`) are refused.

## It takes the link down

The test needs the line quiet: the PHY is forced to 1000/full with
auto-negotiation off, left 500 ms to settle, and tested; afterwards its
control register is put back as it was and auto-negotiation restarted if it
was on. So a port with a link is refused unless asked for explicitly:

```
> cable 6 force
```

drops that link for a few seconds and renegotiates afterwards. The main loop
pauses while the test runs (up to about 10 s if the PHY never finishes);
forwarding on the other ports carries on.

`test/test_cable.c` runs the command against a model of the RTL8224's
registers: the write sequence against the Linux driver's, the decoding of
every result code and of distances, the refusals, and the port being put back.
