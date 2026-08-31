#!/bin/sh
# WS004-p019 production CDC ECM driver gate.
set -eu

repo=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)
temporary=${TMPDIR:-$repo/build/q049-tmp}/usb-cdc-ecm-driver
ordinary=$temporary/ordinary
sanitized=$temporary/sanitized

mkdir -p "$temporary"

cc -std=c11 -O2 -Wall -Wextra -Werror -pthread -I"$repo/include" \
	-I"$repo/include/uapi" -I"$repo/src" -I"$repo" \
	"$repo/plan/ws004-hardware/tests/usb-cdc-ecm-driver-test.c" \
	-o "$ordinary"
"$ordinary"

cc -std=c11 -O1 -g -Wall -Wextra -Werror -pthread \
	-fsanitize=address,undefined -fno-omit-frame-pointer \
	-I"$repo/include" -I"$repo/include/uapi" -I"$repo/src" -I"$repo" \
	"$repo/plan/ws004-hardware/tests/usb-cdc-ecm-driver-test.c" \
	-o "$sanitized"
ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1 "$sanitized"

cc -std=c11 -O0 -Wall -Wextra -Werror -fanalyzer -I"$repo/include" \
	-I"$repo/include/uapi" -I"$repo/src" -I"$repo" -c \
	"$repo/src/drivers/usb-cdc-ecm.c" -o "$temporary/analyzer.o"

echo 'USB CDC ECM production driver gate: PASS'
