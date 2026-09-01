#!/bin/sh
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -eu

test_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repo_root=$(CDPATH= cd -- "$test_dir/../../.." && pwd)
build_dir=$(mktemp -d "${TMPDIR:-/tmp}/zedbsd-intel-ax211-transport-backend.XXXXXX")
trap 'rm -rf "$build_dir"' EXIT HUP INT TERM

cc=${CC:-cc}
warnings="-std=c89 -pedantic -Wno-long-long -Wall -Wextra -Werror"
core="$repo_root/src/drivers/intel-ax211.c"
mmio="$repo_root/src/drivers/intel-ax211-mmio.c"
transport="$repo_root/src/drivers/intel-ax211-transport.c"
backend="$repo_root/src/drivers/intel-ax211-transport-backend.c"
fixture="$test_dir/intel-ax211-transport-backend-test.c"
includes="-I$repo_root/include -I$repo_root/src/drivers"
abi_includes="-I$repo_root/libc/include -I$repo_root/include/uapi"
abi_includes="$abi_includes -I$repo_root/include -I$repo_root/src"

# Ordinary native gate.
# shellcheck disable=SC2086
$cc $warnings -O2 $includes "$core" "$mmio" "$transport" "$backend" \
	"$fixture" -o "$build_dir/ordinary"
"$build_dir/ordinary"

# Memory-safety and undefined-behaviour gate.
# shellcheck disable=SC2086
$cc $warnings -O1 -g -fno-omit-frame-pointer \
	-fsanitize=address,undefined $includes "$core" "$mmio" "$transport" \
	"$backend" "$fixture" -o "$build_dir/sanitize"
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 \
	UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
	"$build_dir/sanitize"

# Static lifetime and bounds analysis gate.
# shellcheck disable=SC2086
$cc $warnings -O0 -fanalyzer $includes "$core" "$mmio" "$transport" \
	"$backend" "$fixture" -o "$build_dir/analyzer"
"$build_dir/analyzer"

# Keeps the adapter ABI-clean for both configured x86 targets.
# shellcheck disable=SC2086
$cc $warnings -m64 -nostdinc $abi_includes -fsyntax-only "$backend"
# shellcheck disable=SC2086
$cc $warnings -m32 -nostdinc $abi_includes -fsyntax-only "$backend"

echo "intel ax211 transport backend: ordinary, ASan/UBSan, analyzer, amd64/i386 syntax PASS"
