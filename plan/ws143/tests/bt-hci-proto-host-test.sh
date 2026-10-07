#!/bin/sh
# The host test of the Bluetooth HCI class's pure part (ws143-p002): builds src/drivers/generic/bt-hci-proto.c with the
# host's compiler (under ASan and UBSan) and runs it.
# usage: plan/ws143/tests/bt-hci-proto-host-test.sh   (from the repository's top; OUT= to choose the build folder)
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -eu
OUT=${OUT:-build/ws143-bt-hci-host}
mkdir -p "$OUT"
cc -std=c11 -Wall -Wextra -Werror -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all -Iinclude -I. \
	-o "$OUT/bt-hci-proto-host-test" plan/ws143/tests/bt-hci-proto-host-test.c src/drivers/generic/bt-hci-proto.c
timeout 60 "$OUT/bt-hci-proto-host-test"
