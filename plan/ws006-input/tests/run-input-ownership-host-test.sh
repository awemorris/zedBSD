#!/bin/sh
# WS006 IN-T30 focused source ownership/subscriber runner.
set -eu

repo=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)
temporary=$(mktemp -d "${TMPDIR:-/tmp}/ws006-input-ownership.XXXXXX")
trap 'rm -rf -- "$temporary"' EXIT HUP INT TERM

common="-std=c11 -O2 -D_POSIX_C_SOURCE=200809L -pthread -I$repo/include -I$repo/include/uapi -I$repo/src -Wall -Wextra -Werror"
sources="$repo/plan/ws006-input/tests/input-ownership-test.c $repo/src/drivers/input-subscriber.c $repo/src/drivers/input-keymap.c $repo/src/drivers/input-capability.c $repo/src/drivers/input-queue.c"

cc $common $sources -o "$temporary/ordinary"
"$temporary/ordinary"

cc $common -g -fno-omit-frame-pointer -fsanitize=address,undefined \
	$sources -o "$temporary/sanitize"
ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1 \
	"$temporary/sanitize"

device_sources="$repo/plan/ws006-input/tests/input-device-ownership-test.c $repo/src/drivers/input-device.c $repo/src/drivers/input-subscriber.c $repo/src/drivers/input-keymap.c $repo/src/drivers/input-capability.c $repo/src/drivers/input-queue.c"
device_common="$common -DZEDBSD_USER_ABI_LP64 -idirafter $repo/libc/include -include $repo/libc/include/sys/ioctl.h"
cc $device_common $device_sources -o "$temporary/input-device"
"$temporary/input-device"
cc $device_common -g -fno-omit-frame-pointer -fsanitize=address,undefined \
	$device_sources -o "$temporary/input-device-sanitize"
ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1 \
	"$temporary/input-device-sanitize"

console_sources="$repo/plan/ws006-input/tests/console-input-ownership-test.c $repo/src/drivers/fs/console.c $repo/src/drivers/input-keymap.c"
console_common="$device_common -DZEDBSD_INPUT_OWNERSHIP_TEST -ffunction-sections -fdata-sections -Wl,--gc-sections"
cc $console_common $console_sources -o "$temporary/console-input"
"$temporary/console-input"
cc $console_common -g -fno-omit-frame-pointer -fsanitize=address,undefined \
	$console_sources -o "$temporary/console-input-sanitize"
ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1 \
	"$temporary/console-input-sanitize"

resync_test="$repo/plan/ws006-input/tests/hal-input-resync-test.c"
hal_common="$common -I$repo -DZEDBSD_INPUT_OWNERSHIP_TEST -ffunction-sections -fdata-sections -Wl,--gc-sections"
cc $hal_common -DTEST_PCAT "$resync_test" \
	"$repo/src/hal/amd64/bsp-pcat/cons.c" -o "$temporary/amd64-pcat-resync"
"$temporary/amd64-pcat-resync"
cc $hal_common -DTEST_PCAT "$resync_test" \
	"$repo/src/hal/i386/bsp-pcat/cons.c" -o "$temporary/i386-pcat-resync"
"$temporary/i386-pcat-resync"
cc $hal_common -std=gnu11 -DTEST_PC98 "$resync_test" \
	"$repo/src/hal/i386/bsp-pc98/cons.c" -o "$temporary/pc98-resync"
"$temporary/pc98-resync"
cc $hal_common -DTEST_X68K "$resync_test" \
	"$repo/src/hal/m68k/bsp-x68k/keyboard.c" \
	"$repo/src/hal/m68k/bsp-x68k/keyboard-map.c" -o "$temporary/x68k-resync"
"$temporary/x68k-resync"

cc $common "$repo/plan/ws006-input/tests/x68k-keyboard-ownership-test.c" \
	"$repo/src/hal/m68k/bsp-x68k/keyboard-map.c" \
	"$repo/src/drivers/input-keymap.c" \
	-o "$temporary/x68k"
"$temporary/x68k"
cc $common -g -fno-omit-frame-pointer -fsanitize=address,undefined \
	"$repo/plan/ws006-input/tests/x68k-keyboard-ownership-test.c" \
	"$repo/src/hal/m68k/bsp-x68k/keyboard-map.c" \
	"$repo/src/drivers/input-keymap.c" \
	-o "$temporary/x68k-sanitize"
ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1 \
	"$temporary/x68k-sanitize"

pc98_sources="$repo/plan/ws006-input/tests/pc98-keyboard-ownership-test.c $repo/src/hal/i386/bsp-pc98/cons.c $repo/src/drivers/input-keymap.c"
pc98_common="$common -std=gnu11 -ffunction-sections -fdata-sections -Wl,--gc-sections"
cc $pc98_common $pc98_sources -o "$temporary/pc98"
"$temporary/pc98"
cc $pc98_common -g -fno-omit-frame-pointer -fsanitize=address,undefined \
	$pc98_sources -o "$temporary/pc98-sanitize"
ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1 \
	"$temporary/pc98-sanitize"

echo "IN-T30/IN-T31/IN-T32/IN-T33/IN-T34 ordinary+ASan+UBSan; IN-T35: PASS"
