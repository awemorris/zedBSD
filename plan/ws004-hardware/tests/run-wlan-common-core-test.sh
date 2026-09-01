#!/bin/sh
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -eu

test_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repo_root=$(CDPATH= cd -- "$test_dir/../../.." && pwd)
build_dir=$(mktemp -d "${TMPDIR:-/tmp}/zedbsd-wlan.XXXXXX")
trap 'rm -rf "$build_dir"' EXIT HUP INT TERM

cc=${CC:-cc}
host_includes="-I$test_dir/host-include -I$repo_root/include/uapi -I$repo_root/include"
abi_includes="-I$repo_root/libc/include -I$repo_root/include/uapi -I$repo_root/include"
warnings="-std=c11 -Wall -Wextra -Werror"
sources="$repo_root/src/kern/net/wlan.c $repo_root/src/kern/net/wlan-frame.c"
fixture="$test_dir/wlan-common-core-test.c"
layout="$test_dir/wlan-uapi-layout-test.c"

# Native ABI layout and ordinary production-source fixture.
$cc $warnings $host_includes "$layout" -o "$build_dir/wlan-layout"
"$build_dir/wlan-layout"
$cc $warnings -O2 -pthread -DWLAN_TESTING $host_includes $sources "$fixture" \
	-o "$build_dir/wlan-common"
"$build_dir/wlan-common"

# Memory/undefined-behavior gate.
$cc $warnings -O1 -g -pthread -DWLAN_TESTING -fno-omit-frame-pointer \
	-fsanitize=address,undefined $host_includes $sources "$fixture" \
	-o "$build_dir/wlan-common-sanitize"
ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1 \
	"$build_dir/wlan-common-sanitize"

# Compiler analyzer gate over both production units and the linked fake.
$cc $warnings -O0 -pthread -DWLAN_TESTING -fanalyzer $host_includes $sources "$fixture" \
	-o "$build_dir/wlan-common-analyzer"
"$build_dir/wlan-common-analyzer"

# The public records must retain exactly the same offsets and sizes for both
# configured user ABIs.  -fsyntax-only avoids requiring 32-bit host libraries.
$cc -m64 -nostdinc $abi_includes -DZEDBSD_USER_ABI_LP64 $warnings \
	-fsyntax-only "$layout" $sources
$cc -m32 -nostdinc $abi_includes $warnings -fsyntax-only "$layout" $sources

echo "wlan common core: ordinary, sanitizer, analyzer, amd64/i386 ABI PASS"
