#!/bin/sh
# Exercises the production formatter reservation and shared VM admission paths.
set -eu

repo=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)
out=$(mktemp -d "${TMPDIR:-/tmp}/zedbsd-format-reservation.XXXXXXXX")
trap 'rm -rf "$out"' EXIT HUP INT TERM

for mode in normal sanitize; do
	flags=''
	if [ "$mode" = sanitize ]; then
		flags='-fsanitize=address,undefined -fno-omit-frame-pointer --param=asan-globals=0'
	fi
	${CC:-cc} -std=c11 -O0 -g -DZEDBSD_USER_ABI_LP64 \
		-I"$repo/include" -I"$repo/include/uapi" -I"$repo" \
		-Wall -Wextra -Werror -ffunction-sections -fdata-sections \
		$flags \
		"$repo/src/kern/file.c" \
		"$repo/src/kern/backing-claim.c" \
		"$repo/src/kern/vm-object.c" \
		"$repo/src/kern/vmspace.c" \
		"$repo/plan/ws019-installation/tests/format-reservation-test.c" \
		-Wl,--gc-sections -pthread "$repo/src/kern/io-stats.c" -o "$out/$mode"
	timeout 30 "$out/$mode"
done
