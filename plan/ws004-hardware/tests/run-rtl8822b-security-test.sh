#!/bin/sh
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -eu

test_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repo_root=$(CDPATH= cd -- "$test_dir/../../.." && pwd)
build_dir=$(mktemp -d "${TMPDIR:-/tmp}/zedbsd-rtl8822b-sec.XXXXXX")
trap 'rm -rf "$build_dir"' EXIT HUP INT TERM

cc=${CC:-cc}
warnings='-std=c11 -Wall -Wextra -Werror'
source="$repo_root/src/drivers/rtl8822b-security.c"
fixture="$test_dir/rtl8822b-security-test.c"

$cc $warnings -O2 "$source" "$fixture" -o "$build_dir/ordinary"
"$build_dir/ordinary"
$cc $warnings -O1 -g -fno-omit-frame-pointer -fsanitize=address,undefined \
	"$source" "$fixture" -o "$build_dir/sanitize"
ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1 \
	"$build_dir/sanitize"
$cc $warnings -O0 -fanalyzer "$source" "$fixture" \
	-o "$build_dir/analyzer"
"$build_dir/analyzer"

echo 'rtl8822b security: ordinary, sanitizer, analyzer PASS'
