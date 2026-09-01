#!/bin/sh
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -eu

test_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repo_root=$(CDPATH= cd -- "$test_dir/../../.." && pwd)
build_dir=$(mktemp -d "${TMPDIR:-/tmp}/zedbsd-intel-ax211-transport.XXXXXX")
trap 'rm -rf "$build_dir"' EXIT HUP INT TERM

cc=${CC:-cc}
warnings="-std=c89 -pedantic -Wno-long-long -Wall -Wextra -Werror"
core="$repo_root/src/drivers/intel-ax211.c"
source="$repo_root/src/drivers/intel-ax211-transport.c"
fixture="$test_dir/intel-ax211-transport-test.c"
abi_includes="-I$repo_root/libc/include -I$repo_root/include/uapi"
abi_includes="$abi_includes -I$repo_root/include -I$repo_root/src"

# Ordinary host gate.
# shellcheck disable=SC2086
$cc $warnings -O2 "$core" "$source" "$fixture" \
	-o "$build_dir/intel-ax211-transport"
"$build_dir/intel-ax211-transport"

# Memory-safety and undefined-behaviour gate.
# shellcheck disable=SC2086
$cc $warnings -O1 -g -fno-omit-frame-pointer \
	-fsanitize=address,undefined "$core" "$source" "$fixture" \
	-o "$build_dir/intel-ax211-transport-sanitize"
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 \
	UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
	"$build_dir/intel-ax211-transport-sanitize"

# Static lifetime and bounds analysis gate.
# shellcheck disable=SC2086
$cc $warnings -O0 -fanalyzer "$core" "$source" "$fixture" \
	-o "$build_dir/intel-ax211-transport-analyzer"
"$build_dir/intel-ax211-transport-analyzer"

# Keeps the private transport clean on both configured x86 ABIs.
# shellcheck disable=SC2086
$cc $warnings -m64 -nostdinc $abi_includes -fsyntax-only "$source"
# shellcheck disable=SC2086
$cc $warnings -m32 -nostdinc $abi_includes -fsyntax-only "$source"

echo "intel ax211 transport: ordinary, ASan/UBSan, analyzer, amd64/i386 syntax PASS"
