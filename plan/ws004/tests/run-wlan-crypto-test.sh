#!/bin/sh
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -eu

test_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repo_root=$(CDPATH= cd -- "$test_dir/../../.." && pwd)
build_dir=$(mktemp -d "${TMPDIR:-/tmp}/zedbsd-wlan-crypto.XXXXXX")
trap 'rm -rf "$build_dir"' EXIT HUP INT TERM

cc=${CC:-cc}
warnings="-std=c11 -Wall -Wextra -Werror"
includes="-I$repo_root/src/kern/net"
source="$repo_root/src/kern/net/wlan-crypto.c"
fixture="$test_dir/wlan-crypto-test.c"

$cc $warnings -O2 $includes "$source" "$fixture" \
	-o "$build_dir/wlan-crypto"
"$build_dir/wlan-crypto"

$cc $warnings -O1 -g -fno-omit-frame-pointer \
	-fsanitize=address,undefined $includes "$source" "$fixture" \
	-o "$build_dir/wlan-crypto-sanitize"
ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 \
	"$build_dir/wlan-crypto-sanitize"

$cc $warnings -O0 -fanalyzer $includes "$source" "$fixture" \
	-o "$build_dir/wlan-crypto-analyzer"
"$build_dir/wlan-crypto-analyzer"

abi_includes="-I$repo_root/src/kern/net -I$repo_root/libc/include"
$cc $warnings -m64 -nostdinc $abi_includes -fsyntax-only "$source"
$cc $warnings -m32 -nostdinc $abi_includes -fsyntax-only "$source"

echo "wlan crypto: ordinary, sanitizer, analyzer, amd64/i386 syntax PASS"
