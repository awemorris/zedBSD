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
		"$repo/userland/base/common/command.c" \
		"$repo/userland/base/$1/main.c" -o "$work/$1"
}

for command in od tr cut paste sort uniq comm fold fmt pr nl expand unexpand \
	grep sed awk xargs iconv diff date du nohup time timeout; do
	build "$command"
done
build readlink
build realpath
build gettext
build msgfmt
cp "$work/gettext" "$work/ngettext"

# The matrix checker requires one exact marker for each reviewed utility whose
# positive and negative behavior is exercised by this file.
# POSIX-UTILITY-TEST: gettext positive negative
# POSIX-UTILITY-TEST: msgfmt positive negative
# POSIX-UTILITY-TEST: ngettext positive negative
# POSIX-UTILITY-TEST: readlink positive negative
# POSIX-UTILITY-TEST: realpath positive negative
# POSIX-UTILITY-TEST: timeout positive negative

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
"$work/timeout" 0 /bin/true
test "$("$work/timeout" 0.02s /bin/sleep 1; printf '%s' "$?")" = 124
test "$("$work/timeout" -s kill 0.02s /bin/sleep 1; printf '%s' "$?")" = 124
test "$("$work/timeout" 1 command-that-does-not-exist; printf '%s' "$?")" = 127
ln -s target "$work/link"
test "$("$work/readlink" -n "$work/link")" = target
test "$("$work/readlink" "$work/link")" = target
if "$work/readlink" "$work/missing-link" >/dev/null 2>&1; then
	echo 'readlink accepted a missing path' >&2
	exit 1
fi
test "$("$work/realpath" -E "$work/missing")" = "$work/missing"
if "$work/realpath" -e "$work/missing" >/dev/null 2>&1; then
	echo 'realpath -e accepted a missing final component' >&2
	exit 1
fi
test "$(LC_ALL=C "$work/gettext" hello)" = hello
test "$(LC_ALL=C "$work/gettext" -s one two)" = 'one two'
test "$(LC_ALL=C "$work/gettext" -e 'one\ntwo')" = $'one\ntwo'
test "$(LC_ALL=C "$work/ngettext" one many 0)" = many
test "$(LC_ALL=C "$work/ngettext" one many 1)" = one
"$work/msgfmt" -S -o "$work/messages.mo" "$repo/tests/fixtures/messages.po"
msgunfmt "$work/messages.mo" >"$work/messages.roundtrip.po"
grep -q '^msgstr "bonjour"$' "$work/messages.roundtrip.po"
grep -q '^msgstr\[1\] "fichiers"$' "$work/messages.roundtrip.po"
if grep -q '^msgid "old"$' "$work/messages.roundtrip.po"; then
	echo 'msgfmt included a fuzzy entry without -f' >&2
	exit 1
fi
if "$work/msgfmt" -c -o "$work/invalid.mo" \
    "$repo/tests/fixtures/messages-invalid.po" >/dev/null 2>&1; then
	echo 'msgfmt accepted incompatible c-format arguments' >&2
	exit 1
fi

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
if "$work/gettext" -eE invalid >/dev/null 2>&1; then
	echo 'gettext accepted mutually exclusive escape options' >&2
	exit 1
fi
if "$work/ngettext" one many -1 >/dev/null 2>&1; then
	echo 'ngettext accepted a negative plural selector' >&2
	exit 1
fi

echo 'zedBSD userland command host test: PASS'
