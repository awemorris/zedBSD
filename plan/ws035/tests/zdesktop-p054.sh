#!/bin/sh
# ws035-p054: acquire fences, checked on the Venus guest.
#
# zdesktop runs at 1280x800.  Client a (acquire-fence-test) is a red 400x300
# window (at 440,250); after 10 frames it gives its next commit one more
# acquire fence, a kernel fence it signals itself HOLD ms later, and
# presents a blue frame.  Client b (wltest) is a green 300x200 window
# (cascaded at 522,332) drawing all the time.
#  1. During the hold: a is still red, and b and the compositor keep making
#     frames (zdesktop does not wait for the fence).
#  2. After the signal: a is blue; zdesktop's log gives the time the commit waited.
#  3. A fence that is never signaled: a stays red, b keeps drawing.
#  4. The time of a window's presents (vkQueuePresentKHR), with the WSI
#     committing before the rendering is done.
#
#   plan/ws035/tests/zdesktop-guest.sh start     (the guest must be up)
#   plan/ws035/tests/zdesktop-p054.sh [OUTDIR]
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
cd "$(dirname -- "$0")/../../.."
export GUEST_RUNTIME="${GUEST_RUNTIME:-$PWD/build/ws035-sq-run}"
out=${1:-build/ws035-p054}
mkdir -p "$out"
hold=${HOLD:-6000}
guest() { timeout 90 python3 plan/tools/guest/guest.py run "$1" 2>&1; }
check() { python3 plan/ws035/tests/zdesktop-check.py "$@" --runtime "$GUEST_RUNTIME"; }
stop_all='for p in $(ps -A -o pid,args | grep -E "[z]desktop( |$)|[w]ltest|[a]cquire-fence" | awk "{print \$1}"); do kill $p; done; i=0; while ps -A -o args | grep -qE "[z]desktop( |$)|[w]ltest|[a]cquire-fence" && [ $i -lt 50 ]; do sleep 0.2; i=$((i+1)); done'
frames_b='grep -c "WLTEST FRAME run=b" /tmp/b.log'
frames_zwl='grep -c "ZWL COMPOSE" /tmp/zdesktop.log'

# Waits until a guest command prints something, up to about a minute.
wait_for() {
	i=0
	while [ $i -lt 120 ]; do
		result=$(guest "$1")
		[ -n "$result" ] && return 0
		sleep 0.5
		i=$((i + 1))
	done
	echo "p054: timed out waiting for: $1"
	return 1
}

# Starts the compositor, client a with the given hold, then client b.
start() {
	guest "$stop_all" >/dev/null
	guest 'export XDG_RUNTIME_DIR=/tmp; rm -f /tmp/wayland-0; /bin/zdesktop --timeout=120 --width=1280 --height=800 --log-frames > /tmp/zdesktop.log 2>&1 </dev/null & sleep 3
/bin/acquire-fence-test --size=400x300 --color=ff0000 --held-color=0000ff --frames=10 --hold-ms='"$1"' --linger-ms=15000 --token=a > /tmp/a.log 2>&1 </dev/null & sleep 1
/bin/wltest --windowed --size=300x200 --color=00ff00 --frames=3000 --token=b > /tmp/b.log 2>&1 </dev/null & echo started' >/dev/null
}

# Checks, during a hold, that a is red and that b and the compositor go on.
during() {
	wait_for 'grep "FENCETEST HELD" /tmp/a.log' || status=1
	sleep 1
	b0=$(guest "$frames_b"); z0=$(guest "$frames_zwl")
	check "$out/$1.png" --expect 450,260,ff0000 --expect 530,340,00ff00 \
	    --expect 10,10,203040 || status=1
	sleep 1
	b1=$(guest "$frames_b"); z1=$(guest "$frames_zwl")
	signaled=$(guest 'grep -c SIGNALED /tmp/a.log')
	echo "$1: b frames $b0 -> $b1, compositor frames $z0 -> $z1, signaled: $signaled" | tee "$out/$1.txt"
	[ "$signaled" = 0 ] || { echo "p054: the hold ended before the check"; status=1; }
	[ "$b1" -gt "$b0" ] && [ "$z1" -gt "$z0" ] || status=1
}

status=0

# 1 and 2: a fence signaled after the hold.
start "$hold"
during held
wait_for 'grep SIGNALED /tmp/a.log' || status=1
sleep 1
check "$out/released.png" --expect 450,260,0000ff --expect 530,340,00ff00 || status=1
guest 'grep ACQUIRED /tmp/zdesktop.log' | tee "$out/acquired.txt"
waited=$(sed -n 's/.*waited_ms=\([0-9]*\).*/\1/p' "$out/acquired.txt" | sort -n | tail -1)
[ -n "$waited" ] && [ "$waited" -ge $((hold / 2)) ] || status=1

# 3: a fence never signaled.
start 0
during never
sleep 4
check "$out/never.png" --expect 450,260,ff0000 --expect 530,340,00ff00 || status=1

guest 'grep -E "ERROR|FAILED" /tmp/zdesktop.log /tmp/a.log /tmp/b.log' | tee "$out/errors.txt"

# 4: presents of a window alone, 300 frames without delay.
guest "$stop_all" >/dev/null
guest 'export XDG_RUNTIME_DIR=/tmp; rm -f /tmp/wayland-0; /bin/zdesktop --timeout=120 --width=1280 --height=800 > /tmp/zdesktop.log 2>&1 </dev/null & sleep 3
start=$(date +%s); /bin/wltest --windowed --size=400x300 --color=ff0000 --frames=300 --delay-ms=0 --token=p > /tmp/p.log 2>&1 </dev/null; end=$(date +%s)
grep -E "PRESENT|FAILED" /tmp/p.log; echo "seconds=$((end - start))"' | tee "$out/presents.txt"
guest "$stop_all" >/dev/null
[ -s "$out/errors.txt" ] && status=1

[ $status -eq 0 ] && echo "p054: PASS" || echo "p054: FAIL"
exit $status
