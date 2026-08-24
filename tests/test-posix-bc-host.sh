#!/usr/bin/env bash
# SPDX-License-Identifier: Zlib
# POSIX-UTILITY-TEST: bc arbitrary precision grammar functions arrays failures
set -euo pipefail

repo="$(cd "$(dirname "$0")/.." && pwd)"
work="$(mktemp -d "${TMPDIR:-/tmp}/zedbsd-posix-bc.XXXXXX")"
trap 'rm -rf "$work"' EXIT

sources=(
	args bc bc_lex bc_parse data file lang lex main num opt parse program
	rand read vector vm
)
objects=()
for source in "${sources[@]}"; do
	objects+=("$repo/userland/base/bc/src/$source.c")
done
cc -std=c11 -D_POSIX_C_SOURCE=200809L -D_XOPEN_SOURCE=700 \
	-O2 -Wall -Wextra -Werror -Wno-clobbered \
	"${objects[@]}" "$repo/userland/base/bc/gen/bc_help.c" \
	"$repo/userland/base/bc/gen/lib.c" \
	"$repo/userland/base/bc/gen/lib2.c" -o "$work/bc"

cat >"$work/program.bc" <<'EOF'
999999999999999999999999999999*999999999999999999999999999999
scale=5
1/8
define fact(n) {
  auto result, i
  result=1
  for (i=2; i<=n; i++) result*=i
  return (result)
}
fact(30)
a[0]=7
a[1]=9
a[0]+a[1]
ibase=16
FF
ibase=A
obase=16
255
EOF
cat >"$work/expected" <<'EOF'
999999999999999999999999999998000000000000000000000000000001
.12500
265252859812191058636308480000000
16
255
FF
EOF
"$work/bc" -q "$work/program.bc" >"$work/output"
cmp "$work/expected" "$work/output"

test "$(printf 'scale=8; s(0)\n' | "$work/bc" -lq)" = '0'

if printf '1/0\n' | "$work/bc" -q >"$work/divzero" 2>&1; then
	echo 'bc accepted division by zero' >&2
	exit 1
fi
grep -qi 'divide by 0\|division by zero' "$work/divzero"
if printf 'define broken( {\n' | "$work/bc" -q >"$work/syntax" 2>&1; then
	echo 'bc accepted invalid syntax' >&2
	exit 1
fi
grep -qi 'parse error\|syntax error' "$work/syntax"

echo 'zedBSD POSIX bc host tests: PASS'
