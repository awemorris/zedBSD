#!/usr/bin/env bash
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
# Exercise the Phase 10 zedBSD-local m4 replacement.
set -euo pipefail

repo="$(cd "$(dirname "$0")/.." && pwd)"
work="$(mktemp -d "${TMPDIR:-/tmp}/zedbsd-posix-m4.XXXXXX")"
trap 'rm -rf "$work"' EXIT

cc -std=c11 -D_DEFAULT_SOURCE -O2 -Wall -Wextra -Werror \
	-I"$repo" "$repo/userland/base/common/command.c" \
	"$repo/userland/base/m4/engine.c" \
	"$repo/userland/base/m4/main.c" -o "$work/m4"

printf 'included\n' >"$work/included.m4"
cat >"$work/input.m4" <<EOF
define(\`twice', \`\$1\$1')dnl
twice(\`ab')
define(\`first', \`second')define(\`second', \`recursive')dnl
first
eval(2+3*4)
substr(\`abcdef',2,3)
index(\`abcdef',\`de')
translit(\`aabbcc',\`ac',\`xz')
ifelse(\`same',\`same',\`yes',\`no')
include(\`$work/included.m4')dnl
divert(1)dnl
deferred
divert(0)dnl
immediate
undivert(1)dnl
EOF
cat >"$work/expected" <<'EOF'
abab
recursive
14
cde
3
xxbbzz
yes
included
immediate
deferred
EOF
"$work/m4" "$work/input.m4" >"$work/output"
cmp "$work/expected" "$work/output"

if "$work/m4" -Z </dev/null >"$work/bad-option.out" 2>&1; then
	echo 'm4 accepted an unknown option' >&2
	exit 1
fi
if printf 'eval(1/0)\n' | "$work/m4" >"$work/divzero.out" 2>&1; then
	echo 'm4 accepted division by zero' >&2
	exit 1
fi
grep -q 'division by zero' "$work/divzero.out"

if printf 'm4exit(0)\n' | "$work/m4" >"$work/unsupported.out" 2>&1; then
	echo 'm4 accepted an unsupported m4exit builtin' >&2
	exit 1
fi
grep -q 'not implemented locally' "$work/unsupported.out"

echo 'zedBSD Phase 10 local m4 host tests: PASS'
