# Copyright (C) 2026 Awe Morris
# SPDX-License-Identifier: Zlib
# A 50-line paste (Unix \n line endings, as terminals deliver pastes)
# must insert all lines and stay responsive.
KEYS = [
    (0.5, "".join("pasteline%02d\n" % i for i in range(50)).encode()),
    (0.6, b"MARKER"),
    (0.4, b"\x18\x03"),
]
EXPECT = ["pasteline49", "MARKER"]
