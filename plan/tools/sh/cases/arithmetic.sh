# ws065: arithmetic that must keep its POSIX meaning now that the shell
# also has bash's ++ and -- (scored against dash).
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib

#### -- between operands is two minus signs
x=3
echo $((1--1)) $((--5)) $((x--1)) $((x - -1)) $x
