#!/bin/sh
# WS018 KA-T070/KA-T071 independent input/HID ownership host runner.
set -eu

repo=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)
temporary=$(mktemp -d "${TMPDIR:-/tmp}/ws018-input-hid.XXXXXX")
trap 'rm -rf -- "$temporary"' EXIT HUP INT TERM

common_flags="-std=c11 -O2 -D_POSIX_C_SOURCE=200809L -I$repo/include -I$repo/include/uapi -I$repo/src -I$repo -Wall -Wextra -Werror"
sanitize_flags="-g -fno-omit-frame-pointer -fsanitize=address,undefined"
tests="$repo/plan/ws018-kernel-architecture/tests"

cc $common_flags "$tests/ps2-mouse-host-test.c" -o "$temporary/ps2"
"$temporary/ps2"
cc $common_flags "$tests/pc98-busmouse-host-test.c" -o "$temporary/pc98"
"$temporary/pc98"

cc $common_flags $sanitize_flags "$tests/ps2-mouse-host-test.c" \
	-o "$temporary/ps2-sanitize"
ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1 \
	"$temporary/ps2-sanitize"
cc $common_flags $sanitize_flags "$tests/pc98-busmouse-host-test.c" \
	-o "$temporary/pc98-sanitize"
ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1 \
	"$temporary/pc98-sanitize"

test ! -e "$repo/src/kern/mouse-device.c"
test ! -e "$repo/include/kern/mouse-device.h"
test ! -e "$repo/include/uapi/zedbsd/mouse.h"
test ! -e "$repo/src/drivers/pcat-ps2-mouse.c"
test ! -e "$repo/src/drivers/pc98-busmouse.c"
test -e "$repo/src/drivers/input-device.c"
test -e "$repo/src/drivers/input-queue.c"
test -e "$repo/src/drivers/input-capability.c"
test -e "$repo/src/drivers/input-keymap.c"
test -e "$repo/src/drivers/fs/console.c"
test -e "$repo/src/drivers/hid/ps2-mouse.c"
test -e "$repo/src/drivers/hid/pc98-busmouse.c"
test -e "$repo/include/drivers/hid/ps2-mouse.h"
test -e "$repo/include/drivers/hid/pc98-busmouse.h"

if rg -n '/dev/mouse|mouse_device_(register|set_backend)|mouse_input_report|zedbsd/mouse' \
	"$repo/src" "$repo/include" "$repo/userland" "$repo/platform" \
	"$repo/Makefile" --glob '!userland/noct/**' \
	--glob '!userland/packages/lang/noct/**'; then
	echo "retired generic mouse implementation remains" >&2
	exit 1
fi

if rg -n 'src/kern/(console-device|input-(device|queue|capability|keymap)|mouse-device)\.c|src/drivers/(pcat-ps2-mouse|pc98-busmouse)\.c' \
	"$repo/platform" "$repo/Makefile" "$repo/plan/ws006-input/tests"; then
	echo "active build or input fixture still names a retired source path" >&2
	exit 1
fi

test "$(rg -l 'input_device_register\(' "$repo/src/drivers/hid"/*.c | wc -l)" -eq 2
test "$(rg -l '\.open = mouse_input_open' "$repo/src/drivers/hid"/*.c | wc -l)" -eq 2

awk '
	/input_core_init\(\)/ { core = NR }
	/console_device_register\(\)/ { console = NR }
	/kern_platform_input_init\(\)/ { platform = NR }
	END { exit !(core && console && platform && core < console && console < platform) }
' "$repo/src/kern/vfs.c"

echo "KA-T070/KA-T071 independent input/HID ownership: PASS"
