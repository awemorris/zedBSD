#!/bin/sh
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -eu

repo=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)
temporary_root=${TMPDIR:-"$repo/plan/ws005-networking/temp"}
mkdir -p -- "$temporary_root"
temporary=$(mktemp -d "$temporary_root/networkd-protocol.XXXXXX")
trap 'rm -rf "$temporary"' EXIT HUP INT TERM

cc=${CC:-cc}
common="-std=c11 -DZEDBSD_USER_ABI_LP64 -I$repo/include/uapi \
	-I$repo/libc/include -I$repo -Wall -Wextra -Werror"

# shellcheck disable=SC2086
$cc $common "$repo/userland/base/net/protocol.c" \
	"$repo/plan/ws005-networking/tests/networkd-protocol-test.c" \
	-o "$temporary/networkd-protocol-test"
"$temporary/networkd-protocol-test"

# shellcheck disable=SC2086
$cc $common -fsanitize=address,undefined -fno-omit-frame-pointer \
	"$repo/userland/base/net/protocol.c" \
	"$repo/plan/ws005-networking/tests/networkd-protocol-test.c" \
	-o "$temporary/networkd-protocol-sanitize"
ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1 \
	"$temporary/networkd-protocol-sanitize"

echo 'networkd protocol test runner: PASS'
