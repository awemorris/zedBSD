#!/bin/sh
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -eu

test_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repo_root=$(CDPATH= cd -- "$test_dir/../../.." && pwd)
build_dir=$(mktemp -d "${TMPDIR:-/tmp}/zedbsd-intel-ax211.XXXXXX")
trap 'rm -rf "$build_dir"' EXIT HUP INT TERM

cc=${CC:-cc}
warnings="-std=c11 -Wall -Wextra -Werror"
source="$repo_root/src/drivers/intel-ax211.c"
fixture="$test_dir/intel-ax211-core-test.c"
abi_includes="-I$repo_root/libc/include -I$repo_root/include/uapi"
abi_includes="$abi_includes -I$repo_root/include -I$repo_root/src"
firmware_cache="$repo_root/build/sources/firmware/intelax211"
firmware_cache="$firmware_cache/dc85ccedc9c973682fbcf4d628ca61174bcc3120"
firmware="$firmware_cache/iwlwifi-so-a0-gf-a0-89.ucode"
pnvm="$firmware_cache/iwlwifi-so-a0-gf-a0.pnvm"

# Ordinary amd64 host gate.
# shellcheck disable=SC2086
$cc $warnings -O2 "$source" "$fixture" \
	-o "$build_dir/intel-ax211-core"
"$build_dir/intel-ax211-core"
if [ -r "$firmware" ] && [ -r "$pnvm" ]; then
	"$build_dir/intel-ax211-core" "$firmware" "$pnvm"
fi

# Memory-safety and undefined-behaviour gate.
# shellcheck disable=SC2086
$cc $warnings -O1 -g -fno-omit-frame-pointer \
	-fsanitize=address,undefined "$source" "$fixture" \
	-o "$build_dir/intel-ax211-core-sanitize"
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 \
	UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
	"$build_dir/intel-ax211-core-sanitize"
if [ -r "$firmware" ] && [ -r "$pnvm" ]; then
	ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 \
		UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
		"$build_dir/intel-ax211-core-sanitize" "$firmware" "$pnvm"
fi

# Static lifetime/bounds analysis gate.
# shellcheck disable=SC2086
$cc $warnings -O0 -fanalyzer "$source" "$fixture" \
	-o "$build_dir/intel-ax211-core-analyzer"
"$build_dir/intel-ax211-core-analyzer"

# AX211 is amd64 hardware, but this private wire core must stay ILP32-clean.
# A syntax-only gate avoids requiring a 32-bit runtime or linker installation.
# shellcheck disable=SC2086
$cc $warnings -m64 -nostdinc $abi_includes -fsyntax-only "$source"
# shellcheck disable=SC2086
$cc $warnings -m32 -nostdinc $abi_includes -fsyntax-only "$source"

echo "intel ax211 core: ordinary, ASan/UBSan, analyzer, amd64/i386 syntax PASS"
