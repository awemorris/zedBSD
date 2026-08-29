#!/bin/sh
# WS004 HW-T20 production strict-GPT host regression runner.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -eu

repo=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)
temporary=$(mktemp -d "${TMPDIR:-/tmp}/zedbsd-gpt.XXXXXX")
trap 'rm -rf "$temporary"' EXIT HUP INT TERM

compiler=${HOSTCC:-cc}
fixture=$repo/plan/ws004-hardware/tests/gpt-host-test.c
sources="$repo/src/drivers/disklabel/mbr.c
	$repo/src/drivers/disklabel/gpt.c
	$repo/src/drivers/disklabel/pcat-auto.c"
common="-std=c11 -O2 -Wall -Wextra -Werror -I$repo/include -I$repo/include/uapi -I$repo/src -I$repo/libc/include"

# shellcheck disable=SC2086
$compiler $common "$fixture" $sources -o "$temporary/gpt-host-test"
"$temporary/gpt-host-test"

# shellcheck disable=SC2086
if $compiler -std=c11 -O1 -g -Wall -Wextra -Werror \
	-fsanitize=address,undefined -fno-omit-frame-pointer \
	-I"$repo/include" -I"$repo/include/uapi" -I"$repo/src" \
	-I"$repo/libc/include" "$fixture" $sources \
	-o "$temporary/gpt-host-test-sanitized" 2>/dev/null; then
	ASAN_OPTIONS=detect_leaks=0:halt_on_error=1 \
	UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
		"$temporary/gpt-host-test-sanitized"
	echo 'HW-T20 strict GPT sanitizer: PASS'
else
	echo 'HW-T20 strict GPT sanitizer: SKIP (compiler unavailable)'
fi

analyzer_pass=yes
for source in "$fixture" \
	"$repo/src/drivers/disklabel/mbr.c" \
	"$repo/src/drivers/disklabel/gpt.c" \
	"$repo/src/drivers/disklabel/pcat-auto.c"; do
	object=$temporary/$(basename "$source" .c)-analyzer.o
	if ! $compiler -std=c11 -O0 -Wall -Wextra -Werror -fanalyzer \
		-I"$repo/include" -I"$repo/include/uapi" -I"$repo/src" \
		-I"$repo/libc/include" -c "$source" -o "$object" 2>/dev/null; then
		analyzer_pass=no
		break
	fi
done
if test "$analyzer_pass" = yes; then
	echo 'HW-T20 strict GPT analyzer: PASS'
else
	echo 'HW-T20 strict GPT analyzer: SKIP (analyzer unavailable)'
fi
