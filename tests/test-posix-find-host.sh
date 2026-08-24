#!/usr/bin/env bash
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
# POSIX-UTILITY-TEST: find positive negative
set -euo pipefail

repo="$(cd "$(dirname "$0")/.." && pwd)"
work="$(mktemp -d "${TMPDIR:-/tmp}/zedbsd-posix-find.XXXXXX")"
trap 'rm -rf "$work"' EXIT

cc -std=c11 -D_DEFAULT_SOURCE -O2 -Wall -Wextra -Werror \
	-I"$repo" "$repo/userland/base/find/main.c" \
	"$repo/libc/fnmatch.c" -o "$work/find"
mkdir -p "$work/tree/keep/nested" "$work/tree/skip/hidden"
printf 'alpha\n' >"$work/tree/keep/one.c"
printf 'beta\n' >"$work/tree/keep/two.h"
printf 'gamma\n' >"$work/tree/skip/hidden/three.c"

"$work/find" "$work/tree" -name '*.c' -print >"$work/names"
grep -qx "$work/tree/keep/one.c" "$work/names"
grep -qx "$work/tree/skip/hidden/three.c" "$work/names"
test "$(wc -l <"$work/names")" -eq 2

"$work/find" "$work/tree" \( -name skip -prune \) -o -name '*.c' -print \
	>"$work/pruned"
grep -qx "$work/tree/keep/one.c" "$work/pruned"
test "$(wc -l <"$work/pruned")" -eq 1

"$work/find" "$work/tree/keep" -type f -exec sh -c \
	'printf "%s\n" "$1"' sh '{}' \; >"$work/executed"
grep -qx "$work/tree/keep/one.c" "$work/executed"
grep -qx "$work/tree/keep/two.h" "$work/executed"

if "$work/find" "$work/tree" -type nope >/dev/null 2>&1; then
	echo 'find accepted an invalid file type' >&2
	exit 1
fi
if "$work/find" "$work/tree" \( -print >/dev/null 2>&1; then
	echo 'find accepted an unterminated expression' >&2
	exit 1
fi

echo 'zedBSD POSIX find host tests: PASS'
