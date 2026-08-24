#!/usr/bin/env bash
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
# POSIX-UTILITY-TEST: tabs positive negative
# POSIX-UTILITY-TEST: tput positive negative
set -euo pipefail

repo="$(cd "$(dirname "$0")/.." && pwd)"
work="$(mktemp -d "${TMPDIR:-/tmp}/zedbsd-posix-terminal.XXXXXX")"
trap 'rm -rf "$work"' EXIT

for command in tput tabs; do
	cc -std=c11 -D_DEFAULT_SOURCE -O2 -Wall -Wextra -Werror -I"$repo" \
		"$repo/userland/base/$command/main.c" \
		"$repo/userland/base/common/terminfo.c" \
		"$repo/userland/base/common/command.c" -o "$work/$command"
done
cc -std=c11 -D_DEFAULT_SOURCE -O2 -Wall -Wextra -Werror -I"$repo" \
	"$repo/userland/base/common/terminfo.c" \
	"$repo/tests/terminfo-expander-host-test.c" -o "$work/expander-test"

export TERMINFO="$repo/userland/base/terminfo"
test "$("$work/tput" -T zed cols)" = 80
test "$("$work/tput" -T zed longname)" = 'zedBSD virtual console'
"$work/tput" -T zed cup 1 2 >"$work/cup"
test "$(od -An -tx1 "$work/cup" | tr -d ' \n')" = '1b5b323b3348'
"$work/tput" -T zed clear >"$work/clear"
test "$(od -An -tx1 "$work/clear" | tr -d ' \n')" = '1b5b481b5b324a'
"$work/tput" -T zed am

if "$work/tput" -T unknown clear >/dev/null 2>&1; then
	echo 'tput accepted an unknown terminal' >&2
	exit 1
fi
if "$work/tput" -T zed not-a-capability >/dev/null 2>&1; then
	echo 'tput accepted an unknown capability' >&2
	exit 1
fi

"$work/tabs" -T zed -8 >"$work/tabs.out"
test -s "$work/tabs.out"
for profile in -a -a2 -c -c2 -c3 -f -p -s -u; do
	"$work/tabs" -T zed "$profile" >"$work/tabs-profile.out"
	test -s "$work/tabs-profile.out"
done
if "$work/tabs" -T zed '9,8' >/dev/null 2>&1; then
	echo 'tabs accepted non-increasing tab stops' >&2
	exit 1
fi
if "$work/tabs" -T unknown >/dev/null 2>&1; then
	echo 'tabs accepted an unknown terminal' >&2
	exit 1
fi
"$work/expander-test"

mkdir "$work/bad"
printf 'ZEDTERM 1\nstr:clear=\\x0\n' >"$work/bad/bad.zti"
if TERMINFO="$work/bad" "$work/tput" -T bad clear >/dev/null 2>&1; then
	echo 'tput accepted a malformed terminal description' >&2
	exit 1
fi

echo 'zedBSD POSIX tabs/tput host tests: PASS'
