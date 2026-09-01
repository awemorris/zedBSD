#!/bin/sh
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -eu

repo=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)
temporary_root=${TMPDIR:-"$repo/plan/ws005-networking/temp"}
mkdir -p -- "$temporary_root"
temporary=$(mktemp -d "$temporary_root/wifi-command.XXXXXX")
trap 'rm -rf "$temporary"' EXIT HUP INT TERM

cc=${CC:-cc}
source_file=$repo/plan/ws005-networking/tests/wifi-command-test.c
common="-std=c11 -D_GNU_SOURCE -DZEDBSD_USER_ABI_LP64 \
	-I$repo/include/uapi -I$repo/libc/include -I$repo \
	-Wall -Wextra -Werror -ffunction-sections -fdata-sections"
discard="-Wl,--gc-sections"

# shellcheck disable=SC2086
$cc $common "$source_file" $discard -o "$temporary/wifi-command-test"
"$temporary/wifi-command-test"

# shellcheck disable=SC2086
$cc $common -fsanitize=address,undefined -fno-omit-frame-pointer \
	"$source_file" $discard -o "$temporary/wifi-command-sanitize"
ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1 \
	"$temporary/wifi-command-sanitize"

# Keep the analyzer build executable so the fixture assertions cover the same
# production command source in every variant.
# shellcheck disable=SC2086
$cc $common -fanalyzer "$source_file" $discard \
	-o "$temporary/wifi-command-analyzer"
"$temporary/wifi-command-analyzer"

echo 'wifi command test runner: PASS'
