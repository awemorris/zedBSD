# Copyright (C) 2026 Awe Morris
# SPDX-License-Identifier: Zlib
# Double-width characters occupy two columns; the cursor lands after
# them, so typed ASCII goes to the right place.
KEYS = [
    (0.5, "こんにちは".encode()),
    (0.2, b"\x02\x02"),          # C-b C-b (over は, ち)
    (0.2, b"X"),
    (0.3, b"\x18\x03"),
]
EXPECT = ["こんにXちは"]
