#!/bin/sh
# WS013 p003 checked UEFI framebuffer mapping test runner.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -eu

repo=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)
temporary=$(mktemp -d "${TMPDIR:-/tmp}/zedbsd-uefi-fb.XXXXXX")
trap 'rm -rf "$temporary"' EXIT HUP INT TERM

compiler=${HOSTCC:-cc}
fixture=$repo/plan/ws013-containers/tests/uefi-framebuffer-test.c
source=$repo/bootloader/uefi/framebuffer.c
common="-std=c11 -Wall -Wextra -Werror -I$repo"

# shellcheck disable=SC2086
$compiler $common -O2 "$fixture" "$source" -o "$temporary/test"
"$temporary/test"

# shellcheck disable=SC2086
if $compiler $common -O1 -g -fsanitize=address,undefined \
	-fno-omit-frame-pointer "$fixture" "$source" \
	-o "$temporary/test-sanitized" 2>/dev/null; then
	ASAN_OPTIONS=detect_leaks=0:halt_on_error=1 \
	UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
		"$temporary/test-sanitized"
	echo 'WS013 p003 UEFI framebuffer sanitizer: PASS'
else
	echo 'WS013 p003 UEFI framebuffer sanitizer: SKIP (compiler unavailable)'
fi

# shellcheck disable=SC2086
if $compiler $common -O0 -fanalyzer -c "$source" \
	-o "$temporary/framebuffer-analyzer.o" 2>/dev/null; then
	echo 'WS013 p003 UEFI framebuffer analyzer: PASS'
else
	echo 'WS013 p003 UEFI framebuffer analyzer: SKIP (analyzer unavailable)'
fi
