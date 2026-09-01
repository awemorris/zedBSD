#!/bin/sh
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -eu

test_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repo_root=$(CDPATH= cd -- "$test_dir/../../.." && pwd)
build_dir=$(mktemp -d "${TMPDIR:-/tmp}/zedbsd-rtl8822b.XXXXXX")
trap 'rm -rf "$build_dir"' EXIT HUP INT TERM

cc=${CC:-cc}
warnings="-std=c11 -Wall -Wextra -Werror"
defines="-DRTL8822B_TESTING -DRTL8822B_HOST_TEST"
source="$repo_root/src/drivers/rtl8822b.c"
fixture="$test_dir/rtl8822b-core-test.c"

$cc $warnings -O2 $defines "$source" "$fixture" \
	-o "$build_dir/rtl8822b-core"
"$build_dir/rtl8822b-core"

$cc $warnings -O1 -g $defines -fno-omit-frame-pointer \
	-fsanitize=address,undefined "$source" "$fixture" \
	-o "$build_dir/rtl8822b-core-sanitize"
ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 \
	"$build_dir/rtl8822b-core-sanitize"

$cc $warnings -O0 $defines -fanalyzer "$source" "$fixture" \
	-o "$build_dir/rtl8822b-core-analyzer"
"$build_dir/rtl8822b-core-analyzer"

# The selected optional firmware package can supply the exact positive image
# without making the ordinary fixture perform network access.
if [ "${RTL8822B_FIRMWARE_TEST_BLOB:-}" != "" ]; then
	"$build_dir/rtl8822b-core" "$RTL8822B_FIRMWARE_TEST_BLOB"
	# The caller may name the verified cache relative to the repository.
	# Copy it before changing directory so ld's binary input is independent of
	# the caller's spelling and cannot become a dangling relative symlink.
	cp "$RTL8822B_FIRMWARE_TEST_BLOB" "$build_dir/firmware.bin"
	(
		cd "$build_dir"
		ld -r -b binary firmware.bin -o firmware-blob.o
	)
	abi_includes="-I$repo_root/include -I$repo_root/include/uapi"
	abi_includes="$abi_includes -I$repo_root/src -I$repo_root"
	abi_includes="$abi_includes -I$repo_root/libc/include"
	loader_flags="$warnings -m64 -DHAL_ARCH_AMD64 -DHAL_BOARD_PCAT"
	loader_flags="$loader_flags -DZEDBSD_USER_ABI_LP64 $abi_includes"
	loader_fixture="$test_dir/rtl8822b-loader-test.c"
	$cc $loader_flags -O1 "$source" "$loader_fixture" \
		"$build_dir/firmware-blob.o" -no-pie \
		-o "$build_dir/rtl8822b-loader"
	"$build_dir/rtl8822b-loader"
	$cc $loader_flags -O1 -g -fno-omit-frame-pointer \
		-fsanitize=address,undefined "$source" "$loader_fixture" \
		"$build_dir/firmware-blob.o" -no-pie \
		-o "$build_dir/rtl8822b-loader-sanitize"
	ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 \
		"$build_dir/rtl8822b-loader-sanitize"
	$cc $loader_flags -O0 -fanalyzer "$source" "$loader_fixture" \
		"$build_dir/firmware-blob.o" -no-pie \
		-o "$build_dir/rtl8822b-loader-analyzer"
	"$build_dir/rtl8822b-loader-analyzer"
fi

echo "rtl8822b core: ordinary, sanitizer, analyzer PASS"
