# Copyright (C) 2026 Awe Morris
# SPDX-License-Identifier: Zlib
# A buffer-local binding shadows the global one; other buffers are
# unaffected. M-x test hook: we use goto-line's minibuffer to verify
# the mode line instead — simpler: shell of the test uses M-x commands.
KEYS = [
    (0.4, b"hello"),
    (0.3, b"\x18\x03"),
]
EXPECT = ["(Fundamental)"]
