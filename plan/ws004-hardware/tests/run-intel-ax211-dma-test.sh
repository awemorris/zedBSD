#!/bin/sh
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -eu

test_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repo_root=$(CDPATH= cd -- "$test_dir/../../.." && pwd)
build_dir=$(mktemp -d "${TMPDIR:-/tmp}/zedbsd-ax211-dma.XXXXXX")
trap 'rm -rf "$build_dir"' EXIT HUP INT TERM

cc=${CC:-cc}
warnings="-std=c11 -Wall -Wextra -Werror"
defines="-DINTEL_AX211_DMA_HOST_TEST"
includes="-I$repo_root/include -I$repo_root/include/uapi -I$repo_root/src"
core=$repo_root/src/drivers/intel-ax211.c
dma=$repo_root/src/drivers/intel-ax211-dma.c
fixture=$test_dir/intel-ax211-dma-test.c
objects=

revision=dc85ccedc9c973682fbcf4d628ca61174bcc3120
cache=${INTEL_AX211_FIRMWARE_TEST_CACHE:-$repo_root/build/sources/firmware/intelax211/$revision}
ucode=$cache/iwlwifi-so-a0-gf-a0-89.ucode
pnvm=$cache/iwlwifi-so-a0-gf-a0.pnvm
if [ -f "$ucode" ] && [ ! -L "$ucode" ] &&
    [ -f "$pnvm" ] && [ ! -L "$pnvm" ]; then
	cp "$ucode" "$build_dir/ucode.bin"
	cp "$pnvm" "$build_dir/pnvm.bin"
	(
		cd "$build_dir"
		ld -r -b binary ucode.bin -o ucode.o
		ld -r -b binary pnvm.bin -o pnvm.o
	)
	defines="$defines -DINTEL_AX211_DMA_EXACT_BLOB_TEST"
	objects="$build_dir/ucode.o $build_dir/pnvm.o"
fi

# Ordinary ownership/layout gate.
# shellcheck disable=SC2086
$cc $warnings -O2 $defines $includes "$core" "$dma" "$fixture" \
	$objects -no-pie -o "$build_dir/ordinary"
"$build_dir/ordinary"

# The complete allocation-failure matrix under memory/UB instrumentation.
# shellcheck disable=SC2086
$cc $warnings -O1 -g -fno-omit-frame-pointer \
	-fsanitize=address,undefined $defines $includes \
	"$core" "$dma" "$fixture" $objects -no-pie \
	-o "$build_dir/sanitize"
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 \
	UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
	"$build_dir/sanitize"

# Static allocation ownership and bounds analysis.
# shellcheck disable=SC2086
$cc $warnings -O0 -fanalyzer $defines $includes \
	"$core" "$dma" "$fixture" $objects -no-pie \
	-o "$build_dir/analyzer"
"$build_dir/analyzer"

# Private codecs and DMA ownership stay ABI-width-clean.
abi_includes="-I$repo_root/libc/include -I$repo_root/include/uapi"
abi_includes="$abi_includes -I$repo_root/include -I$repo_root/src"
# shellcheck disable=SC2086
$cc $warnings -m64 -nostdinc $abi_includes -fsyntax-only "$dma"
# shellcheck disable=SC2086
$cc $warnings -m32 -nostdinc $abi_includes -fsyntax-only "$dma"

echo "intel ax211 dma: ordinary, ASan/UBSan, analyzer, amd64/i386 syntax PASS"
