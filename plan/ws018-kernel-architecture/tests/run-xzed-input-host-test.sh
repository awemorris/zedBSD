#!/bin/sh
# WS018 KA-T060 Xzed evdev consumer host runner.
set -eu

repo=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)
temporary=$(mktemp -d "${TMPDIR:-/tmp}/ws018-xzed-input.XXXXXX")
trap 'rm -rf -- "$temporary"' EXIT HUP INT TERM

common_flags="-std=c11 -D_POSIX_C_SOURCE=200809L -O2 -Wall -Wextra -Werror \
-I$repo/include/uapi -I$repo/include -I$repo"
test_source="$repo/plan/ws018-kernel-architecture/tests/xzed-input-host-test.c"
production_source="$repo/userland/X11/xzed/input.c"

cc $common_flags "$production_source" "$test_source" \
	-o "$temporary/xzed-input-test"
"$temporary/xzed-input-test" "$temporary/plain"

cc $common_flags -O1 -g -fno-omit-frame-pointer \
	-fsanitize=address,undefined "$production_source" "$test_source" \
	-o "$temporary/xzed-input-test-sanitize"
ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1 \
	"$temporary/xzed-input-test-sanitize" "$temporary/sanitize"

if rg -n '/dev/mouse|zedbsd/mouse|ZEDBSD_CONSOLE_(GET|SET)_INPUT_MODE|console_input_event' \
	"$repo/userland/X11/xzed"; then
	echo "KA-T060: legacy Xzed input path remains" >&2
	exit 1
fi
if rg -n '"/dev/input/event[0-9]|EVIOCG(NAME|PHYS|UNIQ|ID|LED)|EVIOCGRAB' \
	"$repo/userland/X11/xzed"; then
	echo "KA-T060: fixed identity or forbidden evdev query remains" >&2
	exit 1
fi

echo "KA-T060 Xzed evdev consumer audit: PASS"
