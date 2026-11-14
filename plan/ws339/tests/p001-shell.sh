#!/bin/sh
# Whether OVMF installs the EDID protocols for a display: OVMF is booted with no boot disk into its UEFI shell, and
# "dh -p" lists the handles of the GraphicsOutput, EDID discovered and EDID active protocols (by GUID).  The screen is
# saved after each command as OUT/<n>.ppm, for a person to read; nothing reads the serial line.
#
#   plan/ws339/tests/p001-shell.sh OUT DISPLAY-OPTIONS
#
# QEMU is named p5-ws339; its monitor is a socket in OUT.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
[ $# -ge 2 ] || { echo "usage: p001-shell.sh OUT DISPLAY-OPTIONS" >&2; exit 2; }
out=$(mkdir -p "$1" && cd "$1" && pwd)
display=$2
cp /usr/share/OVMF/OVMF_VARS_4M.fd "$out/vars.fd"
# shellcheck disable=SC2086
timeout 120 qemu-system-x86_64 -name p5-ws339 -machine q35 -m 1G -accel kvm \
	-drive if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd \
	-drive "if=pflash,format=raw,file=$out/vars.fd" \
	-device qemu-xhci,id=xhci -device usb-kbd,bus=xhci.0 \
	-vga none $display -display none -no-reboot -net none \
	-qmp "unix:$out/qmp.sock,server,nowait" > "$out/qemu.log" 2>&1 &
pid=$!
python3 -I - "$out" <<'PY'
import json, socket, sys, time
out = sys.argv[1]
for _ in range(50):
    try:
        s = socket.socket(socket.AF_UNIX); s.connect(out + "/qmp.sock"); break
    except OSError:
        time.sleep(0.2)
f = s.makefile("rwb")
def cmd(name, **args):
    req = {"execute": name}
    if args:
        req["arguments"] = args
    f.write(json.dumps(req).encode() + b"\n"); f.flush()
    while True:
        a = json.loads(f.readline())
        if "return" in a or "error" in a:
            return a
f.readline(); cmd("qmp_capabilities")
names = {" ": "spc", "-": "minus", ".": "dot"}
def typ(text):
    for c in text:
        cmd("send-key", keys=[{"type": "qcode", "data": names.get(c, c)}]); time.sleep(0.03)
    cmd("send-key", keys=[{"type": "qcode", "data": "ret"}])
time.sleep(25)
cmd("screendump", filename=out + "/0.ppm")
for n, guid in enumerate(["9042a9de-23dc-4a38-96fb-7aded080516a",
                          "1c0c34f6-d380-41fa-a049-8ad06c1a66aa",
                          "bd8c1056-9f36-44ec-92a8-a6337f817986"], 1):
    typ("cls"); time.sleep(1)
    typ("dh -p " + guid); time.sleep(3)
    cmd("screendump", filename="%s/%d.ppm" % (out, n))
cmd("quit")
PY
wait $pid 2>/dev/null
sync
echo "p001-shell: screens in $out"
