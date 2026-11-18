# Copyright (C) 2026 Awe Morris
# SPDX-License-Identifier: Zlib
# The terminal cursor sits in the minibuffer (echo line) during
# minibuffer input, after the prompt and the typed text.
KEYS = [
    (0.5, b"body text"),
    (0.3, b"\x18b"),             # C-x b -> "Switch to buffer: "
    (0.3, b"ab"),                # input: cursor after "ab"
    (0.4, b"\x07"),              # C-g quit
    (0.3, b"\x18\x03"),
]
# After key index 2 ("ab"): echo row 23 (0-based), col 18 + 2 = 20.
CURSOR_AFTER_KEY = 2
EXPECT_CURSOR = (23, 20)
EXPECT = ["body text"]
