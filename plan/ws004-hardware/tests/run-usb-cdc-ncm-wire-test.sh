#!/bin/sh
set -eu

repo=$(CDPATH= cd -- "$(dirname "$0")/../../.." && pwd)
temporary=$(mktemp -d "${TMPDIR:-/tmp}/zedbsd-ncm-wire.XXXXXX")
trap 'rm -rf "$temporary"' EXIT HUP INT TERM

compiler=${HOSTCC:-cc}
production=$repo/src/drivers/usb-cdc-ncm.c
fixture=$repo/plan/ws004-hardware/tests/usb-cdc-ncm-wire-test.c
ordinary=$temporary/usb-cdc-ncm-wire-test
sanitized=$temporary/usb-cdc-ncm-wire-test-sanitized

"$compiler" -std=c11 -O2 -Wall -Wextra -Werror \
	-I"$repo/include" "$production" "$fixture" -o "$ordinary"
"$ordinary"

if "$compiler" -std=c11 -O1 -g -Wall -Wextra -Werror \
	-fno-omit-frame-pointer -fsanitize=address,undefined \
	-I"$repo/include" "$production" "$fixture" -o "$sanitized" \
	2>/dev/null; then
	# LeakSanitizer cannot run in the ptraced Codex test environment; this
	# allocation-free codec is covered by ASan and UBSan here.
	ASAN_OPTIONS=detect_leaks=0:halt_on_error=1 \
	UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
		"$sanitized"
	echo "USB CDC NCM sanitizer fixture: PASS"
else
	echo "USB CDC NCM sanitizer fixture: SKIP (compiler unavailable)"
fi
