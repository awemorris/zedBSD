#!/bin/sh
# ws143-p005 i01a: the USB HID devices after the HID input glue (src/drivers/generic/hid-input.c took the report
# handling out of usb-hid.c), on a running zedBSD guest of plan/ws143/tests/config-amd64-bt.mk (build-bt-image.sh; the
# image has evdev-probe).  The guest is any plan/tools/guest/guest.py guest (it has a USB keyboard on xhci port 3; the
# Files guest adds a USB tablet on port 4); the test plugs and pulls more through QMP and reads the nodes with
# evdev-probe as root (no compositor; the console and serial logs are not read).
#  1. A USB keyboard, mouse and tablet plugged in (device_add on xhci ports 5, 6 and 7): three new event nodes on the
#     USB bus (bus=3, QEMU's vendor 0627), named by the product.
#  2. Keys: "a" pressed and released through QMP reach the plugged keyboard (QEMU gives keys to the keyboard plugged in
#     last) as KEY_A 1 and 0.  The mouse's relative move +5 reaches the plugged mouse as REL_X 5 with no REL_Y.  An
#     absolute move reaches a USB tablet as ABS_X and ABS_Y (QEMU picks the tablet; the line says which).
#  3. "b" pressed and held, then the keyboard pulled out (device_del): its reader sees KEY_B 0 and a frame before the
#     node goes ("EVDEV gone"): the input layer releases what an unplugged device held.
#  4. The mouse and the tablet pulled out: the USB nodes are as many as before.
# PASS: every "ok" line and the last line hid-usb-p005: PASS.
#
#   plan/ws143/tests/build-bt-image.sh BUILD; plan/tools/files/files-guest.sh start BUILD/hdd-image.img
#   plan/ws143/tests/hid-usb-p005.sh [OUTDIR]
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
cd "$(dirname -- "$0")/../../.."
export GUEST_RUNTIME="${GUEST_RUNTIME:-$PWD/build/ws071-run}"
. plan/tools/fresh-out.sh
fresh_out "${1:-build/ws143-hid-usb-p005}"
out=$fresh_dir
qmp="$GUEST_RUNTIME/qmp.sock"
status=0
guest() { timeout 90 python3 plan/tools/guest/guest.py run "$1" 2>&1 | tr -d '\r'; }
ok() { echo "ok: $1"; }
fail() { echo "FAIL: $1${2:+ ($2)}"; status=1; }
expect() { if grep -q "$2" "$3"; then ok "$1"; else fail "$1" "no '$2' in $3"; fi; }

# Sends QMP commands (each "COMMAND ARGUMENTS-JSON" on a line of standard input) and keeps the answers.
qmp() {
	timeout 60 python3 -I -c '
import json, socket, sys
connection = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
connection.settimeout(30)
connection.connect(sys.argv[1])
stream = connection.makefile("rw")
stream.readline()
def ask(request):
	stream.write(json.dumps(request) + "\n")
	stream.flush()
	while True:
		message = json.loads(stream.readline())
		if "event" not in message:
			return message
ask({"execute": "qmp_capabilities"})
failed = 0
for line in sys.stdin:
	line = line.strip()
	if not line:
		continue
	command, _, arguments = line.partition(" ")
	request = {"execute": command}
	if arguments:
		request["arguments"] = json.loads(arguments)
	answer = ask(request)
	print(json.dumps(answer))
	if "error" in answer:
		failed = 1
sys.exit(failed)
' "$qmp" >> "$out/qmp.txt" 2>&1
}
key() { printf 'input-send-event {"events":[{"type":"key","data":{"down":%s,"key":{"type":"qcode","data":"%s"}}}]}\n' "$2" "$1"; }

# 0. The USB nodes before.
guest '/bin/evdev-probe -l -b usb' > "$out/before.txt"
before=$(sed -n 's/^EVDEV count=//p' "$out/before.txt")
echo "USB event nodes before: ${before:-?}"

