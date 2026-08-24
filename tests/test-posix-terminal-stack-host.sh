#!/usr/bin/env bash
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -euo pipefail

repo="$(cd "$(dirname "$0")/.." && pwd)"
work="$(mktemp -d "${TMPDIR:-/tmp}/zedbsd-terminal-stack.XXXXXX")"
trap 'rm -rf "$work"' EXIT

common=(-std=c11 -D_DEFAULT_SOURCE -O2 -Wall -Wextra -Werror -I"$repo")
cc "${common[@]}" "$repo/userland/base/tic/main.c" \
	"$repo/userland/base/common/terminfo.c" -o "$work/tic"
cc "${common[@]}" "$repo/userland/base/infocmp/main.c" \
	"$repo/userland/base/common/terminfo.c" -o "$work/infocmp"
cc "${common[@]}" "$repo/userland/base/tput/main.c" \
	"$repo/userland/base/common/terminfo.c" \
	"$repo/userland/base/common/command.c" -o "$work/tput"
cc "${common[@]}" "$repo/tests/posix-phase85-curses.c" \
	"$repo/userland/base/curses/curses.c" \
	"$repo/userland/base/common/terminfo.c" -o "$work/curses-test"

"$work/tic" -o "$work/db" "$repo/tests/fixtures/phase85-terminal.ti"
test -f "$work/db/phase85.zti"
test -f "$work/db/phase85-test.zti"
"$work/infocmp" -A "$work/db" phase85 >"$work/phase85.out"
grep -q '^phase85|zedBSD Phase 8.5 test terminal,' "$work/phase85.out"
grep -q $'^\tcols#90,' "$work/phase85.out"
test "$(TERMINFO="$work/db" "$work/tput" -T phase85 cols)" = 90
TERM=phase85 TERMINFO="$work/db" "$work/curses-test" >"$work/curses.out"
grep -q 'zedBSD-POSIX-PHASE85-CURSES-PASS' "$work/curses.out"

printf 'broken|entry, use=xterm,\n' >"$work/inherited.ti"
if "$work/tic" -o "$work/bad-db" "$work/inherited.ti" >/dev/null 2>&1; then
	echo 'tic silently accepted unsupported use= inheritance' >&2
	exit 1
fi
if "$work/infocmp" -A "$work/db" missing >/dev/null 2>&1; then
	echo 'infocmp accepted a missing terminal' >&2
	exit 1
fi

while IFS= read -r makefile; do
	directory="${makefile%/Makefile}"
	grep -q 'package.mk' "$makefile"
	make -s -C "$directory" all
done < <(find "$repo/userland/base" -mindepth 2 -maxdepth 2 \
	-name Makefile | sort)

normal="$work/normal"
root="$work/root"
"${MAKE:-make}" -s -C "$repo/userland/base/terminfo" install \
	DESTDIR="$normal" PREFIX=/opt/zed
test -f "$normal/opt/zed/share/terminfo/xterm.zti"
test ! -e "$normal/lib/terminfo/xterm.zti"
"${MAKE:-make}" -s -C "$repo/userland/base/terminfo" install \
	DESTDIR="$root" PREFIX=/
test -f "$root/lib/terminfo/xterm.zti"
test ! -e "$root/share/terminfo/xterm.zti"
"${MAKE:-make}" -s -C "$repo/userland/base/tic" install \
	DESTDIR="$normal" PREFIX=/opt/zed
test -x "$normal/opt/zed/bin/tic"
"${MAKE:-make}" -s -C "$repo/userland/base/tic" install \
	DESTDIR="$root" PREFIX=/
test -x "$root/bin/tic"
"${MAKE:-make}" -s -C "$repo/userland/base/curses" install \
	DESTDIR="$normal" PREFIX=/opt/zed
test -f "$normal/opt/zed/lib/libcurses.a"
test -f "$normal/opt/zed/include/curses.h"
"${MAKE:-make}" -s -C "$repo/userland/base/curses" install \
	DESTDIR="$root" PREFIX=/
test -f "$root/lib/libcurses.a"
test -f "$root/include/curses.h"

echo 'zedBSD terminal packages and standalone base builds: PASS'
