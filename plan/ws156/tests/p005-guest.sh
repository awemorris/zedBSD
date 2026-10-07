#!/bin/sh
# ws156-p005: the notifications' popup (p003) and log (p004) on the zdesktop guest (QEMU, Venus).
# Image: ZEDBSD_CONFIG=plan/ws156/tests/config-amd64-notify.mk (the zdesktop image with keiland-notify and wltest),
# booted with plan/ws035/tests/zdesktop-guest.sh start IMAGE.  The compositor under test (BUILD/bin/wayland) is copied
# in and started 1280x800 with --testing --glass; keiland-notify posts as root (the compositor's user).  The board is a
# fifth of 1280 wide (320), 76 high, 48 above the bottom: x 480..800, y 676..752; its close sign's square is
# x 770..800, y 676..706.  Keys through QMP, the pointer through qmp-pointer.py.
#  1. One notification: "KWL NOTIFY show id=1", the board entering, staying (popup-stay.png), leaving; "hide id=1" and
#     "gone id=1" within 5 s.
#  2. Three at once: shown one after the other, in order; all gone within 10 s (the stay is 1.5 s while others wait).
#  3. The close sign: "dismiss id=" and "closed ... reason=1"; it is not in the log afterwards.
#  4. An ACTION notification's body: "activate id=" and the client's "KEILAND-NOTIFY activated".
#  5. The pointer on the board holds it: no "hide" for 5 s; it leaves after the pointer goes.
#  6. A fullscreen window (wltest --fullscreen-at=1): a notification is not shown ("skip id=... reason=fullscreen");
#     an --urgent one is ("show ... urgent=1").
#  7. The log: Super+N opens it on the newest ("log open count=7", log.png), Right steps to the older ("log at
#     place=2"), Left twice reaches the clearing board ("log at place=0 clear", log-clear.png), Enter clears ("log
#     clear count=7"), the board says "No notifications" (log-empty.png), Esc closes ("log close reason=esc").
#  8. The compositor stays up, with no ERROR in its log.
# The ids are not assumed: notify() reads each one from the client's "KEILAND-NOTIFY posted ... id=N" (posts.txt).
#   plan/ws156/tests/p005-guest.sh BUILD [OUTDIR]
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
cd "$(dirname -- "$0")/../../.."
export GUEST_RUNTIME="${GUEST_RUNTIME:-$PWD/build/ws035-sq-run}"
build=${1:?usage: p005-guest.sh BUILD [OUTDIR]}
out=${2:-build/ws156-p005}
mkdir -p "$out"
qmp="$GUEST_RUNTIME/qmp.sock"
guest() { timeout 120 python3 plan/tools/guest/guest.py run "$1" 2>&1; }
put() { timeout 90 python3 plan/tools/guest/guest.py put "$1" "$2" >/dev/null 2>&1; }
send() { timeout 40 python3 plan/ws049/tests/qmp-send.py "$qmp" "$@" >> "$out/qmp.txt" 2>&1; }
shot() { timeout 60 python3 plan/ws035/tests/zdesktop-check.py "$out/$1.png" --runtime "$GUEST_RUNTIME" >> "$out/qmp.txt" 2>&1; }
pointer() { python3 plan/ws035/tests/qmp-pointer.py "$qmp" "$@" >/dev/null; }
key() { send input-send-event "{\"events\":[{\"type\":\"key\",\"data\":{\"down\":$2,\"key\":{\"type\":\"qcode\",\"data\":\"$1\"}}}]}"; }
tap() { key "$1" true; sleep 0.12; key "$1" false; sleep 0.6; }
count() { guest "grep -c -- '$1' /tmp/zdesktop.log" | tail -1; }
wait_for() { i=0; while [ "$(count "$1")" -lt "$2" ] 2>/dev/null && [ $i -lt "$3" ]; do sleep 0.5; i=$((i+1)); done; }
env='export XDG_RUNTIME_DIR=/tmp WAYLAND_DISPLAY=wayland-0;'
# Quotes one word for the guest's shell (a ' inside becomes '\'').
quote() { printf "'%s'" "$(printf '%s' "$1" | sed "s/'/'\\\\''/g")"; }
# Posts one notification and waits for the client's "posted ... id=N"; the compositor's id goes to $id (0 if none came).
notify() {
	words=''; for w in "$@"; do words="$words $(quote "$w")"; done
	before=$(guest "grep -c 'KEILAND-NOTIFY posted' /tmp/notify-client.log 2>/dev/null || true" | tail -1)
	guest "$env /bin/keiland-notify$words >> /tmp/notify-client.log 2>&1 </dev/null & echo posted" >/dev/null
	i=0; while [ "$(guest "grep -c 'KEILAND-NOTIFY posted' /tmp/notify-client.log 2>/dev/null || true" | tail -1)" -le "${before:-0}" ] 2>/dev/null && [ $i -lt 20 ]; do sleep 0.3; i=$((i+1)); done
	id=$(guest "grep 'KEILAND-NOTIFY posted' /tmp/notify-client.log | tail -1 | sed 's/.*id=//'" | tail -1)
	case "$id" in ''|*[!0-9]*) id=0 ;; esac
	echo "posted id=$id: $*" >> "$out/posts.txt"
}
stop_all='for p in $(ps -A -o pid,args | grep -E "[w]ayland( |$)|[w]ltest|[k]eiland-notify" | awk "{print \$1}"); do kill $p; done; i=0; while ps -A -o args | grep -qE "[w]ayland( |$)|[w]ltest" && [ $i -lt 50 ]; do sleep 0.2; i=$((i+1)); done'
status=0
pass() { echo "$1: ok"; }
fail() { echo "$1: FAILED"; status=1; }
expect_count() { n=$(count "$2"); if [ "${n:-0}" -eq "$3" ] 2>/dev/null; then pass "$1"; else fail "$1 ($2: ${n:-?}, expected $3)"; fi; }
: > "$out/qmp.txt"; : > "$out/posts.txt"

