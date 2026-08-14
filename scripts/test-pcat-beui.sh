#!/usr/bin/env bash
set -euo pipefail

# Exercise /dev/graphics, ASCII glyphs, and text restoration through the
# actual Noct/BeUI user process on both PC/AT display backends.
repo="$(cd "$(dirname "$0")/.." && pwd)"
image="${1:-$repo/build/unified/hdd-image.img}"
qemu="${QEMU_PCAT_I386:-qemu-system-i386}"
work="$repo/build/pcat/tests/beui"
offset=$((2048 * 512))

test -f "$image" || { echo "unified image not found: $image" >&2; exit 1; }
command -v "$qemu" >/dev/null
mkdir -p "$work/files"

cat >"$work/files/GFX.NCT" <<'EOF'
func main() {
    if (BeUI.initWithHint(8) != 1) {
        FileUtil.writeText("/GFXFAIL.TXT", "BeUI.initWithHint failed");
        return 1;
    }
    BeUI.fill(0, 0, BeUI.getWidth(), BeUI.getHeight(), 0x102040);
    BeUI.fill(48, 64, 240, 128, 0xc04020);
    BeUI.patternFill(320, 64, 240, 128, 0x20c060,
                     6172840429334713770L);
    BeUI.line(0, 0, 639, 479, 0xffffff);
    BeUI.drawText("zedBSD PC/AT BeUI", 64, 240, 0xffffff, 0x102040);
    BeUI.flush();
    FileUtil.writeText("/GFXREADY.TXT", "PCAT BEUI READY");
    Keyboard.read();
    BeUI.close();
    FileUtil.writeText("/GFXPASS.TXT", "PCAT BEUI PASS");
    return 0;
}
EOF
printf '/bin/noct /GFX.NCT\nhalt\n' >"$work/files/ZINIT.RC"

run_backend()
{
	local vga="$1" marker="$2"
	local disk="$work/$vga.img" monitor="$work/$vga.sock"
	local log="$work/$vga.log" shot="$work/$vga.ppm"
	cp --reflink=auto -f "$image" "$disk"
	mmd -i "$disk@@$offset" ::/etc 2>/dev/null || true
	mcopy -o -i "$disk@@$offset" "$work/files/GFX.NCT" ::/GFX.NCT
	mcopy -o -i "$disk@@$offset" "$work/files/ZINIT.RC" ::/etc/zinit.rc
	rm -f -- "$monitor" "$log" "$shot"
	"$qemu" -M pc -cpu 486 -m 64 -accel tcg -nic none -vga "$vga" \
		-display none -serial none -monitor none -no-reboot \
		-debugcon "file:$log" -global isa-debugcon.iobase=0xe9 \
		-qmp "unix:$monitor,server=on,wait=off" \
		-drive "if=ide,format=raw,file=$disk" >/dev/null 2>&1 &
	local pid=$!
	trap 'kill "$pid" 2>/dev/null || true; wait "$pid" 2>/dev/null || true' RETURN
	python3 - "$monitor" "$log" "$shot" "$marker" <<'PY'
import json
import socket
import sys
import time

monitor, log, shot, marker = sys.argv[1:]
text_shot = shot + ".text.ppm"
client = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
deadline = time.monotonic() + 15
while True:
    try:
        client.connect(monitor)
        break
    except (FileNotFoundError, ConnectionRefusedError):
        if time.monotonic() >= deadline:
            raise SystemExit("PC/AT BeUI QMP did not become ready")
        time.sleep(.1)
stream = client.makefile("rwb", buffering=0)
json.loads(stream.readline())

def qmp(command, arguments=None, reply=True):
    request = {"execute": command}
    if arguments is not None:
        request["arguments"] = arguments
    stream.write(json.dumps(request).encode("ascii") + b"\n")
    if not reply:
        return
    while True:
        response = json.loads(stream.readline())
        if "return" in response:
            return response["return"]
        if "error" in response:
            raise SystemExit(f"QMP {command} failed: {response['error']}")

qmp("qmp_capabilities")
deadline = time.monotonic() + 75
while time.monotonic() < deadline:
    try:
        text = open(log, encoding="utf-8", errors="replace").read()
    except FileNotFoundError:
        text = ""
    if marker in text:
        qmp("screendump", {"filename": shot})
        try:
            with open(shot, "rb") as capture:
                if capture.readline().strip() != b"P6":
                    raise ValueError
                dimensions = capture.readline()
                while dimensions.startswith(b"#"):
                    dimensions = capture.readline()
                width, height = map(int, dimensions.split())
                if int(capture.readline()) != 255:
                    raise ValueError
                pixels = capture.read()
            colors = {pixels[pos:pos + 3]
                      for pos in range(0, len(pixels), 3)}
            if width == 640 and height == 480 and len(colors) >= 4:
                break
        except (FileNotFoundError, ValueError):
            pass
    time.sleep(.25)
else:
    raise SystemExit(f"graphics marker or screenshot missing: {marker}")

qmp("input-send-event", {"events": [
    {"type": "key", "data": {"down": True,
      "key": {"type": "qcode", "data": "ret"}}},
    {"type": "key", "data": {"down": False,
      "key": {"type": "qcode", "data": "ret"}}},
]})
deadline = time.monotonic() + 10
while time.monotonic() < deadline:
    try:
        text = open(log, encoding="utf-8", errors="replace").read()
    except FileNotFoundError:
        text = ""
    if "graphics: PC/AT text mode restored" in text:
        break
    time.sleep(.1)
else:
    raise SystemExit("PC/AT text mode was not restored")
time.sleep(1)
qmp("screendump", {"filename": text_shot})
with open(text_shot, "rb") as capture:
    if capture.readline().strip() != b"P6":
        raise SystemExit("restored console screenshot is not PPM")
    dimensions = capture.readline()
    while dimensions.startswith(b"#"):
        dimensions = capture.readline()
    width, height = map(int, dimensions.split())
    if int(capture.readline()) != 255:
        raise SystemExit("restored console screenshot has invalid depth")
    pixels = capture.read()
if width < 640 or height < 400 or len(set(
        pixels[pos:pos + 3] for pos in range(0, len(pixels), 3))) < 2:
    raise SystemExit("restored text console is blank or malformed")
qmp("quit", reply=False)
stream.close()
client.close()
PY
	for _ in $(seq 1 50); do
		kill -0 "$pid" 2>/dev/null || break
		sleep .1
	done
	if kill -0 "$pid" 2>/dev/null; then kill "$pid"; fi
	wait "$pid" 2>/dev/null || true
	trap - RETURN
	test "$(mtype -i "$disk@@$offset" ::/GFXPASS.TXT)" = "PCAT BEUI PASS"
	grep -F "$marker" "$log" >/dev/null
	echo "PC/AT BeUI $vga: PASS"
}

run_backend cirrus "graphics: PC/AT Cirrus 640x480x8"
run_backend std "graphics: PC/AT VGA fallback 640x480x4 planar"
