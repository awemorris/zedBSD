# WS019-p020: reliable df capacity observations

Date: 2026-09-09
Status: completed (q151); [results](results.md)
Parent: [WS019](../ws.md)
Timebox: 60 active minutes

The installer admission design requires complete successful capacity output.
Current df silently wraps block conversion/percentage arithmetic and ignores
buffered stdout errors. Fix the existing standard command before consuming it.
Retain its 512-byte default, -k and operand columns; implement the standard -P
form using the existing portable single-line output. No new helper command.

Use checked quotient/remainder arithmetic, reject inconsistent statvfs free
counts, preserve per-operand errors and report final flush failure. Avoid
mutating argv for the default root operand. Cover zero/full filesystems,
rounding, maximum representable conversions, overflow, inconsistent records,
multiple operands, option handling and buffered output failure with an
independent host fixture. Run ordinary and ASan/UBSan fixtures, then the three
maintained x86 builds (-j16 with explicit CI configs). Do not repeat unrelated
runtime campaigns. p004 remains incomplete until public admission/integration.
