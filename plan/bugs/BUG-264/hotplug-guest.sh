#!/bin/sh
# BUG-264: an input device plugged in after the login becomes the seat's user's and the session's compositor
# (running as that user, not root) takes it at once.  Reproduces the bug on a tree before the fix and checks the fix.
#
# Guest: the AAT image built from the tree under test (plan/tools/aat/build-image.sh BUILD), started with
#   GUEST_RUNTIME=... plan/ws035/tests/zdesktop-guest.sh start BUILD/hdd-image.img
# and its autologin session of kei (uid 1000, /run/user/1000/session.log) up.  Do not start aat-input first: before
# ws143-p005 i01a the kernel has 8 input devices only (INPUT_DEVICE_MAX), and the injected ones would take them.
#   plan/bugs/BUG-264/hotplug-guest.sh [OUTDIR]
#
#  0. The session's compositor is up (KWL READY ... role=normal); sessiond hears the devices' events
#     ("SESSIOND SEAT events subscribed" in /var/log/sessiond.log; missing before the fix).
#  1. QMP device_add usb-kbd (id bug264kbd).  A watcher on the guest (root) looks every 0.1 s (a "tick") for the new
#     /dev/input/eventN, its owner, and the compositor's "KWL INPUT device=/dev/input/eventN kind=keyboard".
#     Pass: the node is uid 1000 mode crw------- and the compositor took it within 10 ticks of the node's appearance.
#     Before the fix sessiond gives the node within about a second and the compositor takes it only at its next
#     ordinary scan (KWL_INPUT_SCAN_MS 2 s): "taken-in-time" fails with about 20 ticks.
#  2. QMP device_del: the compositor closes it ("KWL INPUT_CLOSED device=/dev/input/eventN").
#  3. The same with usb-mouse (id bug264mouse, "kind=pointer").
#  4. The compositor is still up.
# Judged from the guest's files over SSH and QMP only (no console or serial log).  The last line is
# "bug264-hotplug: PASS" or "bug264-hotplug: FAIL".
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
cd "$(dirname -- "$0")/../../.."
export GUEST_RUNTIME="${GUEST_RUNTIME:-$PWD/build/ws035-sq-run}"
. plan/tools/fresh-out.sh
fresh_out "${1:-build/bug264-hotplug}"
out=$fresh_dir
qmp="$GUEST_RUNTIME/qmp.sock"
session=/run/user/1000/session.log
status=0
pass() { echo "$1: ok"; }
fail() { echo "$1: FAILED${2:+ ($2)}"; status=1; }
guest() { timeout 120 python3 plan/tools/guest/guest.py run "$1" 2>&1 </dev/null; }
put() { timeout 90 python3 plan/tools/guest/guest.py put "$1" "$2" >/dev/null 2>&1; }

# Sends one QMP command (its arguments as JSON) and keeps the answer; events QEMU sends meanwhile are skipped.
qmp() {
	timeout 40 python3 -I -c '
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
answer = ask({"execute": sys.argv[2], "arguments": json.loads(sys.argv[3])})
print(json.dumps(answer))
sys.exit(1 if "error" in answer else 0)
' "$qmp" "$1" "$2" >> "$out/qmp.txt" 2>&1
}

# The watcher, run on the guest as root: prints NODE, OWNED and TAKEN with the tick each was first seen.
cat > "$out/watch.sh" <<'WATCH'
kind=$1
log=$2
before=$(ls /dev/input | grep '^event')
echo "BEFORE $(echo $before)"
tick=0; node=; owned=; taken=
while [ $tick -lt 150 ]; do
	if [ -z "$node" ]; then
		for name in $(ls /dev/input | grep '^event'); do
			echo "$before" | grep -qx "$name" || node=$name
		done
		[ -n "$node" ] && echo "NODE $node tick=$tick"
	fi
	if [ -n "$node" ] && [ -z "$owned" ]; then
		set -- $(ls -ln /dev/input/$node 2>/dev/null)
		if [ "${3:-}" = 1000 ]; then owned=$tick; echo "OWNED $node tick=$tick mode=$1 uid=$3 gid=$4"; fi
	fi
	if [ -n "$node" ] && [ -z "$taken" ] && grep -q "KWL INPUT device=/dev/input/$node kind=$kind" "$log"; then
		taken=$tick; echo "TAKEN $node tick=$tick"
	fi
	[ -n "$owned" ] && [ -n "$taken" ] && break
	sleep 0.1
	tick=$((tick + 1))
