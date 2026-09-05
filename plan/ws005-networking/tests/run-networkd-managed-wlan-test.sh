#!/bin/sh
set -eu

repo=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT HUP INT TERM

cc -std=c11 -DZEDBSD_USER_ABI_LP64 -Wall -Wextra -Werror \
	-I"$repo/include/uapi" -I"$repo/libc/include" -I"$repo" \
	"$repo/plan/ws005-networking/tests/networkd-managed-wlan-test.c" \
	"$repo/userland/base/networkd/managed-wlan.c" \
	-o "$work/networkd-managed-wlan-test"
"$work/networkd-managed-wlan-test"

cc -std=c11 -DZEDBSD_USER_ABI_LP64 -Wall -Wextra -Werror \
	-fsanitize=address,undefined -fno-omit-frame-pointer \
	-I"$repo/include/uapi" -I"$repo/libc/include" -I"$repo" \
	"$repo/plan/ws005-networking/tests/networkd-managed-wlan-test.c" \
	"$repo/userland/base/networkd/managed-wlan.c" \
	-o "$work/networkd-managed-wlan-sanitize"
ASAN_OPTIONS=detect_leaks=1 "$work/networkd-managed-wlan-sanitize"

cc -std=c11 -DZEDBSD_USER_ABI_LP64 -Wall -Wextra -Werror -fanalyzer \
	-I"$repo/include/uapi" -I"$repo/libc/include" -I"$repo" \
	-c "$repo/userland/base/networkd/managed-wlan.c" \
	-o "$work/networkd-managed-wlan-analyzer.o"

cc -std=c11 -DZEDBSD_USER_ABI_LP64 -Wall -Wextra -Werror \
	-ffunction-sections -fdata-sections \
	-I"$repo/include/uapi" -I"$repo/libc/include" -I"$repo" \
	"$repo/plan/ws005-networking/tests/networkd-radio-preparation-test.c" \
	-Wl,--gc-sections -o "$work/networkd-radio-preparation-test"
"$work/networkd-radio-preparation-test"

cc -std=c11 -DZEDBSD_USER_ABI_LP64 -Wall -Wextra -Werror \
	-ffunction-sections -fdata-sections \
	-fsanitize=address,undefined -fno-omit-frame-pointer \
	-I"$repo/include/uapi" -I"$repo/libc/include" -I"$repo" \
	"$repo/plan/ws005-networking/tests/networkd-radio-preparation-test.c" \
	-Wl,--gc-sections -o "$work/networkd-radio-preparation-sanitize"
ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1 \
	"$work/networkd-radio-preparation-sanitize"

cc -std=c11 -DZEDBSD_USER_ABI_LP64 -Wall -Wextra -Werror \
	-ffunction-sections -fdata-sections \
	-I"$repo/include/uapi" -I"$repo/libc/include" -I"$repo" \
	"$repo/plan/ws005-networking/tests/networkd-wifi-selection-test.c" \
	"$repo/userland/base/networkd/managed-wlan.c" \
	-Wl,--gc-sections -o "$work/networkd-wifi-selection-test"
"$work/networkd-wifi-selection-test"

cc -std=c11 -DZEDBSD_USER_ABI_LP64 -Wall -Wextra -Werror \
	-ffunction-sections -fdata-sections \
	-fsanitize=address,undefined -fno-omit-frame-pointer \
	-I"$repo/include/uapi" -I"$repo/libc/include" -I"$repo" \
	"$repo/plan/ws005-networking/tests/networkd-wifi-selection-test.c" \
	"$repo/userland/base/networkd/managed-wlan.c" \
	-Wl,--gc-sections -o "$work/networkd-wifi-selection-sanitize"
ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1 \
	"$work/networkd-wifi-selection-sanitize"
