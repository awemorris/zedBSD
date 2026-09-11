# q147 terminal identity and WS006 closure

Date: 2026-09-09
Status: completed

`isatty` now uses the shared termios contract. `ttyname_r` resolves the matching
character-device identity under `/dev` and `/dev/pts`; it no longer names every
terminal `/dev/console`. Errors and short output buffers are checked explicitly.

Evidence in `plan/ws006/temp/q147b/`:

- `results.tsv`: every gate passes; xHCI and paired EHCI/UHCI ordinary builds.
- Both `guest.log` files: native console/master/slave/dup terminal checks,
  pathname identity, invalid descriptors, pipe/file rejection and buffer bounds.
- Both topologies pass keyboard, relative/absolute pointer, hotplug/stale-fd
  checks and 64 MiB concurrent USB-root read.
- Both Xzed/zterm sessions execute `tty > /dev/console` and report `/dev/pts/N`
  before the outer console's sleep finishes. USB keyboard writes the exact
  marker file. After graphical shutdown the ordinary TTY reads that marker.
  No graphical command replay into the outer console was observed.
- GUI before/after/restored screenshots supplement the live PTY oracle.
- Explicit amd64, PCAT and PC98 `make -j16` gates pass; logs are
  `/tmp/zedbsd-q147-{amd64,pcat,pc98}.log`.

The first q147 attempt stopped at host fixture syntax because target stat
headers needed the LP64 ABI selector. The fixture was corrected; q147b is the
successful complete replay. No sanitizer or guest assertion was relaxed.

Combined with q126 source/host/Noct BeUI acceptance, q141 checked paired halt
and UHCI ownership repair, q142 heap/lifecycle closure, and the user's p008
physical HID confirmation, this closes p009, p010, p011 and WS006. It adds no
new physical observation. The old unrelated PC98 login failure remains owned
by WS002-p021. No production graphics/TTY ownership change was needed.
