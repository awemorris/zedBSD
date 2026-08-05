#!/usr/bin/env bash
set -euo pipefail

# Boot the FAT12 floppy image under emulation and verify that Stage 1
# located and loaded BOOT.SYS: the B98S Stage 2 header magic must appear
# at the load address 0x20000.
repo="$(cd "$(dirname "$0")/.." && pwd)"
arch="${BOOTS_ARCH:-pc98}"
build="${BOOTS_BUILD_DIR:-$repo/build/$arch}"
qemu="${QEMU:-qemu-system-i386}"
bios_dir="${PC98_BIOS_DIR:-$repo/roms/pc98bios}"
work="$build/tests/fdd-boot"
image="$work/fdd.img"
monitor="$work/monitor.sock"

command -v "$qemu" >/dev/null || { echo "QEMU not found: $qemu" >&2; exit 1; }
test -d "$bios_dir" || {
	echo "PC-98 BIOS directory not found: $bios_dir" >&2
	exit 1
}

rm -rf "$work"
mkdir -p "$work"
"$repo/scripts/make-fdd-image.sh" "$image"

rm -f -- "$monitor"
"$qemu" -M pc9801 -cpu 386 -m 8 -accel tcg -L "$bios_dir" \
	-nic none -drive "if=floppy,format=raw,file=$image" \
	-display none -serial none \
	-qmp "unix:$monitor,server=on,wait=off" -no-reboot \
	>/dev/null 2>&1 &
qemu_pid=$!
cleanup()
{
	if kill -0 "$qemu_pid" 2>/dev/null; then
		kill "$qemu_pid" 2>/dev/null || true
		wait "$qemu_pid" 2>/dev/null || true
	fi
	rm -f -- "$monitor"
}
trap cleanup EXIT INT TERM

python3 - "$monitor" <<'PY'
import json
import socket
import sys
import time

path = sys.argv[1]

deadline = time.time() + 15
sock = None
while time.time() < deadline:
    try:
        sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        sock.connect(path)
        break
    except OSError:
        sock = None
        time.sleep(0.5)
if sock is None:
    raise SystemExit("QMP socket did not appear")

stream = sock.makefile("rw", encoding="utf-8")

def command(name, arguments=None):
    request = {"execute": name}
    if arguments:
        request["arguments"] = arguments
    stream.write(json.dumps(request) + "\n")
    stream.flush()
    while True:
        response = json.loads(stream.readline())
        if "return" in response:
            return response["return"]
        if "error" in response:
            raise SystemExit(f"QMP error: {response['error']}")

json.loads(stream.readline())  # greeting
command("qmp_capabilities")

deadline = time.time() + 60
while time.time() < deadline:
    text = command("human-monitor-command",
                   {"command-line": "xp /4bx 0x20000"})
    if "0x42 0x39 0x38 0x53" in text:  # "B98S"
        print("BOOT.SYS header found at 0x20000")
        sys.exit(0)
    time.sleep(1)
raise SystemExit("BOOT.SYS never appeared at the Stage 2 load address")
PY
echo "Boots FDD FAT12 boot test: PASS"
