#!/usr/bin/env bash
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
# POSIX-UTILITY-TEST: m4 positive negative grammar recursive expansion
set -euo pipefail

repo="$(cd "$(dirname "$0")/.." && pwd)"
work="$(mktemp -d "${TMPDIR:-/tmp}/zedbsd-posix-m4.XXXXXX")"
trap 'rm -rf "$work"' EXIT

cc -std=c11 -D_DEFAULT_SOURCE -O2 -Wall -Wextra -Werror \
	-include "$repo/tests/m4-host-compat.h" \
	-I"$repo/userland/base/m4" \
	"$repo/userland/base/m4/eval.c" \
	"$repo/userland/base/m4/expr.c" \
	"$repo/userland/base/m4/gnum4.c" \
	"$repo/userland/base/m4/look.c" \
	"$repo/userland/base/m4/main.c" \
	"$repo/userland/base/m4/misc.c" \
	"$repo/userland/base/m4/ohash.c" \
	"$repo/userland/base/m4/parser.c" \
	"$repo/userland/base/m4/tokenizer.c" \
	"$repo/userland/base/m4/trace.c" \
	"$repo/tests/m4-host-compat.c" -o "$work/m4"

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

echo 'zedBSD POSIX m4 host tests: PASS'
