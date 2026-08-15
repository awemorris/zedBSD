#!/usr/bin/env bash
set -euo pipefail

repo="$(cd "$(dirname "$0")/.." && pwd)"
arch="${ZEDBSD_ARCH:-pc98}"
build="${ZEDBSD_BUILD_DIR:-$repo/build/$arch}"
vmunix_image="${ZEDBSD_VMUNIX_IMAGE:-$build/vmunix}"
shell_image="${ZEDBSD_SH_IMAGE:-$build/bin/sh}"
# Optional test/image customization hook.  Standard zedBSD images leave this
# empty and therefore enter /bin/sh without an automatic startup script.
zinit_rc="${ZEDBSD_ZINIT_RC:-}"
arch_profile="${ZEDBSD_ARCH_PROFILE:-}"
arch_image="${ZEDBSD_ARCH_IMAGE:-}"
partition="${BOOT_PARTITION:-0}"
install_disk_stubs="${INSTALL_DISK_STUBS:-0}"
while test "$#" -gt 0; do
	case "$1" in
		--partition)
			test "$#" -ge 2 || { echo "Missing value for --partition" >&2; exit 2; }
			partition="$2"
			shift 2
			;;
		--install-disk-stubs)
			install_disk_stubs=1
			shift
			;;
		*) break ;;
	esac
done
image="${1:?usage: $0 [--partition N] [--install-disk-stubs] IMAGE [VMLINUX [BOOT.CFG]]}"
kernel="${2:-}"
boot_cfg="${3:-}"
heads="${DISK_HEADS:-8}"
source_heads="${DISK_SOURCE_HEADS:-}"
sectors="${DISK_SECTORS:-17}"
swap_size_mib="${ZEDBSD_SWAP_SIZE_MIB:-0}"
swap_temp=""
cleanup()
{
	if test -n "$swap_temp"; then
		rm -f -- "$swap_temp"
	fi
}
trap cleanup EXIT INT TERM

test -f "$image" || { echo "Image not found: $image" >&2; exit 1; }
test -z "$kernel" || test -f "$kernel" || {
	echo "Kernel not found: $kernel" >&2
	exit 1
}
case "$heads:$sectors" in
	*[!0-9:]* | 0:* | *:0) echo "Invalid geometry: H=$heads S=$sectors" >&2; exit 2 ;;
esac
case "$source_heads" in
	'' | 4 | 8) ;;
	*) echo "DISK_SOURCE_HEADS must be empty, 4, or 8" >&2; exit 2 ;;
esac
case "$partition" in
	'' | *[!0-9]* | 0) test "$partition" = 0 || { echo "Invalid partition: $partition" >&2; exit 2; } ;;
	*) test "$partition" -le 16 || { echo "Invalid partition: $partition" >&2; exit 2; } ;;
esac
case "$install_disk_stubs" in
	0 | 1) ;;
	*) echo "INSTALL_DISK_STUBS must be 0 or 1" >&2; exit 2 ;;
esac
case "$swap_size_mib" in
	0 | 32 | 64) ;;
	*) echo "ZEDBSD_SWAP_SIZE_MIB must be 0, 32, or 64" >&2; exit 2 ;;
esac
for command in dd mattrib mcopy mdir mformat mmd python3; do
	command -v "$command" >/dev/null || { echo "$command is required" >&2; exit 1; }
done

ensure_directory()
{
	local directory="$1"
	if ! mdir -i "$image@@$offset" "$directory" >/dev/null 2>&1; then
		mmd -i "$image@@$offset" "$directory"
	fi
}

"$repo/build.sh" all "$arch"
test -f "$vmunix_image" || {
	echo "vmunix image not found: $vmunix_image" >&2
	exit 1
}
io_sys_size="$(stat -c %s "$build/IO.SYS")"

# Recreate the first FAT16 partition as the BOOT environment.  The volume has
# one 1024-byte reserved logical sector containing the PBR.  IO.SYS is copied
# first as a normal contiguous FAT file, matching the historical DOS SYS-file
# layout.  Root and swap partitions remain untouched.
layout="$(python3 - "$image" "$heads" "$sectors" "$partition" \
	"$source_heads" <<'PY'
import os
import struct
import sys

image, heads, sectors, selected, source_heads_arg = (
    sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), int(sys.argv[4]),
    sys.argv[5])

