#!/bin/sh
# The host test of the desktop's Bluetooth backend (ws143-p006): builds plan/ws143/tests/bt-desktop-host-test.c, which
# includes userland/desktop/libkeiland-backend-zedbsd/bluetooth-zedbsd.c with its socket in a new folder of the build and
# runs a fake bluetoothd on a thread, with the host's compiler under ASan and UBSan, and runs it.
# usage: plan/ws143/tests/bt-desktop-host-test.sh   (from the repository's top; OUT= to choose the build folder)
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -eu
OUT=${OUT:-build/ws143-bt-desktop-host}
mkdir -p "$OUT"
run=$(mktemp -d "$OUT/run.XXXXXX")
flags="-std=gnu11 -D_GNU_SOURCE -Wall -Wextra -Werror -Wno-format-truncation -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all -Iinclude -I."
cc $flags -o "$OUT/bt-desktop-host-test" plan/ws143/tests/bt-desktop-host-test.c -lpthread
timeout 120 "$OUT/bt-desktop-host-test" "$run"
