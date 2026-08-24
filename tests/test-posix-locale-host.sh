#!/usr/bin/env bash
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
# POSIX-UTILITY-TEST: locale positive negative
set -euo pipefail

repo="$(cd "$(dirname "$0")/.." && pwd)"
work="$(mktemp -d "${TMPDIR:-/tmp}/zedbsd-posix-locale.XXXXXX")"
trap 'rm -rf "$work"' EXIT

cc -std=c11 -D_DEFAULT_SOURCE -O2 -Wall -Wextra -Werror -I"$repo" \
	"$repo/userland/base/localedef/main.c" \
	"$repo/userland/base/common/command.c" -o "$work/localedef"
cc -std=c11 -D_DEFAULT_SOURCE -DZEDBSD_USER_ABI_LP64 -O2 \
	-Wall -Wextra -Werror -I"$repo" -I"$repo/include" \
	-I"$repo/include/uapi" -I"$repo/libc/include" \
	"$repo/libc/locale.c" "$repo/libc/locale-db.c" \
	"$repo/tests/locale-host-shim.c" \
	"$repo/userland/base/locale/main.c" -o "$work/locale"

"$work/localedef" -f "$repo/tests/fixtures/UTF-8.charmap" \
	-i "$repo/tests/fixtures/zed-test-locale.src" "$work/zed-test"

test "$(LOCPATH="$work" LC_ALL=zed-test "$work/locale" decimal_point)" = '","'
test "$(LOCPATH="$work" LC_ALL=zed-test "$work/locale" -k decimal_point)" = \
	'decimal_point=","'
LOCPATH="$work" LC_ALL=zed-test "$work/locale" -ck LC_NUMERIC >"$work/numeric"
grep -qx 'LC_NUMERIC' "$work/numeric"
grep -qx 'grouping=3;3' "$work/numeric"
LOCPATH="$work" "$work/locale" -a >"$work/list"
grep -qx 'C' "$work/list"
grep -qx 'C.UTF-8' "$work/list"
test "$("$work/locale" -m)" = $'US-ASCII\nUTF-8'

if LOCPATH="$work" LC_ALL=zed-test "$work/locale" unknown-key \
	>/dev/null 2>&1; then
	echo 'locale accepted an unknown keyword' >&2
	exit 1
fi
if LOCPATH="$work" LC_ALL=does-not-exist "$work/locale" >/dev/null 2>&1; then
	echo 'locale accepted an unavailable locale' >&2
	exit 1
fi

echo 'zedBSD POSIX locale host tests: PASS'
