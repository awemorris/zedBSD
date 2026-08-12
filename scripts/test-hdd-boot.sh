#!/usr/bin/env bash
set -euo pipefail

# Boot the HDD test image and verify that /etc/zinit.rc starts /bin/menu.nct
# after the one-second countdown.
repo="$(cd "$(dirname "$0")/.." && pwd)"
arch="${ZEDBSD_ARCH:-pc98}"
build="${ZEDBSD_BUILD_DIR:-$repo/build/$arch}"
qemu="${QEMU:-qemu-system-i386}"
bios_dir="${PC98_BIOS_DIR:-$repo/roms/pc98bios}"
machine="${ZEDBSD_TEST_MACHINE:-pc9821}"
cpu="${ZEDBSD_TEST_CPU:-486}"
work="$build/tests/hdd-boot"
image="$work/hdd.img"
monitor="$work/monitor.sock"
menu="$work/MENU.NCT"
background="$work/MENUBACK.BMP"
screenshot="$work/hdd-boot-failure.ppm"
qemu_memory="${ZEDBSD_QEMU_MEMORY:-8}"

command -v "$qemu" >/dev/null || { echo "QEMU not found: $qemu" >&2; exit 1; }
test -d "$bios_dir" || {
	echo "PC-98 BIOS directory not found: $bios_dir" >&2
	exit 1
}

rm -rf "$work"
mkdir -p "$work"
cat > "$menu" <<'EOF'
func main() {
    System.setEnv("BOOT_ACTION", "echo zinit menu verified.");
    return 0;
}
EOF
cp "$repo/apps/menuback.bmp" "$background"
ZEDBSD_MENU="$menu" ZEDBSD_MENU_BACKGROUND="$background" \
	"$repo/scripts/make-hdd-image.sh" "$image"

rm -f -- "$monitor"
"$qemu" -M "$machine" -cpu "$cpu" -m "$qemu_memory" -accel tcg -L "$bios_dir" \
	-nic none -drive "if=ide,bus=0,unit=0,format=raw,file=$image" \
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

monitor, screenshot = sys.argv[1:]

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

cmd("qmp_capabilities")

deadline = time.time() + 60
while time.time() < deadline:
    if mem(0x20000, 4) == b"B98S":
        print("vmunix loaded")
        break
    time.sleep(2)
else:
    raise SystemExit("vmunix never appeared at the Stage 2 load address")

deadline = time.time() + 120
while time.time() < deadline:
    text = mem(0xA0000, 25 * 160)[0::2]
    if b"zinit menu verified." in text:
        print("zinit.rc and /bin/menu.nct completed")
        sys.exit(0)
    time.sleep(2)
cmd("screendump", {"filename": screenshot})
raise SystemExit("zinit.rc/menu completion never appeared; " + screenshot)
PY
echo "zedBSD HDD boot test: PASS"
