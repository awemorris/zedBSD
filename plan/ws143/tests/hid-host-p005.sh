#!/bin/sh
# ws143-p005 i01b: /dev/hid-host on a running zedBSD guest of plan/ws143/tests/config-amd64-bt.mk (build-bt-image.sh: the
# image has hid-host-probe, evdev-probe, systemevents and the test account btuser), without the Bluetooth daemon.  As
# root over SSH; the console and serial logs are not read.
#  1. The node is a character device of root's alone (crw-------).
#  2. hid-host-probe misuse: a report before the setup (EINVAL), a malformed setup (EINVAL) and a declaration after it on
#     the same open, a descriptor of 4097 bytes (EINVAL), a FIDO descriptor (ENXIO), a setup of the wrong size (EINVAL),
#     a short report (EINVAL), a longer one (taken), one of 513 bytes (EINVAL), a read (EAGAIN), poll (POLLOUT only),
#     HID_HOST_GET_DEVICE before (-1) and after (the node, report_max 8), six keyboards at once (with the guest's PS/2
#     and USB devices: more than the old 8 input devices) and a seventh open refused (EBUSY): "HIDHOST PASS".
#  3. hid-host-probe type: a Bluetooth keyboard (bus 5, "Probe Keyboard"); evdev-probe finds its node (the number
#     HID_HOST_GET_DEVICE gave), reads KEY_A 1 and 0 and KEY_B 1, and when the probe closes the file with "b" held,
#     KEY_B 0 and a frame before the node goes.  systemevents sees the node's ADD and REMOVE (subject eventN, bus=5).
#     The node's owner while it lives is recorded (root's 0640 until sessiond gives it to a seat's user).
#  4. btuser (not root) cannot open the node.
# PASS: every "ok" line and the last line hid-host-p005: PASS.
#
#   plan/ws143/tests/build-bt-image.sh BUILD; plan/tools/files/files-guest.sh start BUILD/hdd-image.img
#   plan/ws143/tests/hid-host-p005.sh [OUTDIR]
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
cd "$(dirname -- "$0")/../../.."
export GUEST_RUNTIME="${GUEST_RUNTIME:-$PWD/build/ws071-run}"
. plan/tools/fresh-out.sh
fresh_out "${1:-build/ws143-hid-host-p005}"
out=$fresh_dir
status=0
guest() { timeout 90 python3 plan/tools/guest/guest.py run "$1" 2>&1 | tr -d '\r'; }
ok() { echo "ok: $1"; }
fail() { echo "FAIL: $1${2:+ ($2)}"; status=1; }
expect() { if grep -q "$2" "$3"; then ok "$1"; else fail "$1" "no '$2' in $3"; fi; }

# 1. The node.
guest 'ls -ln /dev/hid-host' > "$out/node.txt"
expect "/dev/hid-host is root's alone" '^crw------- *[0-9]* *0 *0 ' "$out/node.txt"

# 2. The refusals.
guest '/bin/hid-host-probe misuse' > "$out/misuse.txt"
cat "$out/misuse.txt"
expect "hid-host-probe misuse" '^HIDHOST PASS$' "$out/misuse.txt"

# 3. A keyboard typed on, read by evdev-probe, and its events of the system.
guest '/bin/systemevents -c input -n 2 -t 15000 > /tmp/hh-events.txt 2>&1 </dev/null &
/bin/evdev-probe -b bluetooth -n "Probe Keyboard" -N -t 10000 -r 8000 > /tmp/hh-evdev.txt 2>&1 </dev/null &
(sleep 1; ls -ln /dev/input) > /tmp/hh-ls.txt 2>&1 </dev/null &
echo started' > /dev/null
sleep 1
guest '/bin/hid-host-probe type -w 2000 -h 1000' > "$out/type.txt"
cat "$out/type.txt"
expect "the probe typed" '^HIDHOST PASS$' "$out/type.txt"
sleep 2
guest 'cat /tmp/hh-evdev.txt' > "$out/evdev.txt"
guest 'cat /tmp/hh-events.txt' > "$out/events.txt"
guest 'cat /tmp/hh-ls.txt' > "$out/ls.txt"
cat "$out/evdev.txt"
event=$(sed -n 's/^HIDHOST device event=\([0-9]*\) .*/\1/p' "$out/type.txt")
expect "evdev-probe found a Bluetooth keyboard" '^EVDEV node=.* bus=5 vendor=1234 product=5678 .*name="Probe Keyboard"' "$out/evdev.txt"
expect "the node HID_HOST_GET_DEVICE named" "^EVDEV node=/dev/input/event${event:-x} " "$out/evdev.txt"
expect "KEY_A down" '^EVDEV event type=1 code=30 value=1$' "$out/evdev.txt"
expect "KEY_A up" '^EVDEV event type=1 code=30 value=0$' "$out/evdev.txt"
expect "KEY_B down" '^EVDEV event type=1 code=48 value=1$' "$out/evdev.txt"
tail_lines=$(sed -n '/code=48 value=1$/,$p' "$out/evdev.txt" | tr '\n' '|')
case "$tail_lines" in
*"type=1 code=48 value=0|EVDEV event type=0 code=0 value=0|"*"EVDEV gone|"*) ok "KEY_B released when the file closed" ;;
*) fail "KEY_B released when the file closed" "$tail_lines" ;;
esac
expect "the system's ADD" "event .* input add 0 event${event:-x} bus=5" "$out/events.txt"
expect "the system's REMOVE" "event .* input remove 0 event${event:-x} bus=5" "$out/events.txt"
echo "the node's owner while it lived (root's 0640 until a seat's user is given it):"
grep "event${event:-x}\$" "$out/ls.txt" || echo "(not listed)"

# 4. Not for another user.
user=$(guest 'grep -q "^btuser:" /etc/passwd && echo btuser || echo kei' | tail -1)
guest "runas $user /bin/hid-host-probe open 2>&1; true" > "$out/other-user.txt"
expect "$user cannot open it" '^HIDHOST FAIL step=open' "$out/other-user.txt"

echo "outputs in $out"
[ $status = 0 ] && echo "hid-host-p005: PASS" || echo "hid-host-p005: FAIL"
exit $status
