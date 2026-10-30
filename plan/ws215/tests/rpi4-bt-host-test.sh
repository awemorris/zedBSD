#!/bin/sh
# The host test of the Raspberry Pi 4 onboard Bluetooth's pure parts (ws215-p003): builds the H4 reassembly
# (src/drivers/generic/bt-hci-proto.c) and the line's hunt, divisor and reset history
# (src/drivers/platform/rpi-common/rpi-bt-link.c) with the host's compiler under ASan and UBSan and runs them; then checks
# the config.txt the build writes (platform/arm64/tools/make-rpi4-config.py) for the four combinations of the
# Bluetooth driver and the serial console.
# usage: plan/ws215/tests/rpi4-bt-host-test.sh   (from the repository's top; OUT= to choose the build folder)
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -eu
OUT=${OUT:-build/ws215-rpi4-bt-host}
mkdir -p "$OUT"
cc -std=c11 -Wall -Wextra -Werror -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all -Iinclude -Isrc -I. \
	-o "$OUT/rpi4-bt-host-test" plan/ws215/tests/rpi4-bt-host-test.c src/drivers/generic/bt-hci-proto.c \
	src/drivers/platform/rpi-common/rpi-bt-link.c
timeout 60 "$OUT/rpi4-bt-host-test"

# config.txt: byte for byte the source unless the driver is on and the console off; then exactly the three serial
# lines and the two comment lines above uart_2ndstage are gone.
SOURCE=platform/arm64/config.txt
for bluetooth in y n; do
	for serial in y n; do
		python3 platform/arm64/tools/make-rpi4-config.py --source "$SOURCE" --bluetooth "$bluetooth" \
			--serial "$serial" --output "$OUT/config-$bluetooth-$serial.txt"
	done
done
for same in y-y n-y n-n; do
	cmp -s "$SOURCE" "$OUT/config-$same.txt" || { echo "config.txt: bluetooth/serial $same differs from the source"; exit 1; }
done
removed=$(diff "$SOURCE" "$OUT/config-y-n.txt" | grep -c '^<' || true)
added=$(diff "$SOURCE" "$OUT/config-y-n.txt" | grep -c '^>' || true)
[ "$removed" = 5 ] && [ "$added" = 0 ] || { echo "config.txt: y-n removed $removed and added $added lines"; exit 1; }
for line in enable_uart=1 uart_2ndstage=1 dtoverlay=disable-bt; do
	! grep -qx "$line" "$OUT/config-y-n.txt" || { echo "config.txt: $line is left"; exit 1; }
done
grep -qx 'init_uart_clock=48000000' "$OUT/config-y-n.txt" || { echo "config.txt: init_uart_clock is gone"; exit 1; }
echo "rpi4-bt config.txt: all checks passed"
