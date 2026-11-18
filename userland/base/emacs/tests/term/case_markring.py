# Copyright (C) 2026 Awe Morris
# SPDX-License-Identifier: Zlib
# C-SPC marks travel the ring; C-u C-SPC jumps back through them.
KEYS = [
    (0.4, b"one two three"),
    (0.2, b"\x1b<"),                 # M-< (pushes mark at eob=14)
    (0.2, b"\x00"),                  # C-SPC at 1
    (0.2, b"\x06\x06\x06\x06"),      # C-f x4
    (0.2, b"\x00"),                  # C-SPC at 5
    (0.2, b"\x05"),                  # C-e
    (0.2, b"\x15\x00"),              # C-u C-SPC -> jump to mark (5)
    (0.2, b"A"),
    (0.2, b"\x15\x00"),              # C-u C-SPC -> next older (1)
    (0.2, b"B"),
    (0.2, b"\x18\x03"),
]
EXPECT = ["Bone Atwo three"]
