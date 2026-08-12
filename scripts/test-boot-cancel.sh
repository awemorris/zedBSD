#!/usr/bin/env bash
set -euo pipefail

# Verify that a key pressed during the one-second BOOT.CFG countdown skips the
# file and enters the interactive /bin/sh immediately.
repo="$(cd "$(dirname "$0")/.." && pwd)"
build="${ZEDBSD_BUILD_DIR:-$repo/build/pc98}"
qemu="${QEMU:-qemu-system-i386}"
bios="${PC98_BIOS_DIR:-$repo/roms/pc98bios}"
work="$build/tests/boot-cancel"
image="$work/boot-cancel.img"
cfg="$work/BOOT.CFG"
monitor="$work/monitor.sock"
screenshot="$work/boot-countdown.ppm"

test -x "$qemu" || { echo "QEMU not found: $qemu" >&2; exit 1; }
test -d "$bios" || { echo "BIOS directory not found: $bios" >&2; exit 1; }
rm -rf -- "$work"
mkdir -p "$work"
printf 'echo BOOTCFG-RAN\n' >"$cfg"
ZEDBSD_AUTOEXEC_DISABLE=1 ZEDBSD_BOOT_CFG="$cfg" \
	"$repo/scripts/make-hdd-image.sh" "$image"

"$qemu" -M pc9821 -cpu 486 -m 8 -accel tcg -L "$bios" -nic none \
	-drive "if=ide,bus=0,unit=0,format=raw,file=$image" \
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

python3 - "$monitor" "$screenshot" <<'PY'
import json
import socket
import sys
import time

client = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
deadline = time.monotonic() + 15
while True:
    try:
        client.connect(sys.argv[1])
        break
    except (FileNotFoundError, ConnectionRefusedError):
        if time.monotonic() >= deadline:
            raise SystemExit("QMP socket did not appear")
        time.sleep(.05)
stream = client.makefile("rwb", buffering=0)
json.loads(stream.readline())

def qmp(execute, arguments=None):
    request = {"execute": execute}
    if arguments is not None:
        request["arguments"] = arguments
    stream.write(json.dumps(request).encode() + b"\n")
    while True:
        reply = json.loads(stream.readline())
        if "return" in reply:
            return reply["return"]
        if "error" in reply:
            raise SystemExit(f"QMP {execute}: {reply['error']}")

def memory(address, count):
    text = qmp("human-monitor-command",
               {"command-line": f"xp /{count}bx 0x{address:x}"})
    result = []
    for line in text.splitlines():
        if ":" in line:
            result.extend(int(token, 16)
                          for token in line.split(":", 1)[1].split())
    return bytes(result[:count])

qmp("qmp_capabilities")
deadline = time.monotonic() + 45
while time.monotonic() < deadline:
    screen = memory(0xa0000, 25 * 160)[0::2]
    if b"Starting boot.cfg" in screen:
        qmp("screendump", {"filename": sys.argv[2]})
        qmp("input-send-event", {"events": [
            {"type": "key", "data": {"down": True,
             "key": {"type": "qcode", "data": "spc"}}},
            {"type": "key", "data": {"down": False,
             "key": {"type": "qcode", "data": "spc"}}},
        ]})
        break
    time.sleep(.02)
else:
    raise SystemExit("BOOT.CFG countdown was not displayed")

deadline = time.monotonic() + 10
while time.monotonic() < deadline:
    screen = memory(0xa0000, 25 * 160)[0::2]
    if b"BOOTCFG-RAN" in screen:
        raise SystemExit("BOOT.CFG ran despite the cancellation key")
    if b"/ $ " in screen:
        print("BOOT.CFG cancelled and /bin/sh prompt observed")
        qmp("quit")
        break
    time.sleep(.05)
else:
    raise SystemExit("interactive /bin/sh prompt was not observed")
PY

wait "$qemu_pid" 2>/dev/null || true
trap - EXIT INT TERM
rm -f -- "$monitor"
echo "zedBSD BOOT.CFG cancellation QEMU test: PASS"
