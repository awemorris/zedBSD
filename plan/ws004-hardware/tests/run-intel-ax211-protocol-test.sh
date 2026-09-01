#!/bin/sh
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -eu

test_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repo_root=$(CDPATH= cd -- "$test_dir/../../.." && pwd)
build_dir=$(mktemp -d "${TMPDIR:-/tmp}/zedbsd-intel-ax211-protocol.XXXXXX")
trap 'rm -rf "$build_dir"' EXIT HUP INT TERM

cc=${CC:-cc}
warnings="-std=c11 -Wall -Wextra -Werror"
source="$repo_root/src/drivers/intel-ax211-protocol.c"
fixture="$test_dir/intel-ax211-protocol-test.c"
abi_includes="-I$repo_root/libc/include -I$repo_root/include/uapi"
abi_includes="$abi_includes -I$repo_root/include -I$repo_root/src"
firmware_cache="$repo_root/build/sources/firmware/intelax211"
firmware_cache="$firmware_cache/dc85ccedc9c973682fbcf4d628ca61174bcc3120"
firmware="$firmware_cache/iwlwifi-so-a0-gf-a0-89.ucode"

# Ordinary native gate, including the exact cached API89 table when present.
# shellcheck disable=SC2086
$cc $warnings -O2 "$source" "$fixture" \
	-o "$build_dir/intel-ax211-protocol"
"$build_dir/intel-ax211-protocol"
if [ -r "$firmware" ]; then
	"$build_dir/intel-ax211-protocol" "$firmware"
fi

# Memory-safety and undefined-behaviour gate.
# shellcheck disable=SC2086
$cc $warnings -O1 -g -fno-omit-frame-pointer \
	-fsanitize=address,undefined "$source" "$fixture" \
	-o "$build_dir/intel-ax211-protocol-sanitize"
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 \
	UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
	"$build_dir/intel-ax211-protocol-sanitize"
if [ -r "$firmware" ]; then
	ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 \
		UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
		"$build_dir/intel-ax211-protocol-sanitize" "$firmware"
fi

# Static lifetime and bounds analysis gate.
# shellcheck disable=SC2086
$cc $warnings -O0 -fanalyzer "$source" "$fixture" \
	-o "$build_dir/intel-ax211-protocol-analyzer"
"$build_dir/intel-ax211-protocol-analyzer"

# AX211 hardware is amd64, while every private wire layout remains ILP32-clean.
# shellcheck disable=SC2086
$cc $warnings -m64 -nostdinc $abi_includes -fsyntax-only "$source"
# shellcheck disable=SC2086
$cc $warnings -m32 -nostdinc $abi_includes -fsyntax-only "$source"

echo "intel ax211 protocol: ordinary, ASan/UBSan, analyzer, amd64/i386 syntax PASS"
