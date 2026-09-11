#!/bin/sh
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -eu

test_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repo_root=$(CDPATH= cd -- "$test_dir/../../.." && pwd)
build_dir=$(mktemp -d "${TMPDIR:-/tmp}/zedbsd-wlan-ccmp-reference.XXXXXX")
trap 'rm -rf "$build_dir"' EXIT HUP INT TERM

cc=${CC:-cc}
warnings='-std=c11 -Wall -Wextra -Werror'
sources="$repo_root/src/kern/net/wlan-crypto.c \
$repo_root/src/kern/net/wlan-l2.c \
$test_dir/wlan-ccmp-reference-test.c"

$cc $warnings -O2 $sources -o "$build_dir/wlan-ccmp-reference"
"$build_dir/wlan-ccmp-reference"
$cc $warnings -O1 -g -fno-omit-frame-pointer \
	-fsanitize=address,undefined $sources \
	-o "$build_dir/wlan-ccmp-reference-sanitize"
ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1 \
	"$build_dir/wlan-ccmp-reference-sanitize"
$cc $warnings -O0 -fanalyzer $sources \
	-o "$build_dir/wlan-ccmp-reference-analyzer"
"$build_dir/wlan-ccmp-reference-analyzer"

echo 'wlan CCMP reference: RFC 3610, production framing, sanitizer, analyzer PASS'
