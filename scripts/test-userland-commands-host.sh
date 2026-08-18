#!/usr/bin/env bash
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -euo pipefail
repo="$(cd "$(dirname "$0")/.." && pwd)"
work="$(mktemp -d "${TMPDIR:-/tmp}/zedbsd-command-host.XXXXXX")"
trap 'rm -rf "$work"' EXIT

build()
{
	cc -std=c11 -D_DEFAULT_SOURCE -O2 -Wall -Wextra -Werror \
		-Walloc-size-larger-than=18446744073709551615 -I"$repo" \
		"$repo/userland/common/command.c" "$repo/userland/$1/main.c" \
		-o "$work/$1"
}

for command in od tr cut paste sort uniq comm fold fmt pr nl expand unexpand \
    grep sed awk xargs iconv diff date du nohup time timeout; do
	build "$command"
done

printf 'gamma\nalpha\nalpha\nbeta\n' >"$work/in"
printf 'one\ttwo\n' >"$work/tab"

test "$(printf abc | "$work/tr" a-z A-Z)" = ABC
test "$("$work/cut" -b 1-5 "$work/in" | head -n 1)" = gamma
test "$("$work/sort" -u "$work/in")" = $'alpha\nbeta\ngamma'
test "$("$work/uniq" -c "$work/in" | sed -n '2p')" = '      2 alpha'
test "$("$work/comm" -12 "$work/in" "$work/in" | wc -l)" -eq 4
test "$(printf abcdef | "$work/fold" -w 3)" = $'abc\ndef'
test "$(printf 'x\n' | "$work/nl")" = $'     1\tx'
test "$("$work/expand" "$work/tab")" = 'one     two'
test "$(printf 'one     two\n' | "$work/unexpand" -a)" = $'one\ttwo'
test "$("$work/grep" '^a.*a$' "$work/in" | wc -l)" -eq 2
test "$("$work/sed" 's/alpha/zed/g' "$work/in" | grep -c zed)" -eq 2
test "$("$work/awk" '{ print $1 }' "$work/in" | head -n 1)" = gamma
test "$(printf basename | "$work/xargs" /usr/bin/basename)" = basename
cmp "$work/in" <("$work/iconv" -f UTF-8 -t UTF-8 "$work/in")
"$work/diff" "$work/in" "$work/in"
test "$("$work/date" +%Y | wc -c)" -eq 5
"$work/timeout" 1 /bin/true

if "$work/cut" -b 0 "$work/in" >/dev/null 2>&1; then
	echo 'cut accepted an invalid zero position' >&2
	exit 1
fi
if printf '\377' | "$work/iconv" >/dev/null 2>&1; then
	echo 'iconv accepted invalid UTF-8' >&2
	exit 1
fi
if "$work/grep" needle "$work/in" >/dev/null; then
	echo 'grep returned match for an absent pattern' >&2
	exit 1
fi

echo 'zedBSD userland command host test: PASS'
