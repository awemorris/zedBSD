#!/bin/sh
set -eu

repo=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)
temporary=$(mktemp -d "${TMPDIR:-/tmp}/zedbsd-rtl8822bu.XXXXXX")
trap 'rm -rf "$temporary"' EXIT HUP INT TERM

cc=${CC:-cc}
common="-std=c11 -Wall -Wextra -Werror -pthread -I$repo/plan/ws004-hardware/tests/host-include -I$repo/include -I$repo/include/uapi"
source=$repo/plan/ws004-hardware/tests/usb-rtl8822bu-driver-test.c

# shellcheck disable=SC2086
$cc $common "$source" -o "$temporary/driver-test"
"$temporary/driver-test"

# shellcheck disable=SC2086
$cc $common -fsanitize=address,undefined -fno-omit-frame-pointer \
	"$source" -o "$temporary/driver-test-sanitize"
ASAN_OPTIONS=detect_leaks=1 "$temporary/driver-test-sanitize"

# shellcheck disable=SC2086
$cc $common -O0 -fanalyzer "$source" -o "$temporary/driver-test-analyzer"
"$temporary/driver-test-analyzer"

echo 'usb rtl8822bu fixture: PASS'
