#!/usr/bin/env bash
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
# POSIX-UTILITY-TEST: localedef positive negative
set -euo pipefail

repo="$(cd "$(dirname "$0")/.." && pwd)"
work="$(mktemp -d "${TMPDIR:-/tmp}/zedbsd-posix-localedef.XXXXXX")"
trap 'rm -rf "$work"' EXIT

cc -std=c11 -D_DEFAULT_SOURCE -O2 -Wall -Wextra -Werror -I"$repo" \
	"$repo/userland/base/localedef/main.c" \
	"$repo/userland/base/common/command.c" -o "$work/localedef"
cc -std=c11 -D_DEFAULT_SOURCE -DZEDBSD_USER_ABI_LP64 -O2 \
	-Wall -Wextra -Werror -I"$repo" -I"$repo/include" \
	-I"$repo/include/uapi" -I"$repo/libc/include" \
	"$repo/libc/locale.c" "$repo/libc/locale-db.c" \
	"$repo/tests/posix-locale-db-host-test.c" -o "$work/locale-test"

"$work/localedef" -f "$repo/tests/fixtures/UTF-8.charmap" \
	-i "$repo/tests/fixtures/zed-test-locale.src" "$work/zed-test"
"$work/locale-test" "$work"

cp "$work/zed-test" "$work/before"
printf 'LC_NUMERIC\ndecimal_point "unterminated\n' >"$work/invalid.src"
if "$work/localedef" -i "$work/invalid.src" "$work/zed-test" \
	2>/dev/null; then
	echo 'localedef accepted malformed input' >&2
	exit 1
fi
cmp "$work/zed-test" "$work/before"

printf '<code_set_name> "UNSUPPORTED"\n' >"$work/bad.charmap"
if "$work/localedef" -f "$work/bad.charmap" \
	-i "$repo/tests/fixtures/zed-test-locale.src" "$work/bad" \
	2>/dev/null; then
	echo 'localedef accepted an unsupported charmap' >&2
	exit 1
fi

mkdir "$work/corrupt-db"
cp "$work/zed-test" "$work/corrupt-db/zed-test"
printf X | dd of="$work/corrupt-db/zed-test" bs=1 seek=0 conv=notrunc status=none
if "$work/locale-test" "$work/corrupt-db" >/dev/null 2>&1; then
	echo 'setlocale accepted a corrupt locale artifact' >&2
	exit 1
fi

echo 'zedBSD POSIX localedef/locale database host tests: PASS'
