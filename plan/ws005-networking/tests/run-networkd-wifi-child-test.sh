#!/bin/sh
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -eu

repo=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)
temporary_root=${TMPDIR:-"$repo/plan/ws005-networking/temp"}
mkdir -p -- "$temporary_root"
temporary=$(mktemp -d "$temporary_root/networkd-wifi-child.XXXXXX")
trap 'rm -rf "$temporary"' EXIT HUP INT TERM

cc=${CC:-cc}
common="-std=c11 -D_DEFAULT_SOURCE -I$repo/include/uapi -I$repo \
	-Wall -Wextra -Werror"

# The fixture includes the implementation so it can replace only execv while
# every pipe, fork, poll, signal, and wait operation uses the host kernel.
# shellcheck disable=SC2086
$cc $common \
	"$repo/plan/ws005-networking/tests/networkd-wifi-child-test.c" \
	-o "$temporary/networkd-wifi-child-test"
"$temporary/networkd-wifi-child-test"

# shellcheck disable=SC2086
$cc $common -fsanitize=address,undefined -fno-omit-frame-pointer \
	"$repo/plan/ws005-networking/tests/networkd-wifi-child-test.c" \
	-o "$temporary/networkd-wifi-child-sanitize"
ASAN_OPTIONS=detect_leaks=0:handle_segv=0 UBSAN_OPTIONS=halt_on_error=1 \
	"$temporary/networkd-wifi-child-sanitize"

if command -v clang >/dev/null 2>&1; then
	# shellcheck disable=SC2086
	clang $common --analyze \
		"$repo/userland/base/networkd/wifi-child.c" \
		-o "$temporary/networkd-wifi-child.plist"
elif "$cc" -Q --help=common 2>/dev/null | grep -q -- '-fanalyzer'; then
	# shellcheck disable=SC2086
	$cc $common -fanalyzer -c \
		"$repo/userland/base/networkd/wifi-child.c" \
		-o "$temporary/networkd-wifi-child-analyzer.o"
fi

echo 'networkd wifi child test runner: PASS'