def lba(raw, geometry_heads):
    sector, head = raw[0], raw[1]
    cylinder = struct.unpack_from("<H", raw, 2)[0]
    return (cylinder * geometry_heads + head) * sectors + sector

def chs(cylinder, head=0, sector=0):
    return bytes((sector, head)) + struct.pack("<H", cylinder)

def chs_lba(linear):
    cylinder, remainder = divmod(linear, heads * sectors)
    head, sector = divmod(remainder, sectors)
    if cylinder > 0xffff:
        raise SystemExit("partition CHS exceeds the PC-98 table")
    return chs(cylinder, head, sector)

with open(image, "r+b") as stream:
    stream.seek(512)
    table = bytearray(stream.read(512))
    entries = [table[offset:offset + 32]
               for offset in range(0, 512, 32)
               if table[offset] != 0]
    if source_heads_arg:
        source_heads = int(source_heads_arg)
    else:
        maximum_head = max((entry[pos] for entry in entries
                            for pos in (5, 9, 13)), default=0)
        source_heads = 8 if heads == 4 and maximum_head >= 4 else heads
    if source_heads != heads:
        image_sectors = os.path.getsize(image) // 512
        for offset in range(0, 512, 32):
            if table[offset] == 0:
                continue
            for field in (4, 8, 12):
                linear = lba(table[offset + field:offset + field + 4],
                             source_heads)
                if linear >= image_sectors:
                    raise SystemExit("partition CHS lies outside the image")
                table[offset + field:offset + field + 4] = chs_lba(linear)
    for offset in range(0, 512, 32):
        index = offset // 32 + 1
        if selected and index != selected:
            continue
        entry = table[offset:offset + 32]
        if entry[0] == 0:
            continue
        if not selected:
            name = bytes(entry[16:32])
            sid_type = entry[1] & 0x7f
            if name != b"BOOT".ljust(16, b" ") and sid_type not in (
                    0x01, 0x11, 0x20):
                continue
        ipl_cylinder = struct.unpack_from("<H", entry, 6)[0]
        # Keep the IPL and FAT data start identical so the partition PBR is
        # also DOS logical sector zero.
        ipl_lba = ipl_cylinder * heads * sectors
        data_cylinder = ipl_cylinder
        data_lba = ipl_lba
        end_lba = lba(entry[12:16], heads)
        if data_lba > end_lba:
            raise SystemExit("BOOT partition is empty")
        table[offset] |= 0x80
        # MID bit 7 marks the partition active.  SID bit 7 marks it
        # bootable; SID type 0x11 identifies a PC-98 DOS FAT16 volume.
        # Thus A1/91 is visible as a DOS drive and selectable by the NEC
        # fixed-disk boot menu.
        table[offset + 1] = 0x91
        table[offset + 4:offset + 8] = chs(ipl_cylinder)
        table[offset + 8:offset + 12] = chs(data_cylinder)
        table[offset + 16:offset + 32] = b"BOOT".ljust(16, b" ")
        stream.seek(512)
        stream.write(table)
        print(ipl_lba, data_lba, end_lba - data_lba + 1, index)
        break
    else:
        raise SystemExit("FAT16 boot partition not found")
PY
)"
read -r ipl_lba boot_lba boot_sectors partition <<<"$layout"
offset=$((boot_lba * 512))
test $((boot_sectors % 2)) -eq 0 || {
	echo "BOOT partition must contain an even number of physical sectors" >&2
	exit 1
}
logical_sectors=$((boot_sectors / 2))

if test "$install_disk_stubs" -eq 1; then
	dd if="$build/ipl-lba0.bin" of="$image" bs=512 count=1 \
		conv=notrunc status=none
	dd if="$build/ipl-lba2.bin" of="$image" bs=512 seek=2 count=14 \
		conv=notrunc status=none
fi
cluster_sectors=1
while test $((logical_sectors / cluster_sectors)) -ge 65525; do
	cluster_sectors=$((cluster_sectors * 2))
done
test $((logical_sectors / cluster_sectors)) -ge 4085 || {
	echo "BOOT partition is too small for FAT16" >&2
	exit 1
}
mformat -i "$image@@$offset" -S 3 -c "$cluster_sectors" -h "$heads" \
	-s "$sectors" -H "$boot_lba" -T "$logical_sectors" -v BOOT ::
