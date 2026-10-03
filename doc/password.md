# The password and the web interface

The admin password is a `passwd` line in the startup configuration, replayed
at boot like any other command. Without care, the web interface shows it to
anyone logged in: in the System page's configuration box, and in the command
log the console keeps.

`cfgpass.c` keeps it out of both, without changing the configuration format,
so older images still read it:

- **GET /config** and **GET /cmd_log** drop every `passwd` line before
  sending.
- **POST /config**: since the web interface never sees the password, what it
  saves has no `passwd` line, and the running password is put back as the
  first line. A configuration that does carry a `passwd` line sets that
  password, as before.

Changing the password still goes through the System page (or `passwd <new>`
on the console), then a save to flash.

What it doesn't change: the password is still plain text in flash, readable
with a clip on the flash chip, and still crosses the network in clear at
login, since the web interface is HTTP only.

One limit: a reply covers about 2.4 kB of configuration and the rest follows
straight from flash, unfiltered. Saving through the web interface puts the
password first, so it is always in the filtered part; only a configuration
over 2.4 kB written another way, with `passwd` near its end, would show it.

`test/test_cfgpass.c` covers both directions and the round trip.
