#!/bin/sh
# ws035-p053: wl_shm windows and the cursor, checked on the Venus guest.
#
# zwl runs at 1280x800; the pointer is moved with QMP (usb-tablet, absolute).
#  0. A red wl_shm window with a moving white band: only the band's rows are
#     copied (the compositor's log).
#  1. A red wl_shm window 400x300, a half-transparent green wl_shm window 300x200
#     (premultiplied 0x80008000, so red*0.5 + green*0.5 over red), a blue GPU
#     window 200x150 on top, and zdesktop's arrow at the pointer (100,100).
#  2. A wl_shm window that sets its own 8x8 yellow cursor (hotspot 4,4).
#  3. A wl_shm window that hides the cursor.
#  4. Fullscreen mode (wltest): no cursor at the pointer.
#
#   plan/ws035/tests/zdesktop-guest.sh start     (the guest must be up)
#   plan/ws035/tests/zdesktop-p053.sh [OUTDIR]
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
cd "$(dirname -- "$0")/../../.."
export GUEST_RUNTIME="${GUEST_RUNTIME:-$PWD/build/ws035-sq-run}"
out=${1:-build/ws035-p053}
mkdir -p "$out"
guest() { timeout 90 python3 plan/tools/guest/guest.py run "$1" 2>&1; }
check() { python3 plan/ws035/tests/zdesktop-check.py "$@" --runtime "$GUEST_RUNTIME"; }
stop_all='for p in $(ps -A -o pid,args | grep -E "[z]wl|[w]lshm|[w]ltest" | awk "{print \$1}"); do kill $p; done; i=0; while ps -A -o args | grep -qE "[z]wl|[w]lshm|[w]ltest" && [ $i -lt 50 ]; do sleep 0.2; i=$((i+1)); done'

# Moves the pointer to an output pixel.  The tablet's range is 0..32767 and
# zwl takes the pixel floor(v * (size - 1) / 32767), so v is rounded up.
point() {
	x=$((($1 * 32767 + 1278) / 1279))
	y=$((($2 * 32767 + 798) / 799))
	python3 plan/tools/qmp.py "$GUEST_RUNTIME/qmp.sock" input-send-event \
	    "{\"events\":[{\"type\":\"abs\",\"data\":{\"axis\":\"x\",\"value\":$x}},{\"type\":\"abs\",\"data\":{\"axis\":\"y\",\"value\":$y}}]}" >/dev/null
	sleep 2
}

# Starts the compositor, then the given clients one after another.
start() {
	guest "$stop_all" >/dev/null
	guest "export XDG_RUNTIME_DIR=/tmp; rm -f /tmp/wayland-0; /bin/zwl --timeout=120 --width=1280 --height=800 --log-frames > /tmp/zwl.log 2>&1 </dev/null & sleep 3; $1 sleep 4; echo started" >/dev/null
}

status=0

# 0. Only the rows of the moving band are copied after the first frames.
start '/bin/wlshm --size=400x300 --color=ffff0000 --band=ffffffff --frames=3000 --token=b > /tmp/b.log 2>&1 </dev/null &'
guest 'echo "copies: $(grep -c SHM_COPY /tmp/zwl.log)"; grep SHM_COPY /tmp/zwl.log | sed -n "1,2p;10,12p"' | tee "$out/shm-copies.txt"
guest 'grep SHM_COPY /tmp/zwl.log | sed -n 10p' | grep -q 'rows=[0-9]*-[0-9]*' || status=1

# 1. wl_shm windows, a GPU window, the arrow.
start '/bin/wlshm --size=400x300 --color=ffff0000 --frames=3000 --token=s > /tmp/s.log 2>&1 </dev/null & sleep 2; /bin/wlshm --size=300x200 --color=80008000 --frames=3000 --token=t > /tmp/t.log 2>&1 </dev/null & sleep 2; /bin/wltest --windowed --size=200x150 --color=0000ff --frames=3000 --token=g > /tmp/g.log 2>&1 </dev/null &'
point 100 100
check "$out/shm.png" --expect 10,10,203040 --expect 450,260,ff0000 \
    --expect 530,340,7f8000 --expect 810,340,7f8000 --expect 700,450,0000ff \
    --expect 100,100,000000 --expect 101,102,ffffff --expect 99,110,203040 || status=1

# 2. A client's own cursor.
start '/bin/wlshm --size=400x300 --color=ffff0000 --frames=3000 --cursor=ffffff00 --token=c > /tmp/c.log 2>&1 </dev/null &'
point 640 400
check "$out/cursor.png" --expect 636,396,ffff00 --expect 643,403,ffff00 \
    --expect 646,406,ff0000 --expect 634,394,ff0000 || status=1

# 3. A hidden cursor.
start '/bin/wlshm --size=400x300 --color=ffff0000 --frames=3000 --hide-cursor --token=h > /tmp/h.log 2>&1 </dev/null &'
point 640 400
check "$out/hidden.png" --expect 640,400,ff0000 --expect 641,402,ff0000 || status=1

# 4. Fullscreen mode draws no cursor (the pointer is over the blue quadrant).
start '/bin/wltest --frames=3000 --token=f > /tmp/f.log 2>&1 </dev/null &'
point 100 700
check "$out/fullscreen.png" --expect 100,700,0000ff --expect 101,702,0000ff \
    --expect 640,200,00ff00 || status=1
guest 'grep -E "MODE" /tmp/zwl.log | tail -1'

guest "$stop_all" >/dev/null
[ $status -eq 0 ] && echo "p053: PASS" || echo "p053: FAIL"
exit $status
