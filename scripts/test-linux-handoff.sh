#!/usr/bin/env bash
set -euo pipefail

repo="$(cd "$(dirname "$0")/.." && pwd)"
build="${ZEDBSD_BUILD_DIR:-$repo/build/pc98}"
qemu="${QEMU:-qemu-system-i386}"
bios="${PC98_BIOS_DIR:-$repo/roms/pc98bios}"
base="${ZEDBSD_TEST_BASE:-$repo/build/releases/linux-pc98-i386sx-busybox-ide.img}"
work="$build/tests/linux-handoff"
image="$work/linux-handoff.img"
monitor="$work/monitor.sock"
kernel="$work/VMLINUX"

test -x "$qemu" || { echo "QEMU not found: $qemu" >&2; exit 1; }
test -d "$bios" || { echo "BIOS directory not found: $bios" >&2; exit 1; }
test -f "$base" || { echo "Linux image not found: $base" >&2; exit 1; }
mkdir -p "$work"
cp --reflink=auto "$base" "$image"
offset="$(python3 - "$image" <<'PY'
import os, struct, sys
heads = 4 if os.path.getsize(sys.argv[1]) <= 20 * 1024 * 1024 else 8
with open(sys.argv[1], "rb") as stream:
    stream.seek(512)
    table = stream.read(512)
for pos in range(0, 512, 32):
    entry = table[pos:pos + 32]
    if entry[0] and entry[16:32] == b"BOOT".ljust(16, b" "):
        print(struct.unpack_from("<H", entry, 6)[0] * heads * 17 * 512)
        break
else:
    raise SystemExit("BOOT partition not found")
PY
)"
mcopy -o -i "$image@@$offset" ::VMLINUX "$kernel"
if test "${ZEDBSD_LINUX_HANDOFF_FRESH:-0}" = 1; then
	rm -f -- "$image"
	ZEDBSD_TEST_MB="${ZEDBSD_LINUX_HANDOFF_MB:-40}" \
	ZEDBSD_SWAP_SIZE_MIB="${ZEDBSD_SWAP_SIZE_MIB:-32}" \
	ZEDBSD_AUTOEXEC="$repo/tests/auto-linux.nct" \
	ZEDBSD_KERNEL="$kernel" ZEDBSD_CFG="$repo/tests/linux-boot.cfg" \
		"$repo/scripts/make-hdd-image.sh" "$image"
else
	ZEDBSD_AUTOEXEC="$repo/tests/auto-linux.nct" \
		"$repo/scripts/install-image.sh" "$image" "$kernel" \
		"$repo/tests/linux-boot.cfg"
fi

rm -f -- "$monitor"
"$qemu" -M pc9821 -cpu 486 -m 64 -accel tcg -L "$bios" -nic none \
	-drive "if=ide,bus=0,unit=0,format=raw,file=$image" -snapshot \
	-display none -serial none -qmp "unix:$monitor,server=on,wait=off" \
	-no-reboot >/dev/null 2>&1 &
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

python3 - "$monitor" "$work/linux-handoff.ppm" <<'PY'
import json
import re
import socket
import sys
import time

path, screenshot = sys.argv[1:]
client = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
deadline = time.monotonic() + 20
while True:
    try:
        client.connect(path)
        break
    except (FileNotFoundError, ConnectionRefusedError):
        if time.monotonic() >= deadline:
            raise SystemExit("QMP monitor did not become ready")
        time.sleep(.1)
stream = client.makefile("rwb", buffering=0)
json.loads(stream.readline())

def qmp(execute, arguments=None, reply=True):
    request = {"execute": execute}
    if arguments is not None:
        request["arguments"] = arguments
    stream.write(json.dumps(request).encode("ascii") + b"\n")
    if not reply:
        return None
    while True:
        result = json.loads(stream.readline())
        if "return" in result:
            return result["return"]
        if "error" in result:
            raise SystemExit(f"QMP {execute} failed: {result['error']}")

def registers():
    return qmp("human-monitor-command", {"command-line": "info registers"})

qmp("qmp_capabilities")

# A successful handoff leaves zedBSD' 0x8002xxxx low-loader execution range.
# Linux may quickly enable its own high mapping, so accept either a physical
# kernel entry or the normal 0xc0000000 kernel half.
deadline = time.monotonic() + 35
last = ""
while time.monotonic() < deadline:
    last = registers()
    match = re.search(r"EIP=([0-9A-Fa-f]{8})", last)
    if match:
        eip = int(match.group(1), 16)
        if (0x00100000 <= eip < 0x80000000) or eip >= 0xc0000000:
            print(f"Linux handoff observed at EIP=0x{eip:08x}")
            stream.close()
            client.close()
            raise SystemExit(0)
    time.sleep(1)
print(last, file=sys.stderr)
qmp("screendump", {"filename": screenshot})
raise SystemExit("Linux never left the zedBSD loader execution range")
PY

kill "$qemu_pid" 2>/dev/null || true
wait "$qemu_pid" 2>/dev/null || true
trap - EXIT INT TERM
rm -f -- "$monitor"
echo "zedBSD Linux point-of-no-return QEMU test: PASS"
