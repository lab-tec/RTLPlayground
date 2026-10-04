# Diagnostics and monitoring

Six console commands and a Diagnostics page in the web interface, modelled on
what professional switches offer. They all report through the console, and so
through syslog when it is on; nothing here changes how traffic is switched.
Each line starts with a tag (`revert:`, `link:`, `boot:`, `temp:`, `mac:`,
`ping:`) so a log system can filter and alert on it.

The code lives in bank 3 (`kadam.c` and one file per feature), keeps its state
in external RAM and takes no internal RAM; `test/test_kadam.c` runs all of it
on the host against the register mock.

## revert: the safe-change timer

```
> revert in 5
revert: armed; rebooting into the saved config in 5 min 0 s unless the config is saved or "revert cancel"
```

Like Juniper's "commit confirmed" or Cisco's "reload in": arm it before a risky
change. Changes are live at once but only in RAM until saved, so if one cuts
off the management path, the timer runs out and the switch reboots into its
saved configuration. Saving the configuration from the web interface, or
`revert cancel`, disarms it; `revert` shows the time left. The last five
minutes are announced once a minute.

## linkwatch: link health

Once a second the link state and speed of every port are compared with the
last reading; every ten seconds the RX CRC/alignment counter is read.

```
link: port 6 down
link: port 6 up at 1G, below its best since boot, 2.5G
link: port 6 flapping, 6 changes in 10 min
link: port 6, 12 CRC errors in the last minute
```

The first 20 s after boot are only recorded. `linkwatch` lists every port with
its state, changes in the current ten-minute window and CRC errors since boot.
It never shuts a port.

## thermal: the thermal guard

`thermal warn <C> crit <C>` (default 85/95; a startup-configuration line).
Crossing a threshold logs at once, a reading above one is repeated every ten
minutes, and coming back down is logged 3 C below the threshold. `thermal`
shows the reading, the thresholds and the state.

## boot: why the switch restarted

Thirty seconds after boot:

```
boot: v0.1.0-kadam-D-1a2b3c4 up, reason: upgrade; temp 71.9 C
```

The reason is a marker written at the top of the free external RAM just before
a software reset (`reboot.c`): the reset command, the web interface's reboot, a
firmware upload, an upgrade, a rollback, the revert timer, or the reset button
restoring the defaults. No marker means power-on, or a crash. Whether the
chip's reset keeps external RAM is what the line shows on the device; the
first boot after an upgrade *from* an image without this code reports
power-on. `boot` prints the line again.

## macwatch: the new-device log

The learned-address table is walked sixteen entries a second; a device not
seen before, or seen on another port, is logged:

```
mac: new 3c:22:fb:01:02:03 vlan 1 on port 7
mac: 3c:22:fb:01:02:03 vlan 1 moved from port 7 to port 3
```

The first full walk after boot (or after `macwatch on`) is the baseline and
only logs a count. `macwatch ports <ports>` limits reporting to some ports (a
port behind an access point would otherwise log every roaming phone); a move
is reported when either port is watched. Up to 512 devices are remembered
until reboot. `macwatch`, `macwatch on|off` and `macwatch ports` are
startup-configuration lines.

## ping

`ping <ip>` sends four echo requests in the background, one a second; `ping`
shows the result:

```
ping: 172.16.0.254: 4 sent, 4 received, rtt min/avg/max 0/1/5 ms
```

Times are in ticks of 5 ms. The first request can be lost to ARP resolution.
Replies reach `ping_reply()` from uIP's ICMP input, which otherwise drops
everything but echo requests.

## The Diagnostics page

Under Diagnostics in the web interface: a cable-test button per port (a port
with a link asks before testing it with `force`), buttons for `linkwatch`,
`health`, `thermal`, `macwatch`, `revert` and `boot`, and a ping box filled
with the gateway. Each button runs the console command through `/cmd` and
shows its output.
