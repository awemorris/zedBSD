# Copyright (C) 2026 Awe Morris
# SPDX-License-Identifier: Zlib
# Incremental search: matches move live as the string grows, C-s
# repeats, RET exits at the match.
KEYS = [
    (0.4, b"alpha beta alpha gamma"),
    (0.2, b"\x1b<"),          # M-<
    (0.2, b"\x13"),           # C-s isearch
    (0.2, b"al"),             # match 1st "al(pha)"
    (0.2, b"\x13"),           # C-s -> 2nd "alpha"
    (0.2, b"\r"),             # RET exit
    (0.2, b"X"),              # mark the landing point
    (0.2, b"\x18\x03"),
]
EXPECT = ["alpha beta alXpha gamma", "Mark saved where search started"]
