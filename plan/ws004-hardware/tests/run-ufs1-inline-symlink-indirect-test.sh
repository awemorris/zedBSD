#!/usr/bin/env bash
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -euo pipefail

repo=$(cd "$(dirname "$0")/../../.." && pwd)
temporary=$(mktemp -d "${TMPDIR:-/tmp}/zedbsd-ufs1-symlink-indirect.XXXXXX")
trap 'rm -rf -- "$temporary"' EXIT

for command_name in cc mkdir mktemp python3; do
	command -v "$command_name" >/dev/null || {
		echo "missing host command: $command_name" >&2
		exit 1
	}
done

root=$temporary/root
image=$temporary/root.ufs
backend=$temporary/zedimage-host
mkdir -p "$root/etc" "$root/lib/firmware/rtw88" "$root/run" "$root/var"
printf 'zedBSD ufs1 root v1\n' >"$root/etc/zedbsd-root"
ln -s ../run "$root/var/run"

# Twenty full filesystem blocks plus a tail forces the UFS1 single-indirect
# path while remaining close to the size of the RTL8822B firmware payload.
python3 - "$root/lib/firmware/rtw88/large.bin" <<'PY'
import pathlib
import sys

path = pathlib.Path(sys.argv[1])
path.write_bytes(bytes((index * 37 + 11) & 0xff
                       for index in range(20 * 8192 + 317)))
PY

cc -std=c11 -O2 -Wall -Wextra -Werror \
	"$repo/tools/build/zedimage-host.c" -o "$backend"
"$backend" ufs $((16 * 1024 * 1024)) "$root" "$image"
PYTHONPATH="$repo/tools/build" python3 \
	"$repo/tools/build/check-ufs1-image.py" "$image"

PYTHONPATH="$repo/tools/build" python3 - "$root" "$image" <<'PY'
import hashlib
import pathlib
import sys

from check_ufs1_import import load_checker

root = pathlib.Path(sys.argv[1])
image = pathlib.Path(sys.argv[2])
checker = load_checker()
fs = checker.UFS1(image.read_bytes())

link_inode = fs.lookup('/var/run')
assert fs.read_file(link_inode) == b'../run'
assert fs.blocks(link_inode) == []

payload = root / 'lib/firmware/rtw88/large.bin'
payload_inode = fs.lookup('/lib/firmware/rtw88/large.bin')
raw_inode = fs.inode(payload_inode)
assert fs.u64(raw_inode, 8) == payload.stat().st_size
assert all(fs.u32(raw_inode, 40 + index * 4) for index in range(12))
assert fs.u32(raw_inode, 88) != 0
assert fs.u32(raw_inode, 92) == 0
assert fs.u32(raw_inode, 96) == 0
assert len(fs.blocks(payload_inode)) == 21
assert hashlib.sha256(fs.read_file(payload_inode)).digest() == \
       hashlib.sha256(payload.read_bytes()).digest()
PY

echo 'UFS1 inline symlink and single-indirect image: PASS'
