#!/usr/bin/env bash
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
# Exercise the Phase 10 zedBSD-local ed replacement and its failure paths.
set -euo pipefail

repo="$(cd "$(dirname "$0")/.." && pwd)"
work="$(mktemp -d "${TMPDIR:-/tmp}/zedbsd-posix-ed.XXXXXX")"
trap 'rm -rf "$work"' EXIT

cc -std=c11 -D_DEFAULT_SOURCE -O2 -Wall -Wextra -Werror \
	-I"$repo" "$repo/userland/base/common/command.c" \
	"$repo/userland/base/ed/buffer.c" \
	"$repo/userland/base/ed/main.c" \
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
	>"$work/regex-error" 2>&1 && {
	echo 'ed returned success after an invalid BRE' >&2
	exit 1
}
grep -q '^?' "$work/regex-error"

printf 'a\nmodified\n.\nq\nQ\n' | "$work/ed" -s \
	>"$work/modified-error" 2>&1 && {
	echo 'ed returned success after refusing to discard a modified buffer' >&2
	exit 1
}
grep -q '^?' "$work/modified-error"

echo 'zedBSD Phase 10 local ed host tests: PASS'
