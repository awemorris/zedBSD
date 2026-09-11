# WS006-p011: terminal identity for graphical PTYs

Date: 2026-09-09
Status: completed (q147)
Parent: [WS006](../ws.md)
Timebox: 60 active minutes
Dependency: p009 q146 actual Xzed/zterm failure.

Evidence: [q147 results](results.md).

## Established defect

The GUI receives pointer focus and executes `tty > /dev/console`, but reports
`not a tty`. `userland/base/libc/posix.c` implements isatty using only the console
private ioctl, and ttyname/ttyname_r always name `/dev/console`. PTY slaves
implement the same TCGETS/termios contract as other terminals.

## Implementation and acceptance

1. Use the common termios query for isatty, preserving EBADF vs ENOTTY.
2. Resolve terminal names by fstat/stat device/inode identity under `/dev` and
   `/dev/pts`, including console/VT and PTY paths. Do not hardcode a PTY rdev
   encoding or invent an ioctl. Return proper errors and respect buffer bounds.
3. Add a native PTY fixture to the maintained USB/GUI image: master/slave/dup,
   path identity, short output, invalid descriptor and non-terminal cases.
4. Run explicit three-platform builds, native PTY checks and actual Xzed USB
   pointer/keyboard acceptance. Keep the timed pre-return PTY proof so queued
   ordinary-console input cannot falsely pass the GUI gate.
5. Investigate any observed duplicate delivery to the sleeping outer console
   separately. A broader graphics/TTY ownership change needs its own phase;
   do not silently alter evdev grab or restore legacy console-event ioctls.
