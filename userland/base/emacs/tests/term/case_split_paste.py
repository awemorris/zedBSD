# Copyright (C) 2026 Awe Morris
# SPDX-License-Identifier: Zlib
# Regression (storyfuzz seed 1016): pasting many wrapping lines into a
# vertically split window must redisplay promptly -- the scroll-fit
# loop used to recompute the same start line ~500 times per frame.
lines = '\r'.join('word%02d alpha beta gamma delta epsilon' % i
                  for i in range(30))
KEYS = [
    (0.4, b"\x183"),                 # C-x 3: vertical split
    (0.5, lines.encode()),           # paste burst (wraps in 40 cols)
    (0.8, b"\x1b<"),                 # M-< to buffer start
    (0.3, b"TOPMARK "),
    (0.4, b"\x18\x03"),
]
EXPECT = ["TOPMARK word00", "word01"]
