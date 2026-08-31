#!/bin/sh
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -eu

repo=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)
temporary_root=${TMPDIR:-"$repo/plan/ws005-networking/temp"}
mkdir -p -- "$temporary_root"
temporary=$(mktemp -d "$temporary_root/networkd-auth.XXXXXX")
trap 'rm -rf "$temporary"' EXIT HUP INT TERM

cc=${CC:-cc}
source_file=$repo/plan/ws005-networking/tests/networkd-auth-test.c
common="-std=c11 -DZEDBSD_USER_ABI_LP64 -I$repo/include/uapi \
	-I$repo/libc/include -I$repo -Wall -Wextra -Werror \
	-ffunction-sections -fdata-sections"
discard="-Wl,--gc-sections"

# shellcheck disable=SC2086
$cc $common "$source_file" $discard \
	-o "$temporary/networkd-auth-test"
"$temporary/networkd-auth-test"

# shellcheck disable=SC2086
$cc $common -fsanitize=address,undefined -fno-omit-frame-pointer \
	"$source_file" $discard -o "$temporary/networkd-auth-sanitize"
ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1 \
	"$temporary/networkd-auth-sanitize"

# Build and execute the analyzer-instrumented fixture as a third independent
# variant so its runtime assertions remain part of this entry point.
# shellcheck disable=SC2086
$cc $common -fanalyzer "$source_file" $discard \
	-o "$temporary/networkd-auth-analyzer"
"$temporary/networkd-auth-analyzer"

echo 'networkd auth test runner: PASS'
