#!/usr/bin/env bash
set -euo pipefail

# Exercise the display backend and BMP pipeline in a real vmunix VM.
repo="$(cd "$(dirname "$0")/.." && pwd)"
arch="${ZEDBSD_ARCH:-pc98}"
build="${ZEDBSD_BUILD_DIR:-$repo/build/$arch}"
releases="${ZEDBSD_RELEASES_DIR:-$repo/build/releases}"
qemu="${QEMU:-qemu-system-i386}"
bios_dir="${PC98_BIOS_DIR:-$repo/roms/pc98bios}"
machine="${ZEDBSD_BEUI_MACHINE:-pc9801}"
cpu="${ZEDBSD_BEUI_CPU:-386}"
memory="${ZEDBSD_BEUI_MEMORY:-6}"
expected_height="${ZEDBSD_BEUI_EXPECT_HEIGHT:-400}"
backend_name="${ZEDBSD_BEUI_BACKEND_NAME:-GDC}"
test_tag="${ZEDBSD_BEUI_TEST_TAG:-gdc}"
bits_per_pixel="${ZEDBSD_BEUI_BITS_PER_PIXEL:-24}"
base="${ZEDBSD_TEST_BASE_IMAGE:-$releases/linux-pc98-i486dx-debian13-ide.img}"
work="$build/tests/beui-g2a-$test_tag"
image="$work/g2a-ide.raw"
files="$work/files"
cfg="$work/BOOT.CFG"
monitor="$work/monitor.sock"
screenshot="$work/g2a.ppm"

command -v "$qemu" >/dev/null || { echo "QEMU not found: $qemu" >&2; exit 1; }
test -d "$bios_dir" || { echo "PC-98 BIOS directory not found: $bios_dir" >&2; exit 1; }
test -f "$base" || { echo "G2a source image not found: $base" >&2; exit 1; }
for command in mtype python3; do
	command -v "$command" >/dev/null || { echo "$command is required" >&2; exit 1; }
done

mkdir -p "$work" "$files"
cp --reflink=auto "$base" "$image"
case "$bits_per_pixel" in
	8|24) ;;
	*) echo "ZEDBSD_BEUI_BITS_PER_PIXEL must be 8 or 24" >&2; exit 1 ;;
esac
cat > "$files/G2A.NCT" <<'EOF'
func main() {
    if (BeUI.initWithHint(__BEUI_BITS_PER_PIXEL__) != 1) { return 1; }
    if (BeUI.fill(0, 0, 640, 400, 255) != 1) { return 2; }
    if (BeUI.line(0, 0, 639, 399, 16777215) != 1) { return 3; }
    if (BeUI.patternFill(20, 20, 80, 80, 16711680,
                         6172840429334713770L) != 1) { return 4; }
    var file = File.open("G2A.BMP", "rb");
    var bytes = File.read(file, FileUtil.getFileSize("G2A.BMP"));
    File.close(file);
    var image = BeUI.loadImage(bytes);
    if (BeUI.getImageWidth(image) != 80) { return 5; }
    if (BeUI.getImageHeight(image) != 80) { return 5; }
    if (BeUI.drawImage(image, 280, 160) != 1) { return 5; }
    if (BeUI.drawImageRegion(image, 20, 20, 40, 40, 120, 160) != 1) {
        return 6;
    }
    if (BeUI.drawImagePattern(image, 200, 160,
                              6172840429334713770L) != 1) { return 7; }
    BeUI.flush();
    Keyboard.read();
    BeUI.destroyImage(image);
    BeUI.close();
    FileUtil.writeText("G2A.TXT", "BEUI GDC BMP PASS");
    return 0;
}
EOF
sed -i "s/__BEUI_BITS_PER_PIXEL__/$bits_per_pixel/" "$files/G2A.NCT"
printf '/bin/noct /G2A.NCT\n' > "$cfg"

# 80x80, uncompressed, 24-bit BMP. Four exact colors exercise the Cirrus
# packed-color path while the GDC backend quantizes the same source image.
python3 - "$files/G2A.BMP" <<'PY'
import struct
import sys