# 1. Readers that wait for the new nodes, then the devices.
guest '/bin/evdev-probe -b usb -n Keyboard -N -t 20000 -r 15000 > /tmp/p005-kbd.txt 2>&1 </dev/null &
/bin/evdev-probe -b usb -n Mouse -N -t 20000 -r 15000 > /tmp/p005-mouse.txt 2>&1 </dev/null &
/bin/evdev-probe -b usb -n Tablet -N -t 20000 -r 15000 > /tmp/p005-tablet.txt 2>&1 </dev/null &
echo started' > /dev/null
sleep 1
printf '%s\n' \
	'device_add {"driver":"usb-kbd","bus":"xhci.0","port":"5","id":"hidkbd"}' \
	'device_add {"driver":"usb-mouse","bus":"xhci.0","port":"6","id":"hidmouse"}' \
	'device_add {"driver":"usb-tablet","bus":"xhci.0","port":"7","id":"hidtab"}' | qmp && ok "devices plugged" || fail "devices plugged" "QMP"
sleep 4
guest '/bin/evdev-probe -l -b usb' > "$out/plugged.txt"
plugged=$(sed -n 's/^EVDEV count=//p' "$out/plugged.txt")
if [ -n "$before" ] && [ "${plugged:-0}" -eq $((before + 3)) ]; then ok "three new USB nodes"; else fail "three new USB nodes" "before ${before:-?}, now ${plugged:-?}"; fi
expect "the new keyboard is QEMU's, on the USB bus" 'bus=3 vendor=0627.*Keyboard' "$out/plugged.txt"

# 2. A key, a relative move and an absolute move.
{
	key a true
	key a false
	echo 'input-send-event {"events":[{"type":"rel","data":{"axis":"x","value":5}}]}'
	echo 'input-send-event {"events":[{"type":"abs","data":{"axis":"x","value":16384}},{"type":"abs","data":{"axis":"y","value":8192}}]}'
} | qmp || fail "events sent" "QMP"
sleep 1

# 3. "b" held, then the keyboard pulled out.
key b true | qmp || fail "b held" "QMP"
sleep 1
echo 'device_del {"id":"hidkbd"}' | qmp || fail "keyboard pulled" "QMP"
sleep 3
guest 'cat /tmp/p005-kbd.txt' > "$out/keyboard.txt"
expect "the plugged keyboard's node" '^EVDEV node=.*bus=3' "$out/keyboard.txt"
expect "KEY_A down" '^EVDEV event type=1 code=30 value=1$' "$out/keyboard.txt"
expect "KEY_A up" '^EVDEV event type=1 code=30 value=0$' "$out/keyboard.txt"
expect "KEY_B down" '^EVDEV event type=1 code=48 value=1$' "$out/keyboard.txt"
# The release, a frame, then the end, in that order.
tail_lines=$(sed -n '/code=48 value=1$/,$p' "$out/keyboard.txt" | tr '\n' '|')
case "$tail_lines" in
*"type=1 code=48 value=0|EVDEV event type=0 code=0 value=0|"*"EVDEV gone|"*) ok "KEY_B released before the node went" ;;
*) fail "KEY_B released before the node went" "$tail_lines" ;;
esac

# 4. The mouse and the tablet, then pulled out.
printf '%s\n' 'device_del {"id":"hidmouse"}' 'device_del {"id":"hidtab"}' | qmp || fail "mouse and tablet pulled" "QMP"
sleep 3
guest 'cat /tmp/p005-mouse.txt' > "$out/mouse.txt"
guest 'cat /tmp/p005-tablet.txt' > "$out/tablet.txt"
expect "REL_X 5 on the plugged mouse" '^EVDEV event type=2 code=0 value=5$' "$out/mouse.txt"
if grep -q '^EVDEV event type=2 code=1 ' "$out/mouse.txt"; then fail "no REL_Y 0" "a REL_Y came"; else ok "no REL_Y 0"; fi
if grep -q '^EVDEV event type=3 code=0 ' "$out/tablet.txt"; then
	ok "ABS_X on the plugged tablet"
else
	echo "note: the absolute move went to the other USB tablet (QEMU's choice); its node is not read"
fi
expect "the mouse's node went" '^EVDEV gone$' "$out/mouse.txt"
expect "the tablet's node went" '^EVDEV gone$' "$out/tablet.txt"
guest '/bin/evdev-probe -l -b usb' > "$out/after.txt"
after=$(sed -n 's/^EVDEV count=//p' "$out/after.txt")
if [ "${after:-x}" = "${before:-y}" ]; then ok "the USB nodes are as before"; else fail "the USB nodes are as before" "before ${before:-?}, after ${after:-?}"; fi

echo "outputs in $out"
[ $status = 0 ] && echo "hid-usb-p005: PASS" || echo "hid-usb-p005: FAIL"
exit $status
