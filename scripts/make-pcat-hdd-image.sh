#!/usr/bin/env bash
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -euo pipefail

if test "$#" -ne 4; then
	echo "usage: $0 OUTPUT BOOTSECT VMUNIX SH" >&2
	exit 2
fi
output="$1"
bootsect="$2"
vmunix="$3"
shell="$4"
start_lba=65536
partition_mib=128
partition_sectors=$((partition_mib * 1024 * 1024 / 512))
offset=$((start_lba * 512))
total_bytes=$((offset + partition_mib * 1024 * 1024))

test -f "$bootsect"
test -f "$vmunix"
test -f "$shell"
mkdir -p "$(dirname "$output")"
rm -f "$output"
truncate -s "$total_bytes" "$output"

python3 - "$output" "$bootsect" "$vmunix" "$start_lba" "$partition_sectors" <<'PY'
import struct, sys
path, boot_path, kernel_path = sys.argv[1:4]
start, count = int(sys.argv[4]), int(sys.argv[5])
mbr = bytearray(open(boot_path, 'rb').read())
kernel = open(kernel_path, 'rb').read()
if len(mbr) != 512 or mbr[510:512] != b'\x55\xaa':
    raise SystemExit('invalid PC/AT boot sector')
if len(kernel) > (start - 1) * 512:
    raise SystemExit('vmunix exceeds raw kernel area')
# Active FAT16 LBA primary partition. CHS values are conventional maxima;
# zedBSD and the loader use LBA.
mbr[0x1be:0x1ce] = struct.pack('<BBBBBBBBII',
    0x80, 0xfe, 0xff, 0xff, 0x0e, 0xfe, 0xff, 0xff, start, count)
mbr[510:512] = b'\x55\xaa'
with open(path, 'r+b') as image:
    image.write(mbr)
    image.seek(512)
    image.write(kernel)
PY

mformat -i "$output@@$offset" -v BOOT ::
mmd -i "$output@@$offset" ::/bin
mcopy -i "$output@@$offset" "$shell" ::/bin/sh

# Keep vmunix in the FAT image too so the same disk can later be used by a
# GRUB partition install, although the custom MBR loader uses the raw copy.
mmd -i "$output@@$offset" ::/boot
mcopy -i "$output@@$offset" "$vmunix" ::/boot/vmunix
echo "PC/AT HDD image: $output (${total_bytes} bytes, FAT16 at LBA $start_lba)"
