#!/usr/bin/env bash
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
# POSIX-UTILITY-TEST: pax write read list append copy links unsafe paths
set -euo pipefail

repo="$(cd "$(dirname "$0")/.." && pwd)"
work="$(mktemp -d "${TMPDIR:-/tmp}/zedbsd-posix-pax.XXXXXX")"
trap 'rm -rf "$work"' EXIT

cc -std=c11 -D_DEFAULT_SOURCE -O2 -Wall -Wextra -Werror \
	"$repo/userland/base/pax/main.c" -o "$work/pax"

mkdir -p "$work/source/sub" "$work/extract" "$work/copy"
printf 'alpha\n' >"$work/source/a"
printf 'beta payload\n' >"$work/source/sub/b"
ln "$work/source/a" "$work/source/hard"
ln -s sub/b "$work/source/link"

(
	cd "$work"
	./pax -w -f archive.pax source
)
(
	cd "$work"
	./pax -f archive.pax
) >"$work/list"
sort -o "$work/list" "$work/list"
cat >"$work/expected-list" <<'EOF'
source
source/a
source/hard
source/link
source/sub
source/sub/b
EOF
cmp "$work/expected-list" "$work/list"

(
	cd "$work/extract"
	../pax -r -f ../archive.pax
)
cmp "$work/source/a" "$work/extract/source/a"
cmp "$work/source/sub/b" "$work/extract/source/sub/b"
test "$(readlink "$work/extract/source/link")" = 'sub/b'
test "$(stat -c %i "$work/extract/source/a")" = \
	"$(stat -c %i "$work/extract/source/hard")"

printf 'appended\n' >"$work/extra"
(
	cd "$work"
	./pax -wa -f archive.pax extra
	./pax -f archive.pax extra
) >"$work/append-list"
test "$(cat "$work/append-list")" = 'extra'

(
	cd "$work"
	./pax -rw source copy
)
cmp "$work/source/sub/b" "$work/copy/source/sub/b"

(
	cd "$work"
	./pax -s ',^source,renamed,' -f archive.pax 'renamed/sub/b'
) >"$work/substitution"
test "$(cat "$work/substitution")" = 'renamed/sub/b'

python3 - "$work/unsafe.pax" <<'PY'
import io
import tarfile
import sys
with tarfile.open(sys.argv[1], "w", format=tarfile.USTAR_FORMAT) as archive:
    info = tarfile.TarInfo("../escaped")
    payload = b"unsafe\n"
    info.size = len(payload)
    archive.addfile(info, io.BytesIO(payload))
PY
if (
	cd "$work/extract"
	../pax -r -f ../unsafe.pax
); then
	echo 'pax accepted an unsafe extraction path' >&2
	exit 1
fi
test ! -e "$work/escaped"

cp "$work/archive.pax" "$work/corrupt.pax"
printf X | dd of="$work/corrupt.pax" bs=1 seek=0 conv=notrunc status=none
if "$work/pax" -f "$work/corrupt.pax" >/dev/null 2>&1; then
	echo 'pax accepted a corrupt header' >&2
	exit 1
fi

echo 'zedBSD POSIX pax host tests: PASS'
