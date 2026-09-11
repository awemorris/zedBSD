#!/bin/sh
set -eu

repo=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT HUP INT TERM

for variant in ordinary sanitize analyzer; do
	case "$variant" in
	ordinary) options= ;;
	sanitize) options='-fsanitize=address,undefined -fno-omit-frame-pointer' ;;
	analyzer) options=-fanalyzer ;;
	esac
	cc -std=c11 -DKERN_USER_ABI_LP64 -Wall -Wextra -Werror \
		-ffunction-sections -fdata-sections $options \
		-I"$repo/include/uapi" -I"$repo/libc/include" -I"$repo" \
		"$repo/plan/ws005/tests/networkd-retire-stage-test.c" \
		"$repo/userland/base/networkd/managed-wlan.c" \
		-Wl,--gc-sections -o "$work/networkd-retire-stage-$variant"
	ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1 \
		"$work/networkd-retire-stage-$variant"
done

printf '%s\n' 'networkd retirement stages: ordinary, sanitizer, analyzer PASS'
