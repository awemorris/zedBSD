# Copyright (C) 2026 Awe Morris
# SPDX-License-Identifier: Zlib
# During isearch the terminal cursor sits at the end of the search
# string on the echo line, while the match keeps a reverse-video
# pseudo cursor in the buffer (not checked here; SGR is stripped by
# the harness Screen).
KEYS = [
    (0.5, b"one two three"),
    (0.2, b"\x1b<"),             # M-< to buffer start
    (0.3, b"\x13"),              # C-s isearch
    (0.3, b"tw"),                # search "tw"
    (0.4, b"\r"),                # RET: exit search at the match
    (0.3, b"\x18\x03"),
]
# After key index 3 ("tw"): echo row 23, col = len("I-search: tw") = 12.
CURSOR_AFTER_KEY = 3
EXPECT_CURSOR = (23, 12)
EXPECT = ["one two three"]