python3 - "$image" "$offset" "$boot_lba" \
	"$build/partition-pbr.bin" <<'PY'
import struct
import sys

image = sys.argv[1]
offset = int(sys.argv[2])
partition_lba = int(sys.argv[3])
pbr_path = sys.argv[4]

with open(image, "r+b") as stream:
	stream.seek(offset)
	bpb = stream.read(1024)
	if len(bpb) != 1024:
		raise SystemExit("short FAT16 reserved sector")
	with open(pbr_path, "rb") as source:
		pbr = bytearray(source.read())
	if len(pbr) != 1024:
		raise SystemExit("partition-pbr.bin is not 1024 bytes")
	pbr[3:0x3e] = bpb[3:0x3e]
	struct.pack_into("<I", pbr, 0x1c, partition_lba)
	struct.pack_into("<H", pbr, 0x0e, 1)
	pbr[0x1fe:0x200] = b"\x55\xaa"
	pbr[0x3fe:0x400] = b"\x55\xaa"
	stream.seek(offset)
	stream.write(pbr)
PY
mcopy -o -i "$image@@$offset" "$build/IO.SYS" ::IO.SYS
mattrib -i "$image@@$offset" +r +h +s ::IO.SYS
mcopy -o -i "$image@@$offset" "$vmunix_image" ::vmunix
mattrib -i "$image@@$offset" +r +h +s ::vmunix
if test "$swap_size_mib" -ne 0; then
	swap_temp="$(mktemp "${TMPDIR:-/tmp}/zedbsd-swap.XXXXXX")"
	python3 - "$swap_temp" "$swap_size_mib" <<'PY'
import struct
import sys

path = sys.argv[1]
file_bytes = int(sys.argv[2]) * 1024 * 1024
slots = file_bytes // 4096 - 1
header = bytearray(64)
header[:8] = b"ZEDSWAP1"
struct.pack_into("<IIIIII", header, 8, 1, 64, 4096,
                 file_bytes, slots, 0)
checksum = 2166136261
for index, byte in enumerate(header):
    if 28 <= index < 32:
        byte = 0
    checksum = ((checksum ^ byte) * 16777619) & 0xffffffff
struct.pack_into("<I", header, 28, checksum)
with open(path, "wb") as stream:
    stream.truncate(file_bytes)
    stream.seek(0)
    stream.write(header)
PY
	if ! mcopy -o -i "$image@@$offset" "$swap_temp" ::SWAPFILE; then
		echo "BOOT partition has insufficient space for ${swap_size_mib} MiB swapfile" >&2
		exit 1
	fi
	mattrib -i "$image@@$offset" +h +s ::SWAPFILE
	rm -f -- "$swap_temp"
	swap_temp=""
fi
ensure_directory ::BIN
# The architecture profile is overlaid directly on both directories.  Keep
# an empty lower /lib in the base image even while all libraries are linked
# statically; VFS must be able to capture both lower mount points before it
# attaches the profile image.
ensure_directory ::LIB
if test -n "$arch_profile" || test -n "$arch_image"; then
	test -n "$arch_profile" && test -n "$arch_image" || {
		echo "ZEDBSD_ARCH_PROFILE and ZEDBSD_ARCH_IMAGE must be set together" >&2
		exit 2
	}
	test -s "$arch_image" || {
		echo "Architecture profile image not found: $arch_image" >&2
		exit 1
	}
	ensure_directory ::ARCH
	mcopy -o -i "$image@@$offset" "$arch_image" ::ARCH/"${arch_profile^^}.IMG"
fi
test -s "$shell_image" || { echo "Shell ELF not found: $shell_image" >&2; exit 1; }
test -s "$build/bin/noct" || { echo "Noct ELF not found: $build/bin/noct" >&2; exit 1; }
test -s "$build/bin/linux" || { echo "Linux loader ELF not found: $build/bin/linux" >&2; exit 1; }
mcopy -o -i "$image@@$offset" "$shell_image" ::BIN/SH
mcopy -o -i "$image@@$offset" "$build/bin/noct" ::BIN/NOCT
mcopy -o -i "$image@@$offset" "$build/bin/linux" ::BIN/LINUX
if test -n "$zinit_rc"; then
	test -f "$zinit_rc" || { echo "zinit.rc not found: $zinit_rc" >&2; exit 1; }
	ensure_directory ::ETC
	mcopy -o -i "$image@@$offset" "$zinit_rc" ::ETC/ZINIT.RC
