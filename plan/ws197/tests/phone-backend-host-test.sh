#!/bin/sh
# The host test of the desktop's phone backend (ws197-p004a, plan/ws197/phase004/phase.md section 12): builds
# plan/ws197/tests/phone-backend-host-test.c, which includes userland/desktop/libkeiland-backend-zedbsd/phone-zedbsd.c with
# its socket in a new folder of the build and a fake bluetoothd, with the host's compiler under ASan and UBSan, and runs it.
# The last line is "phone-backend-host-test: PASS" or "... FAIL".
# usage: plan/ws197/tests/phone-backend-host-test.sh   (from the repository's top; OUT= to choose the build folder)
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -eu
OUT=${OUT:-build/ws197-phone-backend-host}
mkdir -p "$OUT"
run=$(mktemp -d "$OUT/run.XXXXXX")
flags="-std=gnu11 -D_GNU_SOURCE -Wall -Wextra -Werror -Wno-format-truncation -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all -Iinclude -I."
cc $flags -o "$OUT/phone-backend-host-test" plan/ws197/tests/phone-backend-host-test.c userland/desktop/libmms/mms.c
timeout 120 "$OUT/phone-backend-host-test" "$run"
