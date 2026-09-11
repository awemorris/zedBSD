#!/bin/sh
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -eu

test_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repo_root=$(CDPATH= cd -- "$test_dir/../../.." && pwd)
build_dir=$(mktemp -d "${TMPDIR:-/tmp}/zedbsd-wlan-l2.XXXXXX")
trap 'rm -rf "$build_dir"' EXIT HUP INT TERM

cc=${CC:-cc}
warnings='-std=c11 -Wall -Wextra -Werror'
source="$repo_root/src/kern/net/wlan-l2.c"
fixture="$test_dir/wlan-l2-test.c"

$cc $warnings -O2 "$source" "$fixture" -o "$build_dir/wlan-l2"
"$build_dir/wlan-l2"
$cc $warnings -O1 -g -fno-omit-frame-pointer -fsanitize=address,undefined \
	"$source" "$fixture" -o "$build_dir/wlan-l2-sanitize"
ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1 \
	"$build_dir/wlan-l2-sanitize"
$cc $warnings -O0 -fanalyzer "$source" "$fixture" \
	-o "$build_dir/wlan-l2-analyzer"
"$build_dir/wlan-l2-analyzer"

echo 'wlan l2: ordinary, sanitizer, analyzer PASS'
