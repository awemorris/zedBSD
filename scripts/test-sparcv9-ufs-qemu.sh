#!/usr/bin/env bash
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -euo pipefail

repo="$(cd "$(dirname "$0")/.." && pwd)"
source_disk="$repo/build/sparcv9/ufs-root-hdd-image.img"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

command -v qemu-system-sparc64 >/dev/null || {
	echo "qemu-system-sparc64 missing" >&2
	exit 1
}
test -f "$source_disk" || {
	echo "missing SPARC V9 UFS1 image: $source_disk" >&2
	exit 1
}
cp --reflink=auto "$source_disk" "$work/disk.img"
set +e
timeout 18s qemu-system-sparc64 -M sun4u -m 128M \
	-drive "file=$work/disk.img,format=raw,if=ide" \
	-nographic -no-reboot </dev/null >"$work/qemu.log" 2>&1
status=$?
set -e
if test "$status" -ne 0 && test "$status" -ne 124; then
	cat "$work/qemu.log" >&2
	exit "$status"
fi
tr -d '\r' <"$work/qemu.log" >"$work/qemu.clean.log"
grep -q 'root=ufs1' "$work/qemu.clean.log"
grep -q 'boot: starting init /bin/sh' "$work/qemu.clean.log"
grep -q '/ \$' "$work/qemu.clean.log"
if grep -q 'init not started' "$work/qemu.clean.log" || \
    grep -q '^fatal:' "$work/qemu.clean.log"; then
	cat "$work/qemu.clean.log" >&2
	exit 1
fi
echo "SPARC V9 sun4u UFS1 root/user64 test: PASS"
