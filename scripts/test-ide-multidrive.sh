#!/usr/bin/env bash
set -euo pipefail

# Verify that Stage 1 publishes both IDE descriptors and that Stage 2 can
# select, partition-scan, mount, and read the slave disk through the native
# PC-98 IDE driver.
repo="$(cd "$(dirname "$0")/.." && pwd)"
arch="${ZEDBSD_ARCH:-pc98}"
build="${ZEDBSD_BUILD_DIR:-$repo/build/$arch}"
qemu="${QEMU:-qemu-system-i386}"
bios_dir="${PC98_BIOS_DIR:-$repo/roms/pc98bios}"
machine="${ZEDBSD_TEST_MACHINE:-pc9821}"
cpu="${ZEDBSD_TEST_CPU:-486}"
work="$build/tests/ide-multidrive"
primary="$work/primary.img"
secondary="$work/secondary.img"
cfg="$work/BOOT.CFG"
marker="$work/SECOND.TXT"
monitor="$work/monitor.sock"

command -v "$qemu" >/dev/null || {
	echo "QEMU not found: $qemu" >&2
	exit 1
}
test -d "$bios_dir" || {
	echo "PC-98 BIOS directory not found: $bios_dir" >&2
	exit 1
}

rm -rf -- "$work"
mkdir -p "$work"
cat > "$cfg" <<'EOF'
device
pwd
cd /disk2
pwd
cat SECOND.TXT
halt
EOF
printf '%s\n' 'SECOND IDE OK' > "$marker"

ZEDBSD_BOOT_CFG="$cfg" ZEDBSD_ZINIT_RC="$cfg" \
	"$repo/scripts/make-hdd-image.sh" "$primary"
offset="$(python3 - "$primary" <<'PY'
import struct
import sys

with open(sys.argv[1], "rb") as stream:
    stream.seek(512)
    table = stream.read(512)
for pos in range(0, 512, 32):
    entry = table[pos:pos + 32]
    if entry[0] and entry[16:32] == b"BOOT".ljust(16, b" "):
        cylinder = struct.unpack_from("<H", entry, 6)[0]
        print(cylinder * 8 * 17 * 512)
        break
else:
    raise SystemExit("BOOT partition not found")
PY
)"
# BOOT.CFG controls startup; AUTOEXEC.NCT is never implicit.
mdel -i "$primary@@$offset" ::AUTOEXEC.NCT 2>/dev/null || true
cp --reflink=auto "$primary" "$secondary"
mcopy -o -i "$secondary@@$offset" "$marker" ::SECOND.TXT

rm -f -- "$monitor"
"$qemu" -M "$machine" -cpu "$cpu" -m 8 -accel tcg -L "$bios_dir" \
	-nic none \
	-drive "if=ide,bus=0,unit=0,format=raw,file=$primary" \
	-drive "if=ide,bus=0,unit=1,format=raw,file=$secondary" \
	-snapshot -display none -serial none \
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

monitor = sys.argv[1]
deadline = time.time() + 10
sock = None
while time.time() < deadline:
    try:
        sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        sock.connect(monitor)
        break
    except OSError:
        sock = None
        time.sleep(0.1)
if sock is None:
    raise SystemExit("QMP socket did not appear")
stream = sock.makefile("rw", encoding="utf-8")
json.loads(stream.readline())

def cmd(name, arguments=None):
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

def mem(addr, count):
    text = cmd("human-monitor-command",
               {"command-line": "xp /%dbx 0x%x" % (count, addr)})
    raw = []
    for line in text.splitlines():
        body = line.split(":", 1)
        if len(body) != 2:
            continue
        for token in body[1].split():
            raw.append(int(token, 16))
    return bytes(raw)

cmd("qmp_capabilities")
deadline = time.time() + 35
text = b""
while time.time() < deadline:
    text = mem(0xA0000, 25 * 160)[0::2]
    if b"SECOND IDE OK" in text:
        if b"ide1 BIOS 81 H/S 8/17" not in text:
            raise SystemExit("second disk was readable but its BIOS geometry was not listed")
        if b"/disk2" not in text:
            raise SystemExit("VFS did not enter the second mounted partition")
        print("IDE slave descriptor, CHS, partition mount, and read: PASS")
        sys.exit(0)
    time.sleep(0.25)
print(text.decode("ascii", "replace"), file=sys.stderr)
raise SystemExit("zedBSD did not read SECOND.TXT from IDE unit 1")
PY

echo "zedBSD IDE multidrive QEMU test: PASS"
