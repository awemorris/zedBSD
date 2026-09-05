#!/bin/sh
# WS004 HW-T20 production partition publication/naming runner.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -eu

repo=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)
temporary=$(mktemp -d "${TMPDIR:-/tmp}/zedbsd-partition-publish.XXXXXX")
trap 'rm -rf "$temporary"' EXIT HUP INT TERM

compiler=${HOSTCC:-cc}
fixture=$repo/plan/ws004-hardware/tests/partition-publication-test.c
source=$repo/src/kern/partition.c
includes="-ffunction-sections -fdata-sections -Wl,--gc-sections -I$repo/include -I$repo/include/uapi -I$repo/src -I$repo/libc/include"

# shellcheck disable=SC2086
$compiler -std=c11 -O2 -Wall -Wextra -Werror $includes \
	"$fixture" "$source" -o "$temporary/partition-publication-test"
"$temporary/partition-publication-test"

# shellcheck disable=SC2086
if $compiler -std=c11 -O1 -g -Wall -Wextra -Werror \
	-fsanitize=address,undefined -fno-omit-frame-pointer $includes \
	"$fixture" "$source" \
	-o "$temporary/partition-publication-test-sanitized" 2>/dev/null; then
	ASAN_OPTIONS=detect_leaks=0:halt_on_error=1 \
	UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
		"$temporary/partition-publication-test-sanitized"
	echo 'HW-T20 partition publication sanitizer: PASS'
else
	echo 'HW-T20 partition publication sanitizer: SKIP (compiler unavailable)'
fi

analyzer_pass=yes
for input in "$fixture" "$source"; do
	object=$temporary/$(basename "$input" .c)-analyzer.o
	# shellcheck disable=SC2086
	if ! $compiler -std=c11 -O0 -Wall -Wextra -Werror -fanalyzer \
		$includes -c "$input" -o "$object" 2>/dev/null; then
		analyzer_pass=no
		break
	fi
done
if test "$analyzer_pass" = yes; then
	echo 'HW-T20 partition publication analyzer: PASS'
else
	echo 'HW-T20 partition publication analyzer: SKIP (analyzer unavailable)'
fi
