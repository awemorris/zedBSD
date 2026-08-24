#!/usr/bin/env bash
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -euo pipefail

# POSIX-UTILITY-TEST: cal positive negative
# POSIX-UTILITY-TEST: expr positive negative
# POSIX-UTILITY-TEST: tsort positive negative

repo="$(cd "$(dirname "$0")/.." && pwd)"
work="$(mktemp -d "${TMPDIR:-/tmp}/zedbsd-posix-phase2a.XXXXXX")"
trap 'rm -rf "$work"' EXIT

for command in cal tsort; do
	cc -std=c11 -D_DEFAULT_SOURCE -O2 -Wall -Wextra -Werror \
		"$repo/userland/base/$command/main.c" -o "$work/$command"
done

cc -std=c11 -D_DEFAULT_SOURCE -DZEDBSD_REGEX_HOST_TEST -O2 -Wall -Wextra \
	-Werror -I"$repo/libc/include" -I"$repo/include/uapi" \
	"$repo/libc/regex/regcomp.c" "$repo/libc/regex/regexec.c" \
	"$repo/libc/regex/regerror.c" "$repo/libc/regex/tre-mem.c" \
	"$repo/userland/base/expr/main.c" -o "$work/expr"

LC_ALL=C "$work/cal" 9 1752 >"$work/september"
grep -q 'September 1752' "$work/september"
for missing in 3 4 5 6 7 8 9 10 11 12 13; do
	if grep -Eq "(^|[[:space:]])$missing([[:space:]]|$)" "$work/september"; then
		echo "cal printed omitted September 1752 day $missing" >&2
		exit 1
	fi
done
grep -Eq '(^|[[:space:]])2[[:space:]]+14([[:space:]]|$)' "$work/september"
test "$(LC_ALL=C "$work/cal" 83 | grep -c ' 83$')" -eq 12
if "$work/cal" 13 2024 >/dev/null 2>&1; then
	echo 'cal accepted month 13' >&2
	exit 1
fi

test "$(LC_ALL=C "$work/expr" 2 + 3 \* 4)" = 14
test "$(LC_ALL=C "$work/expr" \( 2 + 3 \) \* 4)" = 20
test "$(LC_ALL=C "$work/expr" 09 \< 10)" = 1
test "$(LC_ALL=C "$work/expr" 0 \| fallback)" = fallback
test "$(LC_ALL=C "$work/expr" yes \& value)" = yes
test "$(LC_ALL=C "$work/expr" abcab : '\(ab\)c\1')" = ab
test "$(LC_ALL=C "$work/expr" z123 : '[[:alpha:]][[:digit:]]\{2,3\}')" = 4
set +e
LC_ALL=C "$work/expr" 0 >"$work/expr-zero"
zero_status=$?
LC_ALL=C "$work/expr" 1 + >"$work/expr-invalid" 2>&1
invalid_status=$?
LC_ALL=C "$work/expr" 9223372036854775807 + 1 \
	>"$work/expr-overflow" 2>&1
overflow_status=$?
set -e
test "$zero_status" -eq 1
test "$invalid_status" -eq 2
test "$overflow_status" -gt 2
grep -q 'integer overflow' "$work/expr-overflow"

printf 'a b\na b\nb c\nd d\n' | "$work/tsort" >"$work/order"
test "$(grep -n '^a$' "$work/order" | cut -d: -f1)" -lt \
	"$(grep -n '^b$' "$work/order" | cut -d: -f1)"
test "$(grep -n '^b$' "$work/order" | cut -d: -f1)" -lt \
	"$(grep -n '^c$' "$work/order" | cut -d: -f1)"
test "$(grep -c '^d$' "$work/order")" -eq 1
if printf 'a b c\n' | "$work/tsort" >/dev/null 2>"$work/odd-error"; then
	echo 'tsort accepted an odd input field count' >&2
	exit 1
fi
grep -q 'odd number' "$work/odd-error"
set +e
printf 'a b b a c d d c\n' | "$work/tsort" -w \
	>"$work/cycle-order" 2>"$work/cycle-error"
cycle_status=$?
set -e
test "$cycle_status" -eq 2
test "$(sort -u "$work/cycle-order" | wc -l)" -eq 4
test "$(grep -c 'input contains a cycle' "$work/cycle-error")" -eq 2

echo 'zedBSD POSIX phase 2a host tests: PASS'
