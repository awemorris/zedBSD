#!/usr/bin/env bash
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -euo pipefail

# POSIX-UTILITY-TEST: uudecode positive negative
# POSIX-UTILITY-TEST: uuencode positive negative

repo="$(cd "$(dirname "$0")/.." && pwd)"
work="$(mktemp -d "${TMPDIR:-/tmp}/zedbsd-posix-phase2b.XXXXXX")"
trap 'rm -rf "$work"' EXIT

for command in uuencode uudecode; do
	cc -std=c11 -D_DEFAULT_SOURCE -O2 -Wall -Wextra -Werror -I"$repo" \
		"$repo/userland/base/$command/main.c" \
		"$repo/userland/base/common/uucodec.c" -o "$work/$command"
done

printf Cat >"$work/input"
chmod 0644 "$work/input"
"$work/uuencode" "$work/input" decoded >"$work/historical"
grep -qx 'begin 644 decoded' "$work/historical"
grep -qx '#0V%T' "$work/historical"
tail -n 2 "$work/historical" | grep -qx '`'
tail -n 1 "$work/historical" | grep -qx end

"$work/uuencode" -m "$work/input" decoded >"$work/base64"
grep -qx 'begin-base64 644 decoded' "$work/base64"
grep -qx Q2F0 "$work/base64"
tail -n 1 "$work/base64" | grep -qx '===='

mkdir "$work/decode"
(
	cd "$work/decode"
	"$work/uudecode" "$work/historical"
	cmp decoded "$work/input"
	rm decoded
	"$work/uudecode" "$work/base64"
	cmp decoded "$work/input"
)

printf 'begin 4777 mode-test\n#0V%%T\n`\nend\n' >"$work/mode-input"
(
	cd "$work/decode"
	"$work/uudecode" "$work/mode-input"
	test "$(stat -c %a mode-test)" = 777
)

printf safe >"$work/existing"
printf 'begin 644 existing\n!invalid\n' >"$work/malformed"
if (cd "$work" && "$work/uudecode" malformed >/dev/null 2>&1); then
	echo 'uudecode accepted malformed data' >&2
	exit 1
fi
test "$(cat "$work/existing")" = safe

printf 'begin 644 ../escaped\n`\nend\n' >"$work/traversal"
if (cd "$work/decode" && "$work/uudecode" "$work/traversal" \
	>/dev/null 2>&1); then
	echo 'uudecode accepted parent-directory traversal' >&2
	exit 1
fi
test ! -e "$work/escaped"

mkdir "$work/outside"
ln -s "$work/outside" "$work/decode/link"
printf 'begin 644 link/escaped\n`\nend\n' >"$work/symlink-traversal"
if (cd "$work/decode" && "$work/uudecode" "$work/symlink-traversal" \
	>/dev/null 2>&1); then
	echo 'uudecode followed a symlinked parent directory' >&2
	exit 1
fi
test ! -e "$work/outside/escaped"

printf 'begin-base64 644 bad\nQ2=F\n====\n' >"$work/bad-base64"
if (cd "$work/decode" && "$work/uudecode" "$work/bad-base64" \
	>/dev/null 2>&1); then
	echo 'uudecode accepted malformed base64' >&2
	exit 1
fi
test ! -e "$work/decode/bad"

echo 'zedBSD POSIX phase 2b host tests: PASS'
