# Console TTY and job control

Copyright (C) 2026 Awe Morris
SPDX-License-Identifier: Zlib

HAL provides only character-console events. A kernel input worker is the sole
consumer after early boot and feeds the console TTY line discipline. This
keeps blocking reads and poll readiness attached to the same queue and avoids
lost input between the shell and user processes.

The console TTY supports canonical and noncanonical input, echo flags, erase,
kill, EOF and signal-generating control characters. `VMIN`/`VTIME` behavior is
implemented for noncanonical reads. Output processing handles the supported
newline and carriage-return transformations.

The TTY stores a foreground process group. Background reads and terminal state
changes apply the zedBSD job-control checks and generate the appropriate TTY
signals. `tcgetattr()`, `tcsetattr()`, `tcgetpgrp()` and `tcsetpgrp()` operate on
the `/dev/console` descriptor. Session login accounting and pseudoterminals are
not implemented.

Readiness is level-triggered: readable canonical records or raw bytes produce
`POLLIN`, output capacity produces `POLLOUT`, and hangup/error conditions are
reported independently of requested bits.