done
echo "DONE tick=$tick"
ls -ln /dev/input
WATCH
put "$out/watch.sh" /tmp/bug264-watch.sh

# 0. The session and sessiond's subscription.
guest "grep 'KWL READY' $session; ls -ln /dev/input; grep 'SESSIOND SEAT events' /var/log/sessiond.log" > "$out/before.txt"
if grep -q 'role=normal' "$out/before.txt"; then pass session-up; else fail session-up "no KWL READY role=normal in $session"; fi
if grep -q 'SESSIOND SEAT events subscribed' "$out/before.txt"; then pass sessiond-subscribed; else fail sessiond-subscribed; fi
nodes=$(grep -c ' event[0-9]' "$out/before.txt")
echo "input nodes before: $nodes"
[ "$nodes" -ge 8 ] && echo "note: 8 nodes already (the kernel's INPUT_DEVICE_MAX before ws143-p005 i01a); a plugged device may get ENOSPC"

# Plugs one device, watches it come, and pulls it out: plug DRIVER ID KIND
plug() {
	guest "sh /tmp/bug264-watch.sh $3 $session > /tmp/bug264-$2.txt 2>&1 </dev/null &" >/dev/null
	sleep 1
	if qmp device_add "{\"driver\":\"$1\",\"bus\":\"xhci.0\",\"id\":\"$2\"}"; then pass "$2-added"; else fail "$2-added" "QMP"; fi
	sleep 16
	guest "cat /tmp/bug264-$2.txt" > "$out/$2.txt"
	node=$(sed -n 's/^NODE \(event[0-9]*\) tick=.*/\1/p' "$out/$2.txt")
	seen=$(sed -n 's/^NODE .* tick=\([0-9]*\)/\1/p' "$out/$2.txt")
	owned=$(sed -n 's/^OWNED .* tick=\([0-9]*\) .*/\1/p' "$out/$2.txt")
	taken=$(sed -n 's/^TAKEN .* tick=\([0-9]*\)/\1/p' "$out/$2.txt")
	echo "$2: node=${node:-none} appeared=${seen:-never} owned=${owned:-never} taken=${taken:-never} (ticks of 0.1 s)"
	if [ -z "$node" ]; then
		fail "$2-node" "no new eventN within 15 s"
		return
	fi
	pass "$2-node"
	if grep -q "^OWNED $node .*mode=crw------- uid=1000" "$out/$2.txt"; then pass "$2-owned"; else fail "$2-owned" "not uid 1000 0600"; fi
	if [ -n "$taken" ] && [ $((taken - seen)) -le 10 ]; then
		pass "$2-taken-in-time"
	else
		fail "$2-taken-in-time" "taken ${taken:-never}, node at $seen"
	fi
	if qmp device_del "{\"id\":\"$2\"}"; then pass "$2-deleted"; else fail "$2-deleted" "QMP"; fi
	sleep 3
	if guest "grep 'KWL INPUT_CLOSED device=/dev/input/$node' $session" | grep -q INPUT_CLOSED; then pass "$2-closed"; else fail "$2-closed"; fi
}

# 1-3. A keyboard, then a mouse.
plug usb-kbd bug264kbd keyboard
plug usb-mouse bug264mouse pointer

# 4. The compositor is still up; sessiond's lines for the record.
guest "ps -A -o args; grep 'SESSIOND SEAT' /var/log/sessiond.log | tail -20; grep -E 'KWL (EVENT input|INPUT)' $session | tail -20" > "$out/after.txt"
if grep -qE '(^|/)[w]ayland( |$)' "$out/after.txt"; then pass compositor-alive; else fail compositor-alive; fi

echo "outputs in $out"
if [ $status = 0 ]; then echo "bug264-hotplug: PASS"; else echo "bug264-hotplug: FAIL"; fi
exit $status
