#!/bin/sh
# ws004-p031 amd64 PC/AT console output serialization host gate.
set -eu

repo=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)
temporary=$(mktemp -d "${TMPDIR:-/tmp}/ws004-console-output.XXXXXX")
trap 'rm -rf -- "$temporary"' EXIT HUP INT TERM

common="-std=c11 -O2 -D_POSIX_C_SOURCE=200809L -DHAL_ARCH_AMD64 \
-DZEDBSD_CONSOLE_OUTPUT_TEST -pthread -ffunction-sections -fdata-sections \
-Wl,--gc-sections -I$repo/include -I$repo/include/uapi -I$repo/src -I$repo \
-Wall -Wextra -Werror"
sources="$repo/plan/ws004-hardware/tests/amd64-console-output-host-test.c \
$repo/src/hal/amd64/bsp-pcat/cons.c"

cc $common $sources -o "$temporary/ordinary"
"$temporary/ordinary"

cc $common -g -fno-omit-frame-pointer -fsanitize=address,undefined \
	$sources -o "$temporary/sanitize"
ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1 \
	"$temporary/sanitize"

echo "HW-T27 console ordinary+ASan+UBSan: PASS"
