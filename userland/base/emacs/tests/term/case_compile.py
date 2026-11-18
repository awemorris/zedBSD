# Copyright (C) 2026 Awe Morris
# SPDX-License-Identifier: Zlib
# M-x compile builds; M-g M-n visits the first error's file:line in the
# other window.
KEYS = [
    (0.5, b"\x1bxcompile\r"),               # M-x compile (prompt prefilled "make -k")
    (0.3, b"\x7f\x7f\x7f\x7f\x7f\x7f\x7f"),  # clear the default
    (0.3, b"gcc -c /tmp/comptest.c -o /tmp/x.o\r"),
    (2.5, b""),                             # compile + fail
    (0.6, b"\x1bg\x1bn"),                   # M-g M-n next-error
    (1.2, b"\x18\x03"),
]
EXPECT = ["comptest.c", "error", "remacs: comptest.c"]