# The compositor under test.
guest "$stop_all" >/dev/null
put "$build/bin/wayland" /bin/wayland
guest 'chmod 755 /bin/wayland; export XDG_RUNTIME_DIR=/tmp; rm -f /tmp/wayland-0 /tmp/notify-client.log
/bin/wayland --testing --timeout=900 --width=1280 --height=800 --glass > /tmp/zdesktop.log 2>&1 </dev/null & for w in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20; do grep -q KWL.READY /tmp/zdesktop.log 2>/dev/null && break; sleep 0.5; done; sleep 2; echo started' >/dev/null
pointer move 100 100 sleep 300

# 1. One notification enters, stays, leaves, and is logged.
notify --app=Test --wait-ms=200 Hello "The first notification's body, long enough to wrap onto its second line on the board"
one=$id
wait_for "KWL NOTIFY show id=$one " 1 10
sleep 1.2
shot popup-stay
wait_for "KWL NOTIFY gone id=$one\$" 1 12
expect_count one-shown "KWL NOTIFY show id=$one " 1
expect_count one-logged "KWL NOTIFY hide id=$one\$" 1
expect_count one-gone "KWL NOTIFY gone id=$one\$" 1

# 2. Three at once: one after the other, in order.
notify --wait-ms=200 One
first=$id
notify --wait-ms=200 Two
second=$id
notify --wait-ms=200 Three
third=$id
wait_for "KWL NOTIFY gone id=$third\$" 1 24
guest "grep -o 'KWL NOTIFY show id=[0-9]*' /tmp/zdesktop.log | tr '\n' ' '" > "$out/order.txt"
grep -q "show id=$first KWL NOTIFY show id=$second KWL NOTIFY show id=$third" "$out/order.txt" && pass three-in-order || fail "three-in-order ($first $second $third: $(cat "$out/order.txt"))"
expect_count three-gone "KWL NOTIFY gone id=$third\$" 1

