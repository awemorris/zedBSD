#!/usr/bin/env bash
set -euo pipefail

# Verify the persistent shell/BOOT.CFG <-> Noct environment bridge on i386.
repo="$(cd "$(dirname "$0")/.." && pwd)"
arch="${ZEDBSD_ARCH:-pc98}"
build="${ZEDBSD_BUILD_DIR:-$repo/build/$arch}"
releases="${ZEDBSD_RELEASES_DIR:-$repo/build/releases}"
qemu="${QEMU:-qemu-system-i386}"
bios_dir="${PC98_BIOS_DIR:-$repo/roms/pc98bios}"
base="${ZEDBSD_TEST_BASE_IMAGE:-$releases/linux-pc98-i486dx-debian13-ide.img}"
work="$build/tests/m14-env"
image="$work/m14-ide.raw"
files="$work/files"
cfg="$work/BOOT.CFG"
expected='M14 ENV PASS'

command -v "$qemu" >/dev/null || { echo "QEMU not found: $qemu" >&2; exit 1; }
test -d "$bios_dir" || { echo "PC-98 BIOS directory not found: $bios_dir" >&2; exit 1; }
test -f "$base" || { echo "M14 source image not found: $base" >&2; exit 1; }
for command in mtype python3 timeout; do
	command -v "$command" >/dev/null || { echo "$command is required" >&2; exit 1; }
done

mkdir -p "$work" "$files"
cp --reflink=auto "$base" "$image"
cat > "$files/M14SET.NCT" <<'EOF'
func main() {
    if (System.getEnv("MODE") != "boot") { return 1; }
    if (System.getEnv("REMOVE") != "") { return 2; }
    var values = System.listEnv();
    if (values.MODE != "boot") { return 3; }
    System.setEnv("FROM_NOCT", "yes");
    return 0;
}
EOF
cat > "$files/M14CHECK.NCT" <<'EOF'
func main() {
    if (System.getEnv("MODE") != "boot") { return 1; }
    if (System.getEnv("FROM_NOCT") != "yes") { return 2; }
    System.unsetEnv("FROM_NOCT");
    if (System.getEnv("FROM_NOCT") != "") { return 3; }
    FileUtil.writeText("M14.TXT", "M14 ENV PASS");
    return 0;
}
EOF
cat > "$cfg" <<'EOF'
set REMOVE old
unset REMOVE
set MODE boot
env
m14set
m14check
halt
EOF
"$repo/build.sh" vmunix "$arch"
ZEDBSD_FILES="$files" ZEDBSD_ZINIT_RC="$cfg" \
	DISK_SECTORS=17 \
	"$repo/scripts/install-image.sh" "$image" "" "$cfg"

offset="$(python3 - "$image" <<'PY'
import struct
import sys

heads = 8
with open(sys.argv[1], "rb") as stream:
    stream.seek(512)
    table = stream.read(512)
for offset in range(0, 512, 32):
    entry = table[offset:offset + 32]
    if entry[0] and entry[16:32] == b"BOOT".ljust(16, b" "):
        cylinder = struct.unpack_from("<H", entry, 6)[0]
        print(cylinder * heads * 17 * 512)
        break
else:
    raise SystemExit("BOOT partition not found")
PY
)"

set +e
timeout --signal=INT --kill-after=5 30 \
	"$qemu" -M pc9801 -cpu 386 -m 6 -accel tcg -L "$bios_dir" \
	-nic none -drive "if=ide,bus=0,unit=0,format=raw,file=$image" \
	-display none -serial none -monitor none -no-reboot >/dev/null 2>&1
status=$?
set -e
if test "$status" -ne 0 && test "$status" -ne 124; then
	echo "M14 QEMU failed with status $status" >&2
	exit 1
fi
actual="$(mtype -i "$image@@$offset" ::M14.TXT)"
test "$actual" = "$expected" || {
	echo "M14 environment marker mismatch: '$actual'" >&2
	exit 1
}
printf 'M14 zedBSD environment QEMU test: PASS (%s)\n' "$image"
