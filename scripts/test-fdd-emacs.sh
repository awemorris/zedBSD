#!/usr/bin/env bash
set -euo pipefail

# Boot the floppy image and walk the full pre-boot editing path:
# FDD IPL -> Stage 1 (FAT12) -> vmunix -> automatic AUTOEXEC.NCT ->
# BeUI menu -> Remacs.  The Remacs mode line in text VRAM is the proof.
repo="$(cd "$(dirname "$0")/.." && pwd)"
arch="${ZEDBSD_ARCH:-pc98}"
build="${ZEDBSD_BUILD_DIR:-$repo/build/$arch}"
qemu="${QEMU:-qemu-system-i386}"
bios_dir="${PC98_BIOS_DIR:-$repo/roms/pc98bios}"
work="$build/tests/fdd-emacs"
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

monitor = sys.argv[1]

deadline = time.time() + 25
sock = None
while time.time() < deadline:
    try:
        sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        sock.connect(monitor)
        break
    except OSError:
        sock = None
        time.sleep(0.5)
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

def vram_row(row):
    return mem(0xA0000 + row * 160, 160)[0::2]

def key(qcode):
    for down in (True, False):
        cmd("input-send-event", {"events": [{
            "type": "key",
            "data": {"down": down, "key": {"type": "qcode", "data": qcode}},
        }]})
        time.sleep(0.06)

cmd("qmp_capabilities")

deadline = time.time() + 120
while time.time() < deadline:
    if b"AUTOEXEC.NCT" in vram_row(5):
        print("automatic AUTOEXEC.NCT starting")
        break
    time.sleep(2)
else:
    raise SystemExit("automatic AUTOEXEC start never appeared")

# The BeUI menu fills the frame buffer; sample mid-screen plane bytes.
deadline = time.time() + 90
while time.time() < deadline:
    if any(mem(0xA8000 + 8000, 64)) or any(mem(0xB0000 + 8000, 64)):
        print("BeUI menu displayed")
        break
    time.sleep(2)
else:
    raise SystemExit("BeUI menu never appeared")

time.sleep(3)
key("down")
time.sleep(1.5)
key("ret")
print("selected Emacs")

deadline = time.time() + 180
while time.time() < deadline:
    blob = b"|".join(vram_row(row) for row in range(25))
    if b"remacs:" in blob:
        print("Remacs mode line found")
        sys.exit(0)
    time.sleep(5)
raise SystemExit("Remacs never appeared in text VRAM")
PY
echo "zedBSD FDD Emacs boot test: PASS"
