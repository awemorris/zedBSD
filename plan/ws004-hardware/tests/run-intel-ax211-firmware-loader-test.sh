#!/bin/sh
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -eu

test_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repo_root=$(CDPATH= cd -- "$test_dir/../../.." && pwd)
build_dir=$(mktemp -d "${TMPDIR:-/tmp}/zedbsd-ax211-loader.XXXXXX")
trap 'rm -rf "$build_dir"' EXIT HUP INT TERM

revision=dc85ccedc9c973682fbcf4d628ca61174bcc3120
cache=${INTEL_AX211_FIRMWARE_TEST_CACHE:-$repo_root/build/sources/firmware/intelax211/$revision}
ucode=$cache/iwlwifi-so-a0-gf-a0-89.ucode
pnvm=$cache/iwlwifi-so-a0-gf-a0.pnvm
if [ ! -f "$ucode" ] || [ -L "$ucode" ] ||
    [ ! -f "$pnvm" ] || [ -L "$pnvm" ]; then
	echo "intel ax211 firmware loader: exact immutable cache is unavailable" >&2
	exit 1
fi

cp "$ucode" "$build_dir/ucode.bin"
cp "$pnvm" "$build_dir/pnvm.bin"
(
	cd "$build_dir"
	ld -r -b binary ucode.bin -o ucode.o
	ld -r -b binary pnvm.bin -o pnvm.o
)

cc=${CC:-cc}
warnings="-std=c11 -Wall -Wextra -Werror"
defines="-m64 -DHAL_ARCH_AMD64 -DHAL_BOARD_PCAT -DZEDBSD_USER_ABI_LP64"
defines="$defines -DINTEL_AX211_FIRMWARE_LOADER_HOST_TEST"
includes="-I$repo_root/libc/include -I$repo_root/include/uapi"
includes="$includes -I$repo_root/include -I$repo_root/src"
core=$repo_root/src/drivers/intel-ax211.c
loader=$repo_root/src/drivers/intel-ax211-firmware.c
fixture=$test_dir/intel-ax211-firmware-loader-test.c
objects="$build_dir/ucode.o $build_dir/pnvm.o"

# Production loader and exact pinned bytes, ordinary host gate.
# shellcheck disable=SC2086
$cc $warnings -O2 $defines $includes "$core" "$loader" "$fixture" \
	$objects -no-pie -o "$build_dir/ordinary"
"$build_dir/ordinary"

# The same VFS/lease/failure matrix under memory and UB instrumentation.
# shellcheck disable=SC2086
$cc $warnings -O1 -g -fno-omit-frame-pointer \
	-fsanitize=address,undefined $defines $includes \
	"$core" "$loader" "$fixture" $objects -no-pie \
	-o "$build_dir/sanitize"
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 \
	UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
	"$build_dir/sanitize"

# GCC interprocedural ownership and bounds analysis over production code.
# shellcheck disable=SC2086
$cc $warnings -O0 -fanalyzer $defines $includes \
	"$core" "$loader" "$fixture" $objects -no-pie \
	-o "$build_dir/analyzer"
"$build_dir/analyzer"

echo "intel ax211 firmware loader: ordinary, ASan/UBSan, analyzer PASS"
