# Copyright (C) 2026 Awe Morris
# SPDX-License-Identifier: Zlib
# C-u repeats: C-u 8 * inserts 8 stars; C-u C-f moves 4; M-5 x -> 5 x's.
KEYS = [
    (0.4, b"\x15" b"8" b"*"),        # C-u 8 *
    (0.2, b"\x1b5x"),                # M-5 x
    (0.2, b"\x1b<"),                 # M-<
    (0.2, b"\x15\x06"),              # C-u C-f (4 forward)
    (0.2, b"Y"),
    (0.2, b"\x18\x03"),
]
EXPECT = ["****Y****xxxxx"]
