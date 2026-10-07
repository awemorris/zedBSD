#!/bin/sh
# ws181-p010 (the 2026-10-08 user decision): Alt+Shift with Left or Right turns the desktop, on the pen test guest
# (plan/ws079/tests/config-amd64-pen.mk; plan/ws079/tests/pen-guest.sh start IMAGE).  The compositor and wltest under
# test (BUILD/bin/wayland, BUILD/bin/wltest) are copied in as /bin/wayland-p010 and /bin/wltest (checked with cksum),
# 1280x800, one wltest window a on the middle desktop.  Keys through QMP.
#  1. Alt+Shift+Right: the right desktop ("KWL GLASS desktop=3 via=alt-shift"); p010-right.png.
#  2. Alt+Shift+Right again: no desktop beyond, it stays ("KWL GLASS desktop stays=3 via=alt-shift").
#  3. Alt+Shift+Left twice: the middle one, then the left one ("desktop=2 via=alt-shift", "desktop=1 via=alt-shift").
#  4. Alt+Shift+Up is not the compositor's: no new "via=alt-shift" line.
#  5. Ctrl+Alt+Right still turns the desktop ("desktop=2 via=key"), and Ctrl+Alt+Shift+Right is not Alt+Shift's: it
#     takes a along ("desktop=3 via=key", no new "via=alt-shift").
#  The compositor stays up, without KWL FAILED.
#
#   plan/ws181/tests/p010-guest.sh BUILD [OUTDIR]
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
cd "$(dirname -- "$0")/../../.."
export GUEST_RUNTIME="${GUEST_RUNTIME:-$PWD/build/ws079-run}"
build=${1:?usage: p010-guest.sh BUILD [OUTDIR]}
out=${2:-build/ws181-p010-guest}
mkdir -p "$out"
qmp="$GUEST_RUNTIME/qmp.sock"
guest() { timeout 120 python3 plan/tools/guest/guest.py run "$1" 2>&1; }
put() { timeout 90 python3 plan/tools/guest/guest.py put "$1" "$2" >> "$out/put.txt" 2>&1; }
# Copies a file in and proves the guest's copy is the same bytes (cksum), or says why not.
install_file() {
	want=$(cksum < "$1" | awk '{print $1, $2}')
	put "$1" "$2"
	got=$(guest "cksum < $2" | tail -1 | awk '{print $1, $2}')
	[ "$got" = "$want" ] && return 0
	echo "install_file $2: guest cksum '$got', host '$want'" >> "$out/put.txt"
	return 1
}
send() { timeout 40 python3 plan/ws049/tests/qmp-send.py "$qmp" "$@" >> "$out/qmp.txt" 2>&1; }
# A picture for the eye, read from the Venus head through QEMU's VNC.
shot() { timeout 60 python3 plan/ws035/tests/zdesktop-check.py "$out/$1.png" --runtime "$GUEST_RUNTIME" >> "$out/qmp.txt" 2>&1; }
pointer() { python3 plan/ws035/tests/qmp-pointer.py "$qmp" "$@" >/dev/null; }
key() { send input-send-event "{\"events\":[{\"type\":\"key\",\"data\":{\"down\":$2,\"key\":{\"type\":\"qcode\",\"data\":\"$1\"}}}]}"; }
tap() { key "$1" true; sleep 0.12; key "$1" false; sleep 0.6; }
# Taps an arrow with modifiers held: chord "alt shift" right.
chord() { mods=$1; arrow=$2; for m in $mods; do key "$m" true; done; tap "$arrow"; for m in $mods; do key "$m" false; done; sleep 0.8; }
count() { guest "grep -c -- '$1' /tmp/zdesktop.log" | tail -1; }
stop_all='for p in $(ps -A -o pid,args | grep -E "[w]ayland(-p010)?( |$)|[w]ltest" | awk "{print \$1}"); do kill $p; done; i=0; while ps -A -o args | grep -qE "[w]ayland(-p010)?( |$)|[w]ltest" && [ $i -lt 50 ]; do sleep 0.2; i=$((i+1)); done'
env='export XDG_RUNTIME_DIR=/tmp WAYLAND_DISPLAY=wayland-0;'
status=0
pass() { echo "$1: ok"; }
fail() { echo "$1: FAILED"; status=1; }
expect_count() { n=$(count "$2"); if [ "${n:-0}" -eq "$3" ] 2>/dev/null; then pass "$1"; else fail "$1 ($2: ${n:-?}, expected $3)"; fi; }
: > "$out/qmp.txt"

# The compositor and wltest under test, and one window.
: > "$out/put.txt"
guest "$stop_all" >/dev/null
if ! install_file "$build/bin/wayland" /bin/wayland-p010 || ! install_file "$build/bin/wltest" /bin/wltest; then
	echo "setup: the compositor or wltest under test is not in the guest ($out/put.txt)"
	echo "ws181-p010: FAIL"
	exit 1
fi
guest 'chmod 755 /bin/wayland-p010 /bin/wltest; export XDG_RUNTIME_DIR=/tmp; rm -f /tmp/wayland-0
/bin/wayland-p010 --testing --timeout=600 --width=1280 --height=800 --glass > /tmp/zdesktop.log 2>&1 </dev/null & for w in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20; do grep -q KWL.READY /tmp/zdesktop.log 2>/dev/null && break; sleep 0.5; done; sleep 2; echo started' >/dev/null
guest "$env /bin/wltest --app-id=apps.a --windowed --size=380x260 --color=f4d0d0 --frames=2400 --delay-ms=250 > /tmp/apps.a.log 2>&1 </dev/null & sleep 3; echo started" >/dev/null
pointer move 640 500 sleep 300 down sleep 60 up sleep 600

# 1. Alt+Shift+Right: the right desktop.
chord "alt shift" right
expect_count right 'KWL GLASS desktop=3 via=alt-shift' 1
shot p010-right

# 2. Again: nothing beyond the last desktop.
chord "alt shift" right
expect_count right-stays 'KWL GLASS desktop stays=3 via=alt-shift' 1

# 3. Left twice: the middle, then the left desktop.
chord "alt shift" left
expect_count middle 'KWL GLASS desktop=2 via=alt-shift' 1
chord "alt shift" left
expect_count left 'KWL GLASS desktop=1 via=alt-shift' 1
shot p010-left

# 4. Alt+Shift+Up is the application's.
chord "alt shift" up
expect_count up-not-taken 'via=alt-shift' 4

# 5. Ctrl+Alt still turns; Ctrl+Alt+Shift takes the window along, not Alt+Shift's.
chord "ctrl alt" right
expect_count ctrl-alt 'KWL GLASS desktop=2 via=key' 1
chord "ctrl alt shift" right
expect_count ctrl-alt-shift 'KWL GLASS desktop=3 via=key' 1
expect_count ctrl-alt-shift-not-alt-shift 'via=alt-shift' 4

# The compositor stays up.
failed=$(count 'KWL FAILED')
[ "${failed:-1}" -eq 0 ] 2>/dev/null && pass no-failed || fail "no-failed (${failed:-?})"
alive=$(guest 'ps -A -o args | grep -cE "^/bin/wayland-p010( |$)"' | tail -1)
[ "${alive:-0}" -gt 0 ] 2>/dev/null && pass alive || fail alive
guest 'cat /tmp/zdesktop.log' > "$out/zdesktop.log"
guest "$stop_all" >/dev/null

[ $status = 0 ] && echo "ws181-p010: PASS" || echo "ws181-p010: FAIL"
exit $status
