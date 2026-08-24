#!/bin/sh
set -eu

repo=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
work=$(mktemp -d "${TMPDIR:-/tmp}/zedbsd-phase7.XXXXXX")
trap 'rm -rf "$work"' EXIT HUP INT TERM

cc -std=c11 -D_GNU_SOURCE -Wall -Wextra -Werror -I"$repo" \
	"$repo/userland/base/compress/main.c" \
	"$repo/userland/base/common/lzw.c" -o "$work/compress"
ln -s compress "$work/uncompress"
ln -s compress "$work/zcat"

python3 - "$repo/tests/fixtures/phase7-ABC.Z.hex" "$work/ABC.Z" <<'PY'
from pathlib import Path
import sys
Path(sys.argv[2]).write_bytes(bytes.fromhex(Path(sys.argv[1]).read_text().strip()))
PY
printf ABC >"$work/ABC.expected"
"$work/zcat" "$work/ABC.Z" >"$work/ABC.actual"
cmp "$work/ABC.expected" "$work/ABC.actual"

python3 - "$work/input" <<'PY'
from pathlib import Path
import sys
data = bytearray()
for i in range(20000):
    data.extend(f"line {i % 997:03d}: the quick brown fox jumps over the lazy dog\n".encode())
Path(sys.argv[1]).write_bytes(data)
PY
"$work/compress" -cf "$work/input" >"$work/input.Z"
"$work/uncompress" -c "$work/input.Z" >"$work/output"
cmp "$work/input" "$work/output"

printf 'not a Z stream' >"$work/bad.Z"
if "$work/uncompress" -c "$work/bad.Z" >"$work/bad.out" 2>/dev/null; then
	exit 1
fi

cp "$work/input" "$work/replaced"
"$work/compress" -f "$work/replaced"
test ! -e "$work/replaced"
test -e "$work/replaced.Z"
"$work/uncompress" -f "$work/replaced.Z"
cmp "$work/input" "$work/replaced"

printf '%s\n' 'zedBSD POSIX Phase 7 compression host test: PASS'
