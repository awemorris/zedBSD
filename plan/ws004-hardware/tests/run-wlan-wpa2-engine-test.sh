#!/bin/sh
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -eu

test_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repo_root=$(CDPATH= cd -- "$test_dir/../../.." && pwd)
build_dir=$(mktemp -d "${TMPDIR:-/tmp}/zedbsd-wlan-wpa2-engine.XXXXXX")
trap 'rm -rf "$build_dir"' EXIT HUP INT TERM

cc=${CC:-cc}
includes="-I$test_dir/host-include -I$repo_root/include/uapi -I$repo_root/include -I$repo_root/src"
abi_includes="-I$repo_root/libc/include -I$repo_root/include/uapi -I$repo_root/include -I$repo_root/src"
warnings="-std=c11 -Wall -Wextra -Werror -DWLAN_WPA2_TESTING"
sources="$repo_root/src/kern/net/wlan-crypto.c $repo_root/src/kern/net/wlan-wpa2-codec.c $repo_root/src/kern/net/wlan-wpa2.c"
fixture="$test_dir/wlan-wpa2-engine-test.c"

$cc $warnings -O2 $includes $sources "$fixture" \
	-o "$build_dir/wlan-wpa2-engine"
"$build_dir/wlan-wpa2-engine"

$cc $warnings -O1 -g -fno-omit-frame-pointer -fsanitize=address,undefined \
	$includes $sources "$fixture" -o "$build_dir/wlan-wpa2-engine-sanitize"
ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1 \
	"$build_dir/wlan-wpa2-engine-sanitize"

$cc $warnings -O0 -fanalyzer $includes $sources "$fixture" \
	-o "$build_dir/wlan-wpa2-engine-analyzer"
"$build_dir/wlan-wpa2-engine-analyzer"

$cc -m64 -nostdinc $abi_includes $warnings -fsyntax-only $sources
$cc -m32 -nostdinc $abi_includes $warnings -fsyntax-only $sources

echo "WPA2 engine: async TX, handshake, replay, rollback, sanitizer/analyzer PASS"
