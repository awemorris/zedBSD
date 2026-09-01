#!/bin/sh
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -eu

test_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repo_root=$(CDPATH= cd -- "$test_dir/../../.." && pwd)
build_dir=$(mktemp -d "${TMPDIR:-/tmp}/zedbsd-ax211-wlan-common.XXXXXX")
trap 'rm -rf "$build_dir"' EXIT HUP INT TERM

cc=${CC:-cc}
warnings="-std=c11 -Wall -Wextra -Werror"
host_includes="-I$test_dir/host-include -I$repo_root/include/uapi"
host_includes="$host_includes -I$repo_root/include -I$repo_root/src"
abi_includes="-I$repo_root/libc/include -I$repo_root/include/uapi"
abi_includes="$abi_includes -I$repo_root/include -I$repo_root/src"
common_sources="$repo_root/src/kern/net/wlan.c \
$repo_root/src/kern/net/wlan-frame.c \
$repo_root/src/kern/net/wlan-crypto.c \
$repo_root/src/kern/net/wlan-wpa2-codec.c \
$repo_root/src/kern/net/wlan-wpa2.c \
$repo_root/src/kern/net/wlan-l2.c"
private_sources="$repo_root/src/drivers/intel-ax211-protocol.c \
$repo_root/src/drivers/intel-ax211-scan.c \
$repo_root/src/drivers/intel-ax211-assoc.c \
$repo_root/src/drivers/intel-ax211-bss.c \
$repo_root/src/drivers/intel-ax211-key.c \
$repo_root/src/drivers/intel-ax211-tx.c \
$repo_root/src/drivers/intel-ax211-rx.c"
sources="$common_sources $private_sources"
fixture="$test_dir/intel-ax211-wlan-common-integration-test.c"

# Production common-WLAN plus AX211-private finite-transport integration.
# shellcheck disable=SC2086
$cc $warnings -O2 -DWLAN_TESTING $host_includes $sources "$fixture" \
	-o "$build_dir/ordinary"
"$build_dir/ordinary"

# Memory and undefined-behaviour gate over the identical linked path.
# shellcheck disable=SC2086
$cc $warnings -O1 -g -fno-omit-frame-pointer -DWLAN_TESTING \
	-fsanitize=address,undefined $host_includes $sources "$fixture" \
	-o "$build_dir/sanitize"
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 \
	UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
	"$build_dir/sanitize"

# Interprocedural analyzer gate over common and private production units.
# shellcheck disable=SC2086
$cc $warnings -O0 -fanalyzer -DWLAN_TESTING $host_includes $sources \
	"$fixture" -o "$build_dir/analyzer"
"$build_dir/analyzer"

# The PCI function is amd64-only, while every private/common wire boundary
# used here must retain clean fixed-width arithmetic under both user ABIs.
# shellcheck disable=SC2086
$cc $warnings -m64 -nostdinc $abi_includes -DWLAN_TESTING \
	-fsyntax-only $sources "$fixture"
# shellcheck disable=SC2086
$cc $warnings -m32 -nostdinc $abi_includes -DWLAN_TESTING \
	-fsyntax-only $sources "$fixture"

echo "intel ax211/common WLAN: ordinary, sanitizer, analyzer, amd64/i386 PASS"
