#!/bin/sh
# WS006 dynamic cdev/devfs generation-lifetime runner.
set -eu

repo=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)
temporary=$(mktemp -d "${TMPDIR:-/tmp}/ws006-dynamic-cdev.XXXXXX")
trap 'rm -rf -- "$temporary"' EXIT HUP INT TERM

common="-std=c11 -O2 -D_POSIX_C_SOURCE=200809L -pthread -DZEDBSD_USER_ABI_LP64 -DZEDBSD_DEVFS_HOST_TEST -I$repo/include -I$repo/include/uapi -I$repo/src -idirafter $repo/libc/include -Wall -Wextra -Werror"
sources="$repo/plan/ws006-input/tests/dynamic-cdev-devfs-test.c $repo/src/kern/cdev.c $repo/src/kern/devfs.c"

cc $common $sources -o "$temporary/ordinary"
"$temporary/ordinary"

cc $common -g -fno-omit-frame-pointer -fsanitize=address,undefined \
	$sources -o "$temporary/sanitize"
ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 \
	"$temporary/sanitize"

cc $common -fanalyzer -c "$repo/src/kern/cdev.c" \
	-o "$temporary/cdev-analyzer.o"
cc $common -fanalyzer -c "$repo/src/kern/devfs.c" \
	-o "$temporary/devfs-analyzer.o"
cc $common -fanalyzer -c \
	"$repo/plan/ws006-input/tests/dynamic-cdev-devfs-test.c" \
	-o "$temporary/dynamic-cdev-devfs-analyzer.o"
input_analyzer="$common -include $repo/libc/include/sys/ioctl.h"
cc $input_analyzer -fanalyzer -c "$repo/src/drivers/input-device.c" \
	-o "$temporary/input-device-analyzer.o"
cc $input_analyzer -fanalyzer -c \
	"$repo/plan/ws006-input/tests/input-device-ownership-test.c" \
	-o "$temporary/input-device-lifecycle-analyzer.o"

echo "WS006 dynamic cdev/devfs ordinary+ASan+UBSan+analyzer: PASS"
