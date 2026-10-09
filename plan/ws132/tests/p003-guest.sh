#!/bin/sh
# ws132-p003 on the desktop test guest (plan/ws079/tests/config-amd64-pen.mk built from this branch: a kernel with the
# system's events of ws132-p002, started with plan/ws079/tests/pen-guest.sh start IMAGE).  The compositor under test
# (BUILD/bin/wayland) is copied into the running guest; the output is 1280x800.  q35 has no battery and no AC adapter.
#  1. The compositor's backend subscribes to /dev/system ("KL EVENTS subscribed classes=0xaf") and reads the power
#     as unknown ("KWL POWER source=unknown percent=-1"): no battery on the bar (bar.png is for the eye: no battery
#     outline left of the clock).
#  2. A USB keyboard plugged in through QMP (usb-kbd on xhci.0, on a free port QEMU chooses: the pen harness takes ports 1-4, and the
#     controller has 8 since T1-125): the compositor hears "KWL EVENT input changed" and takes the keyboard ("KWL INPUT device=... kind=keyboard") within 1.5 s, before its own 2 s scan would.
#  3. The keyboards still work after the plug: App Home opened with the mouse (the launcher), then Esc closes it
#     ("KWL HOME close via=escape").  QEMU routes the key to its first keyboard: input-send-event with "device"
#     (the plugged keyboard only) aborts QEMU on this guest, whose text console has no "device" property
#     (T1-129: "Property 'qemu-fixed-text-console.device' not found").  That the plugged keyboard's own keys
#     arrive is left to the UAT; here it is taken and opened (2).
#  4. The keyboard pulled out: "KWL EVENT input changed" again, and its device closes.
#  5. The compositor is still up, with no ERROR in its log.
#
#   plan/ws079/tests/pen-guest.sh start IMAGE
#   plan/ws132/tests/p003-guest.sh BUILD [OUTDIR]
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
cd "$(dirname -- "$0")/../../.."
export GUEST_RUNTIME="${GUEST_RUNTIME:-$PWD/build/ws079-run}"
build=${1:?usage: p003-guest.sh BUILD [OUTDIR]}
out=${2:-build/ws132-p003}
mkdir -p "$out"
qmp="$GUEST_RUNTIME/qmp.sock"
guest() { timeout 120 python3 plan/tools/guest/guest.py run "$1" 2>&1; }
put() { timeout 90 python3 plan/tools/guest/guest.py put "$1" "$2" >/dev/null 2>&1; }
send() { timeout 40 python3 plan/ws049/tests/qmp-send.py "$qmp" "$@" >> "$out/qmp.txt" 2>&1; }
# A picture for the eye, read from the Venus head through QEMU's VNC (QMP screendump shows the text console instead).
shot() { timeout 60 python3 plan/ws035/tests/zdesktop-check.py "$out/$1.png" --runtime "$GUEST_RUNTIME" >> "$out/qmp.txt" 2>&1; }
pointer() { python3 plan/ws035/tests/qmp-pointer.py "$qmp" "$@"; }
stop_all='for p in $(ps -A -o pid,args | grep -E "[w]ayland( |$)|[w]ltest|[w]lshm" | awk "{print \$1}"); do kill $p; done; i=0; while ps -A -o args | grep -qE "[w]ayland( |$)" && [ $i -lt 50 ]; do sleep 0.2; i=$((i+1)); done'
status=0
pass() { echo "$1: ok"; }
fail() { echo "$1: FAILED"; status=1; }
expect_log() {
	if guest "grep -E '$2' /tmp/zdesktop.log" | grep -Eq "$2"; then pass "$1"; else fail "$1"; fi
}
: > "$out/qmp.txt"

# The compositor under test.
guest "$stop_all" >/dev/null
put "$build/bin/wayland" /bin/wayland
guest 'chmod 755 /bin/wayland; export XDG_RUNTIME_DIR=/tmp; rm -f /tmp/wayland-0
/bin/wayland --testing --timeout=600 --width=1280 --height=800 --glass > /tmp/zdesktop.log 2>&1 </dev/null & for w in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20; do grep -q KWL.READY /tmp/zdesktop.log 2>/dev/null && break; sleep 0.5; done; sleep 2; echo started' >/dev/null

# 1. The subscription and the power.
expect_log subscribed 'KL EVENTS subscribed classes=0xaf'
expect_log power-unknown 'KWL POWER source=unknown percent=-1 charging=0'
shot bar

# 2. The keyboard plugged in; the compositor's scan of 2 s is beaten by the event.
keyboards_before=$(guest "grep -c 'kind=keyboard' /tmp/zdesktop.log" | tail -1)
send device_add '{"driver":"usb-kbd","bus":"xhci.0","id":"hotkbd"}'
sleep 1.5
guest 'cat /tmp/zdesktop.log' > "$out/after-plug.log"
if grep -q 'KWL EVENT input changed' "$out/after-plug.log"; then pass plug-event; else fail plug-event; fi
keyboards_after=$(grep -c 'kind=keyboard' "$out/after-plug.log")
if [ "${keyboards_after:-0}" -gt "${keyboards_before:-0}" ]; then pass plug-keyboard-taken; else fail plug-keyboard-taken; fi

# 3. The new keyboard works: Home opened with the mouse, closed with Esc on the plugged keyboard only.
pointer move 23 17 sleep 300 down sleep 60 up sleep 1200 >/dev/null
pointer move 700 500 sleep 400 >/dev/null
expect_log home-opened 'KWL HOME opened'
send input-send-event '{"events":[{"type":"key","data":{"down":true,"key":{"type":"qcode","data":"esc"}}}]}'
sleep 0.1
send input-send-event '{"events":[{"type":"key","data":{"down":false,"key":{"type":"qcode","data":"esc"}}}]}'
sleep 1
expect_log escape-after-plug 'KWL HOME close via=escape'

# 4. The keyboard pulled out.
changes_before=$(guest "grep -c 'KWL EVENT input changed' /tmp/zdesktop.log" | tail -1)
send device_del '{"id":"hotkbd"}'
sleep 2
guest 'cat /tmp/zdesktop.log' > "$out/after-unplug.log"
changes_after=$(grep -c 'KWL EVENT input changed' "$out/after-unplug.log")
if [ "${changes_after:-0}" -gt "${changes_before:-0}" ]; then pass unplug-event; else fail unplug-event; fi
if grep -q 'KWL INPUT_CLOSED device=' "$out/after-unplug.log"; then pass unplug-closed; else fail unplug-closed; fi

# 5. Still up, no ERROR.
if guest 'ps -A -o args' | grep -qE '[w]ayland( |$)'; then pass alive; else fail alive; fi
if grep -q 'ERROR' "$out/after-unplug.log"; then fail no-error; else pass no-error; fi

echo "p003-guest: status $status (outputs in $out; bar.png for the eye)"
exit $status
