# Copyright (C) 2026 Awe Morris
# SPDX-License-Identifier: Zlib
# M-% asks per match: y replaces, n skips, ! finishes the rest.
KEYS = [
    (0.4, b"aa bb aa bb aa bb aa"),
    (0.2, b"\x1b<"),               # M-<
    (0.2, b"\x1b%"),               # M-%
    (0.3, b"aa\r"),
    (0.3, b"XX\r"),
    (0.3, b"y"),                   # replace 1st
    (0.3, b"n"),                   # skip 2nd
    (0.3, b"!"),                   # rest
    (0.3, b"\x18\x03"),
]
EXPECT = ["XX bb aa bb XX bb XX", "Replaced 3 occurrences"]
