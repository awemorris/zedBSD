#!/bin/sh
# ws004-p043 amd64 hal_printf conversion host gate.
set -eu

repo=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)
temporary=$(mktemp -d "${TMPDIR:-/tmp}/ws004-hal-printf.XXXXXX")
trap 'rm -rf -- "$temporary"' EXIT HUP INT TERM

common="-std=c11 -O2 -D_POSIX_C_SOURCE=200809L -DHAL_ARCH_AMD64 \
-I$repo/include -I$repo/include/uapi -I$repo/src -I$repo \
-Wall -Wextra -Werror"
sources="$repo/plan/ws004-hardware/tests/hal-printf-format-host-test.c \
$repo/src/hal/amd64/lib.c"

# shellcheck disable=SC2086
cc $common $sources -o "$temporary/ordinary"
"$temporary/ordinary"

# shellcheck disable=SC2086
cc $common -g -fno-omit-frame-pointer -fsanitize=address,undefined \
	$sources -o "$temporary/sanitize"
ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1 \
	"$temporary/sanitize"

echo "hal printf ordinary+ASan+UBSan: PASS"
