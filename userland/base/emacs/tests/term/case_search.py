# Copyright (C) 2026 Awe Morris
# SPDX-License-Identifier: Zlib
# C-s prompts for a string and moves point past the match.
KEYS = [
    (0.4, b"alpha beta gamma"),
    (0.2, b"\x1b<"),                # M-< back to the top
    (0.2, b"\x13"),                 # C-s
    (0.3, b"beta\r"),
    (0.2, b"X"),                    # insert right after the match
    (0.2, b"\x18\x03"),
]
EXPECT = ["alpha betaX gamma"]
