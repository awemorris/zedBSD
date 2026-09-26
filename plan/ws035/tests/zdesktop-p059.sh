#!/bin/sh
# ws035-p059: the glass look (floating title bars, frosted glass, system bar),
# checked on the Venus guest.
#
# zwl --glass runs at 1280x800 with the font at /usr/share/fonts/zdesktop.ttf
# (build/ws035-fonts/Inter.ttf, not in git: see phase059).  Window a is a
# pale 420x300 wltest window, window b a pale 360x240 wl_shm window over it.
# The pointer is driven with QMP (usb-tablet, absolute).
#  1. desktop.png: both windows with their title bars.
#  2. hover.png: the pointer over b's close button (red).
#  3. b's title bar is dragged 300 left and 100 up: zwl logs the move and b's
#     body is at its new place.
#  4. a's maximize button: a is configured to the space under the system bar
#     and fills it; pressed again, a goes back to 420x300 at its place.
#  5. b's close button: the client gets xdg_toplevel.close and exits.
#
#   plan/ws035/tests/zdesktop-guest.sh start     (the guest must be up)
#   plan/ws035/tests/zdesktop-p059.sh [OUTDIR]
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
cd "$(dirname -- "$0")/../../.."
export GUEST_RUNTIME="${GUEST_RUNTIME:-$PWD/build/ws035-sq-run}"
out=${1:-build/ws035-p059}
mkdir -p "$out"
guest() { timeout 90 python3 plan/tools/guest/guest.py run "$1" 2>&1; }
check() { python3 plan/ws035/tests/zdesktop-check.py "$@" --runtime "$GUEST_RUNTIME"; }
qmp() { python3 plan/tools/qmp.py "$GUEST_RUNTIME/qmp.sock" input-send-event "$1" >/dev/null; }
stop_all='for p in $(ps -A -o pid,args | grep -E "[z]wl|[w]lshm|[w]ltest|[m]view" | awk "{print \$1}"); do kill $p; done; i=0; while ps -A -o args | grep -qE "[z]wl|[w]lshm|[w]ltest|[m]view" && [ $i -lt 50 ]; do sleep 0.2; i=$((i+1)); done'

# Moves the pointer to an output pixel (zwl takes floor(v * (size - 1) / 32767)).
move() {
	qmp "{\"events\":[{\"type\":\"abs\",\"data\":{\"axis\":\"x\",\"value\":$((($1 * 32767 + 1278) / 1279))}},{\"type\":\"abs\",\"data\":{\"axis\":\"y\",\"value\":$((($2 * 32767 + 798) / 799))}}]}"
	sleep "${3:-0.3}"
}

# Presses or releases the left button.
button() {
	qmp "{\"events\":[{\"type\":\"btn\",\"data\":{\"down\":$1,\"button\":\"left\"}}]}"
	sleep 0.5
}

# Clicks at a pixel.
click() {
	move "$1" "$2" 0.5
	button true
	button false
}

# The place zwl gave a surface of a client (x and y from its MAP line).
place() {
	guest "grep 'ZWL MAP client=$1 ' /tmp/zwl.log | tail -1" | sed -n 's/.* x=\([-0-9]*\) y=\([-0-9]*\).*/\1 \2/p'
}

status=0
guest "$stop_all" >/dev/null
guest 'export XDG_RUNTIME_DIR=/tmp; rm -f /tmp/wayland-0; /bin/zwl --timeout=300 --width=1280 --height=800 --glass --log-frames > /tmp/zwl.log 2>&1 </dev/null & sleep 3
/bin/wltest --windowed --size=420x300 --color=f4f7fc --frames=3600 --delay-ms=50 --token=a > /tmp/a.log 2>&1 </dev/null & sleep 2
/bin/wlshm --size=360x240 --color=ffe8eef8 --frames=6000 --token=b > /tmp/b.log 2>&1 </dev/null & sleep 3; echo started' >/dev/null
guest 'grep -E "GLASS|MAP" /tmp/zwl.log' | tee "$out/log-start.txt"
set -- $(place 1); ax=$1; ay=$2
set -- $(place 2); bx=$1; by=$2

# 1. Both windows with their title bars (a's body left of b, b's body).
move 1200 700
check "$out/desktop.png" --expect $((ax + 20)),$((ay + 150)),f4f7fc \
    --expect $((bx + 180)),$((by + 120)),e8eef8 || status=1

# 2. The pointer over b's close button.
move $((bx + 360 - 26)) $((by - 8 - 22)) 1
check "$out/hover.png" >/dev/null || status=1

# 3. b's title bar dragged 300 left and 100 up.
move $((bx + 150)) $((by - 30)) 0.5
button true
i=1
while [ $i -le 10 ]; do
	move $((bx + 150 - 30 * i)) $((by - 30 - 10 * i)) 0.2
	i=$((i + 1))
done
button false
nx=$((bx - 300)); ny=$((by - 100))
guest 'grep "GLASS moved" /tmp/zwl.log' | tee "$out/moved.txt"
grep -q "x=$nx y=$ny" "$out/moved.txt" || status=1
move 1200 700
check "$out/moved.png" --expect $((nx + 180)),$((ny + 120)),e8eef8 \
    --expect $((ax + 400)),$((ay + 280)),f4f7fc || status=1

# 4. a maximized (its button, two from the right), then back.
click $((ax + 420 - 26 - 34)) $((ay - 8 - 22))
sleep 3
move 1200 700
check "$out/maximized.png" --expect 640,500,f4f7fc --expect 20,120,f4f7fc \
    --expect 1260,780,f4f7fc || status=1
click $((12 + 1256 - 26 - 34)) $((98 - 8 - 22))
sleep 3
move 1200 700
check "$out/restored.png" --expect $((ax + 20)),$((ay + 150)),f4f7fc || status=1
guest 'grep -E "CONFIGURE client=1" /tmp/zwl.log; grep RESIZE /tmp/a.log' | tee "$out/maximize.txt"
grep -q "width=1256 height=690" "$out/maximize.txt" || status=1
grep -q "RESIZE run=a width=420 height=300" "$out/maximize.txt" || status=1

# 5. b closed from its close button.
click $((nx + 360 - 26)) $((ny - 8 - 22))
sleep 2
move 1200 700
check "$out/closed.png" >/dev/null || status=1
guest 'grep "GLASS close" /tmp/zwl.log; tail -2 /tmp/b.log; ps -A -o args | grep -c "[w]lshm"' | tee "$out/closed.txt"
[ "$(tail -1 "$out/closed.txt")" = 0 ] || status=1
guest 'grep -E "ERROR|FAILED" /tmp/zwl.log /tmp/a.log /tmp/b.log' | tee "$out/errors.txt"
[ -s "$out/errors.txt" ] && status=1

guest "$stop_all" >/dev/null
[ $status -eq 0 ] && echo "p059: PASS" || echo "p059: FAIL"
exit $status
