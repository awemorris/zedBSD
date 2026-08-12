#!/usr/bin/env bash
set -euo pipefail

# Drive the Holoris sample in its deterministic test mode: a fixed camera
# and piece sequence, delayed-auto-shift movement from a held arrow key,
# and a gravity drop timed by the polled millisecond clock.  The script
# records its state to marker files that this test asserts exactly.
repo="$(cd "$(dirname "$0")/.." && pwd)"
arch="${ZEDBSD_ARCH:-pc98}"
build="${ZEDBSD_BUILD_DIR:-$repo/build/$arch}"
releases="${ZEDBSD_RELEASES_DIR:-$repo/build/releases}"
qemu="${QEMU:-qemu-system-i386}"
bios_dir="${PC98_BIOS_DIR:-$repo/roms/pc98bios}"
machine="${ZEDBSD_HOLORIS_MACHINE:-pc9821}"
cpu="${ZEDBSD_TEST_CPU:-486}"
tag="${ZEDBSD_HOLORIS_TEST_TAG:-holoris-cirrus}"
# The driver is event-based: it detects the game (cyan well pixels) and its
# test-mode exit (cyan gone) from periodic screendumps, so only the held-key
# durations and the overall timeouts are machine dependent.
hold_wait="${ZEDBSD_HOLORIS_HOLD_WAIT:-2}"
start_timeout="${ZEDBSD_HOLORIS_START_TIMEOUT:-240}"
exit_timeout="${ZEDBSD_HOLORIS_EXIT_TIMEOUT:-120}"
minimum_colors="${ZEDBSD_HOLORIS_MINIMUM_COLORS:-4}"
if [[ "$machine" == pc9801 ]]; then
	# The throttled 9801 loads and JIT-compiles the bytecode for minutes,
	# and every soft-dropped row costs one synchronous GDC redraw.
	minimum_colors="${ZEDBSD_HOLORIS_MINIMUM_COLORS:-3}"
	hold_wait="${ZEDBSD_HOLORIS_HOLD_WAIT:-8}"
	start_timeout="${ZEDBSD_HOLORIS_START_TIMEOUT:-900}"
	exit_timeout="${ZEDBSD_HOLORIS_EXIT_TIMEOUT:-900}"
fi
base="${ZEDBSD_TEST_BASE_IMAGE:-$releases/linux-pc98-i386sx-busybox-ide.img}"
work="$build/tests/beui-$tag"
image="$work/holoris.raw"
cfg="$work/BOOT.CFG"
monitor="$work/monitor.sock"
screenshot="$work/holoris.ppm"
qemu_log="$work/qemu.log"

command -v "$qemu" >/dev/null || { echo "QEMU not found: $qemu" >&2; exit 1; }
test -d "$bios_dir" || { echo "PC-98 BIOS directory not found: $bios_dir" >&2; exit 1; }
test -f "$base" || { echo "source image not found: $base" >&2; exit 1; }
for command in mtype python3; do
	command -v "$command" >/dev/null || { echo "$command is required" >&2; exit 1; }
done

mkdir -p "$work"
cp --reflink=auto "$base" "$image"
printf 'holoris test\nhalt\n' > "$cfg"

"$repo/build.sh" vmunix "$arch"
ZEDBSD_ZINIT_RC="$cfg" DISK_SECTORS=17 \
	"$repo/scripts/install-image.sh" "$image" "" "$cfg"

offset="$(python3 - "$image" <<'PY'
import os
import struct
import sys

heads = 4 if os.path.getsize(sys.argv[1]) <= 20 * 1024 * 1024 else 8
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

rm -f -- "$monitor" "$screenshot" "$qemu_log"
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

python3 - "$monitor" "$screenshot" "$hold_wait" "$start_timeout" \
    "$exit_timeout" <<'PY'
import json
import os
import socket
import sys
import time

