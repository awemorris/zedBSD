# Copyright (C) 2026 Awe Morris
# SPDX-License-Identifier: Zlib
# M-x gdb: --fullname source following opens the file in the other
# window; the C-a C-a C-b chord issues a break command for the line.
KEYS = [
    (0.5, b"\x1bx"),
    (0.4, b"gdb\r"),
    (0.5, b"/tmp/gudtest\r"),
    (3.5, b"break main\rrun\r"),            # stop at main -> source shows
    (4.0, b"\x18o"),                        # C-x o -> source window
    (1.0, b"\x0e\x0e"),                     # C-n C-n
    (0.4, b"\x01"),                         # C-a
    (0.4, b"\x01"),                         # C-a again -> arm
    (0.4, b"\x02"),                         # C-b -> break here
    (1.5, b"\x18\x03"),
]
# Source following put gudtest.c in the other window at the stop line,
# and the breakpoint chord sent a "break FILE:LINE" command.
EXPECT = ["gudtest.c", "int x = add(20, 22);", "break gudtest.c:13"]
