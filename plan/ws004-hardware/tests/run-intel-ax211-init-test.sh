#!/bin/sh
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -eu

test_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repo_root=$(CDPATH= cd -- "$test_dir/../../.." && pwd)
build_dir=$(mktemp -d "${TMPDIR:-/tmp}/zedbsd-intel-ax211-init.XXXXXX")
trap 'rm -rf "$build_dir"' EXIT HUP INT TERM

cc=${CC:-cc}
warnings="-std=c11 -Wall -Wextra -Werror"
sources="$repo_root/src/drivers/intel-ax211-protocol.c"
sources="$sources $repo_root/src/drivers/intel-ax211-init.c"
fixture="$test_dir/intel-ax211-init-test.c"
abi_includes="-I$repo_root/libc/include -I$repo_root/include/uapi"
abi_includes="$abi_includes -I$repo_root/include -I$repo_root/src"
firmware_cache="$repo_root/build/sources/firmware/intelax211"
firmware_cache="$firmware_cache/dc85ccedc9c973682fbcf4d628ca61174bcc3120"
firmware="$firmware_cache/iwlwifi-so-a0-gf-a0-89.ucode"

# shellcheck disable=SC2086
$cc $warnings -O2 $sources "$fixture" -o "$build_dir/ordinary"
"$build_dir/ordinary"
if [ -r "$firmware" ]; then
	"$build_dir/ordinary" "$firmware"
fi

# shellcheck disable=SC2086
$cc $warnings -O1 -g -fno-omit-frame-pointer -fsanitize=address,undefined \
	$sources "$fixture" -o "$build_dir/sanitize"
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 \
	"$build_dir/sanitize"
if [ -r "$firmware" ]; then
	ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 \
		UBSAN_OPTIONS=halt_on_error=1 "$build_dir/sanitize" "$firmware"
fi

# shellcheck disable=SC2086
$cc $warnings -O0 -fanalyzer $sources "$fixture" \
	-o "$build_dir/analyzer"
"$build_dir/analyzer"

# AX211 is amd64-only, but the private wire codec remains ILP32-clean.
# shellcheck disable=SC2086
$cc $warnings -m64 -nostdinc $abi_includes -fsyntax-only $sources
# shellcheck disable=SC2086
$cc $warnings -m32 -nostdinc $abi_includes -fsyntax-only $sources

echo "intel ax211 init: ordinary, ASan/UBSan, analyzer, amd64/i386 syntax PASS"
