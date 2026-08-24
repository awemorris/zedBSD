#!/usr/bin/env bash
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
# POSIX-UTILITY-TEST: ed positive negative
set -euo pipefail

repo="$(cd "$(dirname "$0")/.." && pwd)"
work="$(mktemp -d "${TMPDIR:-/tmp}/zedbsd-posix-ed.XXXXXX")"
trap 'rm -rf "$work"' EXIT

cc -std=c11 -D_DEFAULT_SOURCE -O2 -Wall -Wextra -Werror \
	-I"$repo/userland/base/ed" "$repo/userland/base/ed/buf.c" \
	"$repo/userland/base/ed/glbl.c" "$repo/userland/base/ed/io.c" \
	"$repo/userland/base/ed/main.c" "$repo/userland/base/ed/re.c" \
	"$repo/userland/base/ed/sub.c" "$repo/userland/base/ed/undo.c" \
	-o "$work/ed"

cat >"$work/script" <<EOF
a
alpha
beta
gamma
.
1,\$p
g/a/s/a/A/g
1,\$p
u
1,\$p
w $work/output
Q
EOF
"$work/ed" -s <"$work/script" >"$work/result"
cat >"$work/expected" <<'EOF'
alpha
beta
gamma
AlphA
betA
gAmmA
alpha
beta
gamma
EOF
cmp "$work/result" "$work/expected"
test "$(cat "$work/output")" = $'alpha\nbeta\ngamma'

if "$work/ed" -Z >/dev/null 2>&1; then
	echo 'ed accepted an unknown option' >&2
	exit 1
fi
printf 'H\na\none\n.\ns/[invalid/x/\nQ\n' | "$work/ed" -s \
	>"$work/regex-error" 2>&1 || true
grep -q 'unbalanced brackets' "$work/regex-error"

echo 'zedBSD POSIX ed host tests: PASS'