# 3. The close sign dismisses: not logged.
notify --wait-ms=8000 Close-me
close=$id
wait_for "KWL NOTIFY show id=$close " 1 10
sleep 0.6
pointer move 785 690 sleep 200 down sleep 80 up sleep 600
expect_count close-dismissed "KWL NOTIFY dismiss id=$close\$" 1
expect_count close-told "KWL NOTIFY closed client=[0-9]* id=$close reason=1" 1
pointer move 100 100 sleep 200

# 4. An ACTION notification's body is activated.
notify --action --wait-ms=8000 Act "Click the body"
act=$id
wait_for "KWL NOTIFY show id=$act " 1 10
sleep 0.6
pointer move 560 725 sleep 200 down sleep 80 up sleep 800
expect_count action-activated "KWL NOTIFY activate id=$act\$" 1
n=$(guest "grep -c 'KEILAND-NOTIFY activated' /tmp/notify-client.log" | tail -1)
[ "${n:-0}" -ge 1 ] 2>/dev/null && pass action-client-told || fail "action-client-told (${n:-?})"
pointer move 100 100 sleep 200

# 5. The pointer on the board holds it.
notify --wait-ms=200 Hold
hold=$id
wait_for "KWL NOTIFY show id=$hold " 1 10
pointer move 600 710 sleep 5000
expect_count hover-holds "KWL NOTIFY hide id=$hold\$" 0
pointer move 100 100 sleep 200
wait_for "KWL NOTIFY gone id=$hold\$" 1 12
expect_count hover-then-leaves "KWL NOTIFY gone id=$hold\$" 1

# 6. Under a fullscreen window: skipped, but an urgent one shows.
guest "$env /bin/wltest --windowed --fullscreen-at=1 --frames=3600 --delay-ms=250 > /tmp/wltest-fs.log 2>&1 </dev/null & sleep 4; echo started" >/dev/null
notify --wait-ms=200 Quiet
quiet=$id
wait_for "KWL NOTIFY skip id=$quiet reason=fullscreen" 1 10
expect_count fullscreen-skipped "KWL NOTIFY skip id=$quiet reason=fullscreen" 1
notify --urgent --wait-ms=200 Urgent
urgent=$id
wait_for "KWL NOTIFY show id=$urgent client=[0-9]* urgent=1" 1 10
expect_count fullscreen-urgent-shown "KWL NOTIFY show id=$urgent client=[0-9]* urgent=1" 1
sleep 1
shot urgent-over-fullscreen
pointer move 600 710 sleep 400
pointer move 100 100 sleep 200
wait_for "KWL NOTIFY gone id=$urgent\$" 1 14
guest "for p in \$(ps -A -o pid,args | grep '[w]ltest' | awk '{print \$1}'); do kill \$p; done; sleep 1; echo stopped" >/dev/null

# 7. The log: open, step, the clearing board, clear, empty, close.
key meta_l true; tap n; key meta_l false; sleep 0.5
expect_count log-open 'KWL NOTIFY log open count=7' 1
shot log
tap right
expect_count log-older "KWL NOTIFY log at place=2 id=$quiet\$" 1
tap left
tap left
expect_count log-clearing-board 'KWL NOTIFY log at place=0 clear' 1
shot log-clear
tap ret
expect_count log-cleared 'KWL NOTIFY log clear count=7' 1
shot log-empty
tap esc
expect_count log-closed 'KWL NOTIFY log close reason=esc' 1

# 8. Still up, no error.
n=$(guest "ps -A -o args | grep -c '[w]ayland'" | tail -1)
[ "${n:-0}" -ge 1 ] 2>/dev/null && pass alive || fail "alive (${n:-?})"
expect_count no-error 'ERROR' 0
guest "cat /tmp/zdesktop.log" > "$out/zdesktop.log"
guest "cat /tmp/notify-client.log" > "$out/notify-client.log"
guest "$stop_all" >/dev/null
if [ $status -eq 0 ]; then echo "ws156-p005: PASS"; else echo "ws156-p005: FAIL"; fi
exit $status
