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

make -C "$repo" ARCH="$arch" "build/$arch/fdd-ipl.bin" "build/$arch/IO.SYS" "build/$arch/vmunix"

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
mcopy -i "$output" "$repo/apps/menu.nct" ::AUTOEXEC.NCT
mmd -i "$output" ::APPS 2>/dev/null || true
mmd -i "$output" ::HOME 2>/dev/null || true
if [ -f "$repo/apps/hello.nct" ]; then
	mcopy -i "$output" "$repo/apps/hello.nct" ::APPS/HELLO.NCT
fi
for utility in ls.nct cp.nct; do
	if [ -f "$repo/apps/$utility" ]; then
		mcopy -i "$output" "$repo/apps/$utility" ::APPS/"${utility^^}"
	fi
done

# Remacs and its SKK dictionary make the floppy a self-contained
# pre-boot editing environment.
remacs_nap="${REMACS_NAP:-$repo/build/remacs/REMACS.NAP}"
remacs_skk="${REMACS_SKK_DICT:-$repo/build/remacs/SKKJISYO.DIC}"
if [ ! -s "$remacs_nap" ]; then
	"$repo/scripts/build-remacs-bytecode.sh"
fi
test -s "$remacs_nap" || {
	echo "Remacs bytecode not found: $remacs_nap" >&2
	exit 1
}
mcopy -i "$output" "$remacs_nap" ::APPS/REMACS.NAP
mcopy -i "$output" "$remacs_skk" ::HOME/SKKJISYO.DIC

sha256sum "$output"
printf 'zedBSD FDD image: %s\n' "$output"
