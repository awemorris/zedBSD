#!/usr/bin/env bash
set -euo pipefail

repo="$(cd "$(dirname "$0")/.." && pwd)"
arch="${ZEDBSD_ARCH:-pc98}"
build="${ZEDBSD_BUILD_DIR:-$repo/build/$arch}"
output="${1:-$build/hdd-test.img}"
heads="${DISK_HEADS:-}"
sectors="${DISK_SECTORS:-17}"

test ! -e "$output" || {
	echo "Refusing to overwrite existing image: $output" >&2
	exit 1
}
mkdir -p "$(dirname "$output")"
truncate -s "${ZEDBSD_TEST_MB:-16}M" "$output"
if test -z "$heads"; then
	image_bytes="$(stat -c %s "$output")"
	# Only the legacy 20 MiB disk class uses four-head geometry.
	if test "$image_bytes" -le $((20 * 1024 * 1024)); then
		heads=4
	else
		heads=8
	fi
fi

# Write one NEC PC-98 partition-table entry.  The table lives in the
# second physical sector (LBA 1) and holds sixteen 32-byte entries:
#   byte  0     MID  (0xA1: active/bootable bit 7 + DOS FAT medium)
#   byte  1     SID  (0x91: bootable bit 7 + type 0x11, PC-98 DOS FAT16)
#   bytes 4-7   IPL address as sector, head, uint16 cylinder
#   bytes 8-11  data start address in the same CHS form
#   bytes 12-15 end address in the same CHS form
#   bytes 16-31 partition name, space-padded
# The partition spans from the second cylinder to the end of the disk;
# keeping IPL == data start makes the PBR also DOS logical sector zero.
python3 - "$output" "$heads" "$sectors" <<'PY'
import os
import struct
import sys

image, heads, sectors = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
physical_sectors = os.path.getsize(image) // 512
last_lba = physical_sectors - 1

def chs(lba):
    cylinder, rem = divmod(lba, heads * sectors)
    head, sector = divmod(rem, sectors)
    return bytes((sector, head)) + struct.pack("<H", cylinder)

entry = bytearray(32)
entry[0] = 0xA1
entry[1] = 0x91
entry[4:8] = chs(heads * sectors)
entry[8:12] = chs(heads * sectors)
entry[12:16] = chs(last_lba)
entry[16:32] = b"BOOT".ljust(16, b" ")
with open(image, "r+b") as stream:
    stream.seek(512)
    stream.write(entry)
PY

# Without a startup configuration the menu has no automatic target:
# the only partition is the BOOT volume itself, which is deliberately
# excluded from chain boot.  Ship the graphical AUTOEXEC.NCT menu so
# the automatic countdown has somewhere to go.
if test "${ZEDBSD_AUTOEXEC_DISABLE:-0}" = 1; then
	autoexec=""
else
	autoexec="${ZEDBSD_AUTOEXEC:-$repo/apps/AUTOEXEC.NCT}"
fi
BOOT_LOGO="${ZEDBSD_LOGO:-}" \
ZEDBSD_FILES="${ZEDBSD_FILES:-}" \
ZEDBSD_AUTOEXEC="$autoexec" \
DISK_HEADS="$heads" DISK_SECTORS="$sectors" \
	"$repo/scripts/install-image.sh" --partition 1 \
	--install-disk-stubs "$output" "${ZEDBSD_KERNEL:-}" "${ZEDBSD_CFG:-}"

sha256sum "$output"
printf 'zedBSD HDD test image: %s\n' "$output"