width = height = 80
stride = (width * 3 + 3) & ~3
offset = 14 + 40
pixels = bytearray(stride * height)
for y in range(height):
    for x in range(width):
        colors = ((255, 0, 0), (0, 255, 0),
                  (0, 0, 255), (255, 255, 255))
        red, green, blue = colors[(x >= 40) + 2 * (y >= 40)]
        position = (height - 1 - y) * stride + x * 3
        pixels[position:position + 3] = bytes((blue, green, red))
header = struct.pack('<2sIHHI', b'BM', offset + len(pixels), 0, 0, offset)
dib = struct.pack('<IiiHHIIiiII', 40, width, height, 1, 24, 0,
                  len(pixels), 0, 0, 0, 0)
with open(sys.argv[1], 'wb') as stream:
    stream.write(header)
    stream.write(dib)
    stream.write(pixels)
PY

"$repo/build.sh" vmunix "$arch"
ZEDBSD_AUTOEXEC="$files/G2A.NCT" ZEDBSD_FILES="$files" \
	ZEDBSD_ZINIT_RC="$cfg" DISK_SECTORS=17 \
	"$repo/scripts/install-image.sh" "$image" "" "$cfg"

offset="$(python3 - "$image" <<'PY'
import struct
import sys

heads = 8
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

rm -f -- "$monitor" "$screenshot"
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

python3 - "$monitor" "$screenshot" "$expected_height" <<'PY'
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

qmp('qmp_capabilities')
started = time.monotonic()
deadline = started + 60
while True:
    qmp('screendump', {'filename': sys.argv[2]})
    try:
        with open(sys.argv[2], 'rb') as capture:
            if capture.readline().strip() != b'P6':
                raise ValueError
            dimensions = capture.readline()
            while dimensions.startswith(b'#'):
                dimensions = capture.readline()
            width, height = map(int, dimensions.split())
            if int(capture.readline()) != 255:
                raise ValueError
            pixels = capture.read()
        colors = {pixels[pos:pos + 3]
                  for pos in range(0, len(pixels), 3)}
        if width >= 640 and height >= int(sys.argv[3]) and len(colors) >= 3:
            break
        if time.monotonic() - started >= 5:
            lit_rows = [pos // (width * 3)
                        for pos in range(0, len(pixels), 3)
                        if pixels[pos:pos + 3] != b'\0\0\0']
            if lit_rows and max(lit_rows) < 40:
                raise SystemExit('compatible BIOS POST exceeded 5 seconds')
    except (FileNotFoundError, ValueError):
        pass
    if time.monotonic() >= deadline:
        raise SystemExit('BeUI display did not become ready')
    time.sleep(1)
qmp('input-send-event', {'events': [
    {'type': 'key', 'data': {'down': True,
      'key': {'type': 'qcode', 'data': 'ret'}}},
    {'type': 'key', 'data': {'down': False,
      'key': {'type': 'qcode', 'data': 'ret'}}},
]})
time.sleep(5)
qmp('quit', wait_reply=False)
stream.close()
client.close()
PY

for _ in $(seq 1 50); do
	if ! kill -0 "$qemu_pid" 2>/dev/null; then
		break
	fi
	sleep .1
done
if kill -0 "$qemu_pid" 2>/dev/null; then
	# A halted guest may not complete the asynchronous QMP quit request.
	# Terminate only the QEMU process started by this test.
	kill "$qemu_pid"
fi
wait "$qemu_pid" 2>/dev/null || true
trap - EXIT INT TERM
rm -f -- "$monitor"

actual="$(mtype -i "$image@@$offset" ::G2A.TXT)"
test "$actual" = 'BEUI GDC BMP PASS' || {
	echo "G2a marker mismatch: '$actual'" >&2
	exit 1
}
python3 - "$screenshot" "$expected_height" <<'PY'
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
expected_height = int(sys.argv[2])
if width < 640 or height < expected_height or len(colors) < 3:
    raise SystemExit(f'display screenshot validation failed: {width}x{height}, {len(colors)} colors')
if expected_height >= 480:
    expected = {b'\xff\0\0', b'\0\xff\0', b'\0\0\xff', b'\xff\xff\xff'}
    if not expected.issubset(colors):
        raise SystemExit(f'24bpp colors missing: {expected - colors}')
PY
printf 'zedBSD BeUI %s/BMP QEMU test: PASS (%s)\n' "$backend_name" "$image"
