#!/usr/bin/env bash
# SPDX-License-Identifier: Zlib
# Exercise the Phase 10 zedBSD-local bc replacement and its explicit gaps.
set -euo pipefail

repo="$(cd "$(dirname "$0")/.." && pwd)"
work="$(mktemp -d "${TMPDIR:-/tmp}/zedbsd-posix-bc.XXXXXX")"
trap 'rm -rf "$work"' EXIT

cc -std=c11 -D_POSIX_C_SOURCE=200809L -D_XOPEN_SOURCE=700 \
	-D_DEFAULT_SOURCE -O2 -Wall -Wextra -Werror -I"$repo" \
	"$repo/userland/base/common/command.c" \
	"$repo/userland/base/bc/number.c" \
	"$repo/userland/base/bc/main.c" -o "$work/bc"

cat >"$work/program.bc" <<'EOF'
999999999999999999999999999999*999999999999999999999999999999
scale=5
EOF
if "$work/bc" "$work/program.bc" >"$work/unsupported" 2>&1; then
	echo 'bc accepted unsupported non-zero scale' >&2
	exit 1
fi
grep -q 'non-zero scale is not implemented locally' "$work/unsupported"

cat >"$work/program.bc" <<'EOF'
999999999999999999999999999999*999999999999999999999999999999
scale=0
a=7
a^20
100/7
100%7
-12+5
EOF
cat >"$work/expected" <<'EOF'
999999999999999999999999999998000000000000000000000000000001
79792266297612001
14
2
-7
EOF
"$work/bc" "$work/program.bc" >"$work/output"
cmp "$work/expected" "$work/output"

if printf '1/0\n' | "$work/bc" >"$work/divzero" 2>&1; then
	echo 'bc accepted division by zero' >&2
	exit 1
fi
grep -qi 'division by zero' "$work/divzero"
if printf 'define broken( {\n' | "$work/bc" >"$work/syntax" 2>&1; then
	echo 'bc accepted invalid syntax' >&2
	exit 1
fi
grep -qi 'unexpected token\|expected number' "$work/syntax"
if "$work/bc" -l </dev/null >"$work/math" 2>&1; then
	echo 'bc accepted the unavailable math library' >&2
	exit 1
fi
grep -q 'not implemented locally' "$work/math"

echo 'zedBSD Phase 10 local bc host tests: PASS'
