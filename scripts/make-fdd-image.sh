#!/usr/bin/env bash

set -euo pipefail

repo="$(cd "$(dirname "$0")/.." && pwd)"
arch="${ZEDBSD_ARCH:-pc98}"
build="${ZEDBSD_BUILD_DIR:-$repo/build/$arch}"
output="${1:-$build/fdd-test.img}"

if [ -e "$output" ]; then
	echo "Refusing to overwrite existing image: $output" >&2
	exit 1
fi

for command in mformat mcopy mattrib mmd python3; do
	command -v "$command" >/dev/null || {
		echo "$command is required" >&2
		exit 1
	}
done

"$repo/build.sh" "build/$arch/fdd-ipl.bin" "$arch" \
	"build/$arch/IO.SYS" "build/$arch/vmunix" \
	"build/$arch/bin/sh" "build/$arch/bin/noct"

# Stage 1 lives raw in the reserved sectors 3..16.
io_sys_size="$(stat -c %s "$build/IO.SYS")"
test "$io_sys_size" -le $((14 * 512)) || {
	echo "IO.SYS does not fit the reserved floppy sectors" >&2
	exit 1
}

mkdir -p "$(dirname "$output")"
truncate -s 1440K "$output"

# FAT12-format the disk with 16 reserved sectors: the boot sector plus
# room for Stage 1.
mformat -i "$output" -f 1440 -R 16 ::

# Merge the IPL code around the BPB that mformat just wrote.
python3 - "$output" "$build/fdd-ipl.bin" <<'PY'
import sys

image_path, ipl_path = sys.argv[1], sys.argv[2]
with open(ipl_path, "rb") as source:
    ipl = source.read(512)
with open(image_path, "r+b") as image:
    boot = bytearray(image.read(512))
    boot[0:3] = ipl[0:3]              # jump and nop
    boot[3:11] = ipl[3:11]            # OEM name / IPL tag
    boot[0x3e:0x200] = ipl[0x3e:0x200]  # loader code and signature
    image.seek(0)
    image.write(boot)
PY

dd if="$build/IO.SYS" of="$output" bs=512 seek=2 conv=notrunc status=none

mcopy -i "$output" "$build/vmunix" ::vmunix
mattrib -i "$output" +r +h +s ::vmunix
mmd -i "$output" ::BIN 2>/dev/null || true
mcopy -i "$output" "$build/bin/sh" ::BIN/SH
mcopy -i "$output" "$build/bin/noct" ::BIN/NOCT
mmd -i "$output" ::APPS 2>/dev/null || true
holoris="$repo/userland/noct/noct-upstream/apps/holoris/holoris.noct"
test -s "$holoris" || {
	echo "Holoris Noct source not found: $holoris" >&2
	exit 1
}
mcopy -i "$output" "$holoris" ::APPS/HOLORIS.NCT
if [ -f "$repo/apps/hello.nct" ]; then
	mcopy -i "$output" "$repo/apps/hello.nct" ::APPS/HELLO.NCT
fi
for utility in ls.nct cp.nct; do
	if [ -f "$repo/apps/$utility" ]; then
		mcopy -i "$output" "$repo/apps/$utility" ::APPS/"${utility^^}"
	fi
done

sha256sum "$output"
printf 'zedBSD FDD image: %s\n' "$output"
