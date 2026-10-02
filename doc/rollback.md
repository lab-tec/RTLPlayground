# Rolling back a firmware update

An upload that passes its checksum but does not boot properly used to need a
flash programmer to undo. On devices with a 2 MB flash, every update now keeps
a copy of the firmware it replaces, and that copy can be put back with the
reset button or a command.

## What is kept

When an uploaded image has passed its checksum and is about to be moved into
place, the running firmware (everything below the startup configuration at
`0x70000`: code, web interface and default configuration) is copied to
`0x100000`. A header sector at `0x170000` follows it: a magic, the CRC16 of the
copy, a state byte and the version string of the copied firmware.

The startup configuration is never part of it: an update leaves it alone, and
so does a rollback.

Protection starts with the update *after* the one that installs this feature:
the firmware being replaced does the copying, so it has to know how.

## Rolling back

- **Button:** hold the reset button while powering on. After about 3 s the
  system LED blinks fast; let go while it does (between 3 and 8 s). Held for
  10-30 s the button still restores the default configuration, as before.
  The button is read right after the update check, before the network is
  brought up, so a firmware that hangs during bring-up can still be rolled
  back.
- **Command:** `rollback restore`, on the console or the web interface's
  console box.

Either way the copy is checked against its CRC, written to the upload area as
an ordinary update image, and the switch resets; the normal update path then
installs it. The copy is kept rather than replaced with the firmware being
rolled back, so it can be restored again.

`rollback` on its own shows the version held and whether its CRC is good. The
check reads 448 kB of flash, so it takes a few seconds.

## What it does not cover

- A firmware that fails before the button is read (clock, interrupts, chip
  set-up at the very start of the boot), or a broken copy routine.
- Devices with less than 2 MB of flash: the update says so and goes ahead
  without a copy.

The host test `test/test_rollback.c` runs update, rollback and update again
against a simulated flash.
