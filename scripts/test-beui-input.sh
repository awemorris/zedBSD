#!/usr/bin/env bash
set -euo pipefail

# Exercise the polled millisecond clock and the real-time key state API
# from a Noct script, without opening the display.  QEMU holds the down
# arrow for a while; the script must see it held, see it released, and
# see an unpressed key stay up.
repo="$(cd "$(dirname "$0")/.." && pwd)"
arch="${BOOTS_ARCH:-pc98}"
build="${BOOTS_BUILD_DIR:-$repo/build/$arch}"
releases="${BOOTS_RELEASES_DIR:-$repo/build/releases}"
qemu="${QEMU:-qemu-system-i386}"
bios_dir="${PC98_BIOS_DIR:-$repo/roms/pc98bios}"
machine="${BOOTS_INPUT_MACHINE:-pc9821}"
cpu="${BOOTS_TEST_CPU:-486}"
initial_wait="${BOOTS_INPUT_INITIAL_WAIT:-15}"
hold_wait="${BOOTS_INPUT_HOLD_WAIT:-3}"
completion_wait="${BOOTS_INPUT_COMPLETION_WAIT:-15}"
base="${BOOTS_TEST_BASE_IMAGE:-$releases/linux-pc98-i386sx-busybox-ide.img}"
work="$build/tests/beui-input"
image="$work/input.raw"
files="$work/files"
cfg="$work/BOOTS.CFG"
monitor="$work/monitor.sock"
qemu_log="$work/qemu.log"

command -v "$qemu" >/dev/null || { echo "QEMU not found: $qemu" >&2; exit 1; }
test -d "$bios_dir" || { echo "PC-98 BIOS directory not found: $bios_dir" >&2; exit 1; }
test -f "$base" || { echo "source image not found: $base" >&2; exit 1; }
for command in mtype python3; do
	command -v "$command" >/dev/null || { echo "$command is required" >&2; exit 1; }
done

mkdir -p "$work" "$files"
cp --reflink=auto "$base" "$image"
cat > "$files/INPUT98.NCT" <<'EOF'
func main() {
    var t0 = BeUI.getMilliseconds();
    BeUI.sleep(1000);
    var elapsed = BeUI.getMilliseconds() - t0;
    var clockOk = 0;
    if (elapsed >= 900 && elapsed <= 5000) { clockOk = 1; }
    FileUtil.writeText("CLK.TXT", "clock " + clockOk + " " + elapsed);

    var held = 0;
    var upWhileHeld = 0;
    var released = 0;
    var waited = 0;
    while (waited < 30000 && held == 0) {
        if (BeUI.isKeyDown(Key.Down) == 1) {
            held = 1;
            if (BeUI.isKeyDown(Key.Up) == 0) { upWhileHeld = 1; }
        }
        BeUI.sleep(20);
        waited = waited + 20;
    }
    waited = 0;
    while (waited < 30000 && held == 1 && released == 0) {
        if (BeUI.isKeyDown(Key.Down) == 0) { released = 1; }
        BeUI.sleep(20);
        waited = waited + 20;
    }
    FileUtil.writeText("KEYST.TXT",
                       "keystate " + held + " " + upWhileHeld + " " +
                       released);
    return 0;
}
EOF
printf 'input98\nhalt\n' > "$cfg"

make -C "$repo" ARCH="$arch" -j"$(nproc)" BOOT.SYS
BOOTS_FILES="$files" DISK_SECTORS=17 \
	"$repo/scripts/install-image.sh" "$image" "" "$cfg"

offset="$(python3 - "$image" <<'PY'
import os
import struct
import sys

heads = 4 if os.path.getsize(sys.argv[1]) <= 40 * 1024 * 1024 else 8
with open(sys.argv[1], 'rb') as stream:
    stream.seek(512)
    table = stream.read(512)
for pos in range(0, 512, 32):
    entry = table[pos:pos + 32]
    if entry[0] and entry[16:32] == b'BOOT'.ljust(16, b' '):
        cylinder = struct.unpack_from('<H', entry, 6)[0]
        print(cylinder * heads * 17 * 512)
        break
else:
    raise SystemExit('BOOT partition not found')
PY
)"

rm -f -- "$monitor" "$qemu_log"
"$qemu" -M "$machine" -cpu "$cpu" -m 6 -accel tcg -L "$bios_dir" \
	-nic none -drive "if=ide,bus=0,unit=0,format=raw,file=$image" \
	-display none -serial none -qmp "unix:$monitor,server=on,wait=off" \
	-no-reboot >"$qemu_log" 2>&1 &
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

python3 - "$monitor" "$initial_wait" "$hold_wait" "$completion_wait" <<'PY'
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
            raise SystemExit('QEMU monitor did not become ready')
        time.sleep(.1)
stream = client.makefile('rwb', buffering=0)
json.loads(stream.readline())

def qmp(execute, arguments=None, wait_reply=True):
    request = {'execute': execute}
    if arguments is not None:
        request['arguments'] = arguments
    stream.write(json.dumps(request).encode('ascii') + b'\n')
    if not wait_reply:
        return
    while True:
        reply = json.loads(stream.readline())
        if 'return' in reply:
            return reply['return']
        if 'error' in reply:
            raise SystemExit(f"QMP {execute} failed: {reply['error']}")

def key_event(qcode, down):
    qmp('input-send-event', {'events': [
        {'type': 'key', 'data': {'down': down,
          'key': {'type': 'qcode', 'data': qcode}}},
    ]})

qmp('qmp_capabilities')
time.sleep(float(sys.argv[2]))
key_event('down', True)
time.sleep(float(sys.argv[3]))
key_event('down', False)
time.sleep(float(sys.argv[4]))
qmp('quit', wait_reply=False)
stream.close()
client.close()
PY

for _ in $(seq 1 50); do
	if ! kill -0 "$qemu_pid" 2>/dev/null; then break; fi
	sleep .1
done
if kill -0 "$qemu_pid" 2>/dev/null; then kill "$qemu_pid"; fi
wait "$qemu_pid" 2>/dev/null || true
trap - EXIT INT TERM
rm -f -- "$monitor"

clock_marker="$(mtype -i "$image@@$offset" ::CLK.TXT)"
case "$clock_marker" in
clock\ 1\ *) ;;
*)
	echo "clock marker mismatch: '$clock_marker'" >&2
	exit 1
	;;
esac
keystate_marker="$(mtype -i "$image@@$offset" ::KEYST.TXT)"
test "$keystate_marker" = 'keystate 1 1 1' || {
	echo "key state marker mismatch: '$keystate_marker'" >&2
	exit 1
}
printf 'Boots BeUI input QEMU test: PASS (%s)\n' "$image"