monitor, screenshot, hold_wait, start_timeout, exit_timeout = sys.argv[1:6]
probe = screenshot + '.probe'

client = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
deadline = time.monotonic() + 15
while True:
    try:
        client.connect(monitor)
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

def cyan_pixels(path):
    # The wire well is cyan; nothing on the text console is.  Accept any
    # bright cyan so both the Cirrus truecolor path and the GDC palette
    # match.
    with open(path, 'rb') as stream:
        if stream.readline().strip() != b'P6':
            return 0
        line = stream.readline()
        while line.startswith(b'#'):
            line = stream.readline()
        line.split()
        stream.readline()
        pixels = stream.read()
    count = 0
    for pos in range(0, len(pixels) - 2, 3):
        if pixels[pos] < 100 and pixels[pos + 1] >= 200 and \
           pixels[pos + 2] >= 200:
            count += 1
    return count

def wait_cyan(predicate, timeout, what):
    deadline = time.monotonic() + float(timeout)
    while time.monotonic() < deadline:
        qmp('screendump', {'filename': probe})
        if predicate(cyan_pixels(probe)):
            os.remove(probe)
            return
        time.sleep(3)
    raise SystemExit(f'timed out waiting for {what}')

qmp('qmp_capabilities')
wait_cyan(lambda count: count >= 100, start_timeout, 'the game to start')
# Slide the piece to the left wall through delayed auto shift.
key_event('left', True)
time.sleep(float(hold_wait))
key_event('left', False)
# The piece is still falling at the wall: capture the wireframe.
qmp('screendump', {'filename': screenshot})
# Soft-drop to the floor; the test-mode script exits at the lock and the
# display returns to the text console.
key_event('down', True)
wait_cyan(lambda count: count < 10, exit_timeout, 'the test-mode exit')
key_event('down', False)
time.sleep(2)
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

# Seed 7 spawns a Z piece first; held-left slides it to column 0 and the
# held soft drop locks it on the floor rows 18/19.
start_marker="$(mtype -i "$image@@$offset" ::HOLOTEST.TXT)"
test "$start_marker" = 'start 4' || {
	echo "start marker mismatch: '$start_marker'" >&2
	exit 1
}
lock_marker="$(mtype -i "$image@@$offset" ::HOLOLOCK.TXT)"
test "$lock_marker" = 'lock 1 4 0 19' || {
	echo "lock marker mismatch: '$lock_marker'" >&2
	exit 1
}
# The soft-drop score depends on the row the drop started from.
result_marker="$(mtype -i "$image@@$offset" ::HOLORES.TXT)"
if ! [[ "$result_marker" =~ ^result\ 1\ [0-9]+\ 0$ ]]; then
	echo "result marker mismatch: '$result_marker'" >&2
	exit 1
fi
python3 - "$screenshot" "$minimum_colors" <<'PY'
import sys

with open(sys.argv[1], 'rb') as stream:
    if stream.readline().strip() != b'P6':
        raise SystemExit('QEMU screenshot is not binary PPM')
    line = stream.readline()
    while line.startswith(b'#'):
        line = stream.readline()
    width, height = map(int, line.split())
    if int(stream.readline()) != 255:
        raise SystemExit('unsupported PPM range')
    pixels = stream.read()
colors = {pixels[pos:pos + 3] for pos in range(0, len(pixels), 3)}
field = 0
for y in range(0, min(height, 400)):
    row = y * width * 3
    for x in range(100, min(width, 400)):
        pos = row + x * 3
        if pixels[pos:pos + 3] != b'\x00\x00\x00':
            field += 1
minimum_colors = int(sys.argv[2])
if width < 640 or height < 400 or len(colors) < minimum_colors or field < 100:
    raise SystemExit('holoris screenshot validation failed: '
                     f'{width}x{height}, {len(colors)} colors, '
                     f'{field} lit field pixels')
PY
printf 'zedBSD BeUI Holoris QEMU test: PASS (%s)\n' "$image"
