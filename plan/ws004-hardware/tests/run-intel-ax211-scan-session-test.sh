#!/bin/sh
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -eu

test_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repo_root=$(CDPATH= cd -- "$test_dir/../../.." && pwd)
build_dir=$(mktemp -d "${TMPDIR:-/tmp}/zedbsd-intel-ax211-scan-session.XXXXXX")
trap 'rm -rf "$build_dir"' EXIT HUP INT TERM

cc=${CC:-cc}
warnings="-std=c89 -pedantic -Wno-long-long -Wall -Wextra -Werror"
core="$repo_root/src/drivers/intel-ax211.c"
protocol="$repo_root/src/drivers/intel-ax211-protocol.c"
transport="$repo_root/src/drivers/intel-ax211-transport.c"
command="$repo_root/src/drivers/intel-ax211-command.c"
runtime="$repo_root/src/drivers/intel-ax211-runtime.c"
scan="$repo_root/src/drivers/intel-ax211-scan.c"
session="$repo_root/src/drivers/intel-ax211-scan-session.c"
sources="$core $protocol $transport $command $runtime $scan $session"
fixture="$test_dir/intel-ax211-scan-session-test.c"
abi_includes="-I$repo_root/libc/include -I$repo_root/include/uapi"
abi_includes="$abi_includes -I$repo_root/include -I$repo_root/src"

# shellcheck disable=SC2086
$cc $warnings -O2 $sources "$fixture" -o "$build_dir/ordinary"
"$build_dir/ordinary"

# shellcheck disable=SC2086
$cc $warnings -O1 -g -fno-omit-frame-pointer -fsanitize=address,undefined \
	$sources "$fixture" -o "$build_dir/sanitize"
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 \
	"$build_dir/sanitize"

# shellcheck disable=SC2086
$cc $warnings -O0 -fanalyzer $sources "$fixture" \
	-o "$build_dir/analyzer"
"$build_dir/analyzer"

# The PCI function is amd64-only; the private controller remains ILP32-clean.
# shellcheck disable=SC2086
$cc $warnings -m64 -nostdinc $abi_includes -fsyntax-only "$session"
# shellcheck disable=SC2086
$cc $warnings -m32 -nostdinc $abi_includes -fsyntax-only "$session"

echo "intel ax211 scan session: ordinary, ASan/UBSan, analyzer, amd64/i386 syntax PASS"
