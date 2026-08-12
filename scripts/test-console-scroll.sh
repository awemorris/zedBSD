#!/usr/bin/env bash
set -euo pipefail

repo="$(cd "$(dirname "$0")/.." && pwd)"
build="${ZEDBSD_BUILD_DIR:-$repo/build/pc98}"
qemu="${QEMU:-qemu-system-i386}"
bios="${PC98_BIOS_DIR:-$repo/roms/pc98bios}"
work="$build/tests/console-scroll"
image="$work/console-scroll.img"
zinit="$work/ZINIT.RC"
monitor="$work/monitor.sock"

test -x "$qemu" || { echo "QEMU not found: $qemu" >&2; exit 1; }
test -d "$bios" || { echo "BIOS directory not found: $bios" >&2; exit 1; }
rm -rf -- "$work"
mkdir -p "$work"
for number in $(seq -w 0 30); do
	printf 'echo SCROLL-%s\n' "$number"
done >"$zinit"
ZEDBSD_ZINIT_RC="$zinit" "$repo/scripts/make-hdd-image.sh" "$image"

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

python3 - "$monitor" <<'PY'
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
    if b"SCROLL-30" in screen and b"/ $ " in screen:
        if b"NEC PC-9800" in screen or b"SCROLL-00" in screen:
            raise SystemExit("console reached the last row without scrolling")
        print("console scrolled and retained the newest rows")
        qmp("quit")
        break
    time.sleep(.1)
else:
    raise SystemExit("final scrolling marker was not observed")
PY

wait "$qemu_pid" 2>/dev/null || true
trap - EXIT INT TERM
rm -f -- "$monitor"
echo "zedBSD console scrolling QEMU test: PASS"
