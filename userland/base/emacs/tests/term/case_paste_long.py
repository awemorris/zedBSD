# Copyright (C) 2026 Awe Morris
# SPDX-License-Identifier: Zlib
# A single-line paste longer than 1KB must arrive whole.
#
# The command loop coalesces a burst of plain characters into one insert
# (selfInsertRun), building the run by string concatenation. Concatenation
# in the VM used to be formatted through a fixed 1KB buffer and dropped
# everything past 1023 bytes without an error, so a paste of one long
# line silently lost its tail. case_paste.py does not catch it: its lines
# end in newlines, which break the run into ~12-character pieces.
KEYS = [
    (0.8, b"A" * 1200 + b"TAIL"),
    (0.4, b"\x18\x03"),
]
EXPECT = ["TAIL"]
