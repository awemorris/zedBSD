#!/bin/sh
#
# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib
#
# Builds the Raspberry Pi 4 SDIO host test (ws206-p002b, p004) twice and runs it:
# the single-threaded checks under AddressSanitizer and
# UndefinedBehaviorSanitizer, the stop and the threads under
# ThreadSanitizer.  The guest is not started.
#
#   sh plan/ws206/tests/sdio-host-test.sh OUT [DTB]
#
# OUT is a directory of the caller's own, such as build/ws206-p002b/sdio.
# DTB is the Raspberry Pi 4 device tree the firmware ships.

set -eu

REPO=$(cd "$(dirname "$0")/../../.." && pwd)
OUT=${1:?usage: sdio-host-test.sh OUT [DTB]}
DTB=${2:-$REPO/vendor/raspberrypi-firmware/boot/bcm2711-rpi-4-b.dtb}
CC=${CC:-cc}

mkdir -p "$OUT"

SOURCES="$REPO/plan/ws206/tests/sdio-host-test.c
$REPO/src/drivers/platform/rpi-common/rpi-mmc-irq.c
$REPO/src/drivers/platform/rpi-common/rpi-sdio-host.c
$REPO/src/drivers/platform/rpi-common/rpi-sdio.c
$REPO/src/drivers/platform/rpi4/rpi4-wifi.c
$REPO/src/drivers/platform/rpi-common/rpi-gpio.c
$REPO/src/drivers/platform/rpi4/rpi4-board.c
$REPO/src/drivers/generic/fdt.c"

# The test's thread header stands in for the kernel's, which needs the HAL.
INCLUDES="-I$REPO/plan/ws206/tests/stub -I$REPO/include -I$REPO/src -I$REPO"

# The unchanged drivers with the model, under ASan and UBSan.
# shellcheck disable=SC2086
"$CC" -D_POSIX_C_SOURCE=200809L $INCLUDES \
	-std=c11 -O1 -g -Wall -Wextra -Werror \
	-fsanitize=address,undefined -fno-sanitize-recover=undefined -pthread \
	$SOURCES -o "$OUT/sdio-host-test-unit"

# The same under ThreadSanitizer for the threads.
# shellcheck disable=SC2086
"$CC" -D_POSIX_C_SOURCE=200809L $INCLUDES \
	-std=c11 -O1 -g -Wall -Wextra -Werror -fsanitize=thread -pthread \
	$SOURCES -o "$OUT/sdio-host-test-smp"

# Any ThreadSanitizer report fails the run.
TSAN_OPTIONS="halt_on_error=1 exitcode=66 ${TSAN_OPTIONS:-}"
export TSAN_OPTIONS

"$OUT/sdio-host-test-unit" unit "$DTB"
"$OUT/sdio-host-test-smp" stop "$DTB"
"$OUT/sdio-host-test-smp" smp "$DTB"
"$OUT/sdio-host-test-unit" nopackage "$DTB"
"$OUT/sdio-host-test-unit" refused "$DTB"
echo "sdio-host-test: PASS"
