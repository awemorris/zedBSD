#!/bin/sh
# The host test of bluetoothd (ws143-p003): builds userland/base/bluetoothd/hci.c, intel.c and session.c with the host's
# compiler (under ASan and UBSan) and runs plan/ws143/tests/bt-daemon-host-test.c (the parsers, the Intel load's plan, a
# scripted controller's sessions, a fixed-seed fuzz).  The synthetic firmware files go in a new folder of the build.
# usage: plan/ws143/tests/bt-daemon-host-test.sh   (from the repository's top; OUT= to choose the build folder)
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -eu
OUT=${OUT:-build/ws143-bt-daemon-host}
mkdir -p "$OUT"
firmware=$(mktemp -d "$OUT/firmware.XXXXXX")
cc -std=gnu11 -D_GNU_SOURCE -Wall -Wextra -Werror -Wdeclaration-after-statement -O1 -g -fsanitize=address,undefined \
	-fno-sanitize-recover=all -Iinclude -I. -o "$OUT/bt-daemon-host-test" plan/ws143/tests/bt-daemon-host-test.c \
	userland/base/bluetoothd/hci.c userland/base/bluetoothd/intel.c userland/base/bluetoothd/session.c -lpthread
timeout 120 "$OUT/bt-daemon-host-test" "$firmware"
