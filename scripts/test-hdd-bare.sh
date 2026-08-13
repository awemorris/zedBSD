#!/usr/bin/env bash
set -euo pipefail

# Build the standard image and prove that it enters /bin/sh without a product
# startup script.  Product GUI tests belong to linux-pc98/bootloader/tests.
repo="$(cd "$(dirname "$0")/.." && pwd)"
arch="${ZEDBSD_ARCH:-pc98}"
build="${ZEDBSD_BUILD_DIR:-$repo/build/$arch}"
qemu="${QEMU:-qemu-system-i386}"
bios_dir="${PC98_BIOS_DIR:-$repo/roms/pc98bios}"
machine="${ZEDBSD_TEST_MACHINE:-pc9821}"
cpu="${ZEDBSD_TEST_CPU:-486}"
memory="${ZEDBSD_QEMU_MEMORY:-8}"
work="$build/tests/hdd-bare"
image="$work/hdd.img"
monitor="$work/monitor.sock"
screenshot="$work/hdd-bare-failure.ppm"

command -v "$qemu" >/dev/null || { echo "QEMU not found: $qemu" >&2; exit 1; }
test -d "$bios_dir" || { echo "PC-98 BIOS directory not found: $bios_dir" >&2; exit 1; }

rm -rf -- "$work"
mkdir -p "$work"
"$repo/scripts/make-hdd-image.sh" "$image"

# The test image has one partition beginning at cylinder 1 (H=8, S=17).
fat_offset=$((8 * 17 * 512))
for absent in ::ETC/ZINIT.RC ::BIN/MENU.NCT ::BIN/MENUBACK.BMP \
    ::APPS/HOLORIS.NAP ::APPS/EMACS.NAP \
    ::HOME/SKKJISYO.DIC; do
	if mdir -i "$image@@$fat_offset" "$absent" >/dev/null 2>&1; then
		echo "bare image unexpectedly contains $absent" >&2
		exit 1
	fi
done

rm -f -- "$monitor"
"$qemu" -M "$machine" -cpu "$cpu" -m "$memory" -accel tcg -L "$bios_dir" \
	-nic none -drive "if=ide,bus=0,unit=0,format=raw,file=$image" \
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

python3 - "$monitor" "$screenshot" <<'PY'
import json
import socket
import sys
import time

monitor, screenshot = sys.argv[1:]
deadline = time.time() + 25
sock = None
while time.time() < deadline:
    try:
        sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        sock.connect(monitor)
        break
    except OSError:
        time.sleep(0.25)
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
        if len(body) == 2:
            raw.extend(int(token, 16) for token in body[1].split())
    return bytes(raw)

cmd("qmp_capabilities")
deadline = time.time() + 90
while time.time() < deadline:
    text = mem(0xA0000, 25 * 160)[0::2]
    if b"Loading /etc/zinit.rc" in text:
        raise SystemExit("bare image attempted to load /etc/zinit.rc")
    if b"/ $" in text:
        print("bare zedBSD /bin/sh prompt observed")
        sys.exit(0)
    time.sleep(1)
cmd("screendump", {"filename": screenshot})
raise SystemExit("/bin/sh prompt was not observed; " + screenshot)
PY

echo "zedBSD bare HDD boot test: PASS"
