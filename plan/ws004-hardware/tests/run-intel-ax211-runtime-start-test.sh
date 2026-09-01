#!/bin/sh
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -eu

test_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repo_root=$(CDPATH= cd -- "$test_dir/../../.." && pwd)
build_dir=$(mktemp -d "${TMPDIR:-/tmp}/zedbsd-intel-ax211-runtime-start.XXXXXX")
trap 'rm -rf "$build_dir"' EXIT HUP INT TERM

cc=${CC:-cc}
warnings="-std=c89 -pedantic -Wno-long-long -Wall -Wextra -Werror"
core="$repo_root/src/drivers/intel-ax211.c"
protocol="$repo_root/src/drivers/intel-ax211-protocol.c"
init="$repo_root/src/drivers/intel-ax211-init.c"
command="$repo_root/src/drivers/intel-ax211-command.c"
runtime="$repo_root/src/drivers/intel-ax211-runtime.c"
start="$repo_root/src/drivers/intel-ax211-runtime-start.c"
fixture="$test_dir/intel-ax211-runtime-start-test.c"
sources="$core $protocol $init $command $runtime $start $fixture"
abi_includes="-I$repo_root/libc/include -I$repo_root/include/uapi"
abi_includes="$abi_includes -I$repo_root/include -I$repo_root/src"

# Ordinary native gate.
# shellcheck disable=SC2086
$cc $warnings -O2 -I"$repo_root/include" $sources \
	-o "$build_dir/intel-ax211-runtime-start"
"$build_dir/intel-ax211-runtime-start"

# Memory-safety and undefined-behaviour gate.
# shellcheck disable=SC2086
$cc $warnings -O1 -g -fno-omit-frame-pointer \
	-fsanitize=address,undefined -I"$repo_root/include" $sources \
	-o "$build_dir/intel-ax211-runtime-start-sanitize"
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 \
	UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
	"$build_dir/intel-ax211-runtime-start-sanitize"

# Static lifetime and bounds analysis gate.
# shellcheck disable=SC2086
$cc $warnings -O0 -fanalyzer -I"$repo_root/include" $sources \
	-o "$build_dir/intel-ax211-runtime-start-analyzer"
"$build_dir/intel-ax211-runtime-start-analyzer"

# AX211 hardware is amd64, while the private coordinator stays ILP32-clean.
# shellcheck disable=SC2086
$cc $warnings -m64 -nostdinc $abi_includes -fsyntax-only "$start"
# shellcheck disable=SC2086
$cc $warnings -m32 -nostdinc $abi_includes -fsyntax-only "$start"

echo "intel ax211 operational runtime start: ordinary, ASan/UBSan, analyzer, amd64/i386 syntax PASS"
