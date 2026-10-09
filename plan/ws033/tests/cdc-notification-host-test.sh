#!/bin/sh
# BUG-222: the host test of the USB CDC notification reader (src/drivers/usb/usb-cdc-notification.c) that the ECM and
# NCM drivers share for the link's connection and speed: builds it with plan/ws033/tests/cdc-notification-host-test.c
# under ASan and UBSan with the host's compiler and runs it.  The last line is "cdc-notification-host-test: PASS" or
# "... FAIL".  Nothing is removed (OUT= chooses the build folder, default build/bug222-cdc-notification).
#   plan/ws033/tests/cdc-notification-host-test.sh
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
cd "$(dirname -- "$0")/../../.."
OUT=${OUT:-build/bug222-cdc-notification}
mkdir -p "$OUT"
if ! cc -std=c99 -Wall -Wextra -Werror -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all -Iinclude \
	-o "$OUT/cdc-notification-host-test" plan/ws033/tests/cdc-notification-host-test.c \
	src/drivers/usb/usb-cdc-notification.c; then
	echo "cdc-notification-host-test: FAIL (build)"
	exit 1
fi
if timeout 60 "$OUT/cdc-notification-host-test"; then
	echo "cdc-notification-host-test: PASS"
else
	echo "cdc-notification-host-test: FAIL"
	exit 1
fi
