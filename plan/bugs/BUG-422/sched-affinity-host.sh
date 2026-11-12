#!/bin/sh
# BUG-422's host test: compiles src/kern/sched.c into plan/bugs/BUG-422/sched-affinity-host.c freestanding against
# the kernel's headers, links it with the host C library and runs it.  usage: plan/bugs/BUG-422/sched-affinity-host.sh
# (from the repository's top; OUT= to choose the build folder, CLANG= for the compiler)
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -eu
OUT=${OUT:-build/bug422-host}
CLANG=${CLANG:-clang}
resource=$("$CLANG" -print-resource-dir)
mkdir -p "$OUT"
"$CLANG" -std=gnu11 -ffreestanding -nostdinc -isystem "$resource/include" -D__ZEDBSD__ -DHAL_ARCH_AMD64 \
	-Iinclude -Isrc -O1 -g -fno-stack-protector -Wall -Werror -Wno-unused-function -Wno-unused-parameter \
	-c -o "$OUT/sched-affinity-host.o" plan/bugs/BUG-422/sched-affinity-host.c
"$CLANG" -o "$OUT/sched-affinity-host" "$OUT/sched-affinity-host.o"
timeout 60 "$OUT/sched-affinity-host"