fi
ensure_directory ::APPS
holoris="$repo/userland/noct/noct-upstream/apps/holoris/holoris.noct"
test -s "$holoris" || {
	echo "Holoris Noct source not found: $holoris" >&2
	exit 1
}
mcopy -o -i "$image@@$offset" "$holoris" ::APPS/HOLORIS.NCT
if test -f "$repo/apps/hello.nct"; then
	mcopy -o -i "$image@@$offset" "$repo/apps/hello.nct" ::APPS/HELLO.NCT
fi
for utility in ls.nct cp.nct; do
	if test -f "$repo/apps/$utility"; then
		mcopy -o -i "$image@@$offset" "$repo/apps/$utility" ::APPS/"${utility^^}"
	fi
done
bmpview_nap="${BMPVIEW_NAP:-$repo/build/bmpview/BMPVIEW.NAP}"
if test ! -s "$bmpview_nap" ||
   test "$repo/apps/bmpview.nct" -nt "$bmpview_nap"; then
	"$repo/scripts/build-bmpview-bytecode.sh"
fi
test -s "$bmpview_nap" || {
	echo "BMP viewer bytecode not found: $bmpview_nap" >&2
	exit 1
}
mcopy -o -i "$image@@$offset" "$bmpview_nap" ::APPS/BMPVIEW.NAP
if test -n "$kernel"; then
	mcopy -o -i "$image@@$offset" "$kernel" ::VMLINUX
fi
if test -n "$boot_cfg"; then
	test -f "$boot_cfg" || { echo "BOOT.CFG not found: $boot_cfg" >&2; exit 1; }
	mcopy -o -i "$image@@$offset" "$boot_cfg" ::BOOT.CFG
fi
if test -f "$repo/platform/pc98/dos/linux98.exe" ||
   test -f "$repo/platform/pc98/dos/inst.exe"; then
	ensure_directory ::INST
fi
if test -f "$repo/platform/pc98/dos/linux98.exe"; then
	mcopy -o -i "$image@@$offset" "$repo/platform/pc98/dos/linux98.exe" ::INST/LINUX98.EXE
fi
if test -f "$repo/platform/pc98/dos/inst.exe"; then
	# INST.EXE resolves its payloads relative to its own path.  Keep an
	# installer copy of IO.SYS here while leaving the boot copy at the root.
	mcopy -o -i "$image@@$offset" "$repo/platform/pc98/dos/inst.exe" ::INST/INST.EXE
	mcopy -o -i "$image@@$offset" "$build/IO.SYS" ::INST/IO.SYS
	mcopy -o -i "$image@@$offset" "$build/ipl-lba0.img" ::INST/IPL-LBA0.IMG
	mcopy -o -i "$image@@$offset" "$build/ipl-lba2.img" ::INST/IPL-LBA2.IMG
	mcopy -o -i "$image@@$offset" "$build/ipl-part.img" ::INST/IPL-PART.IMG
fi
if test -n "${ZEDBSD_FILES:-}"; then
	for file in "$ZEDBSD_FILES"/*; do
		test -f "$file" || continue
		mcopy -o -i "$image@@$offset" "$file" ::
	done
fi
# mtools closes the image when each command exits but does not promise that
# dirty pages have reached storage.  Flush only this image instead of all
# pending writes on the host.
python3 - "$image" <<'PY'
import os
import sys

descriptor = os.open(sys.argv[1], os.O_RDWR)
try:
    os.fdatasync(descriptor)
finally:
    os.close(descriptor)
PY
printf 'Installed zedBSD in %s partition %s (H=%s S=%s, PBR LBA %s, IO.SYS %s bytes)\n' \
	"$image" "$partition" "$heads" "$sectors" "$ipl_lba" "$io_sys_size"
if test "$install_disk_stubs" -eq 1; then
	printf 'Installed distributed disk stubs at LBA 0 and LBA 2-15\n'
else
	printf 'Preserved existing disk IPL code at LBA 0 and LBA 2-15\n'
fi
