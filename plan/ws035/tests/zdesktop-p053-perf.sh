#!/bin/sh
# ws035-p053: does a wl_shm window slow a GPU window?  A GPU window (wltest,
# 200x150) is measured alone, beside two more GPU windows, beside two static
# wl_shm windows, and beside two wl_shm windows whose band moves (a copy every
# frame).  Each setup runs RUNS times; each run counts the GPU client's frames
# in 30 seconds and takes the compositor's last ZWL PERF lines.
#   plan/ws035/tests/zdesktop-p053-perf.sh [RUNS]    (the guest must be up)
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
cd "$(dirname -- "$0")/../../.."
export GUEST_RUNTIME="${GUEST_RUNTIME:-$PWD/build/ws035-sq-run}"
runs=${1:-3}
guest() { timeout 150 python3 plan/tools/guest/guest.py run "$1" 2>&1; }
stop_all='for p in $(ps -A -o pid,args | grep -E "[z]desktop( |$)|[w]lshm|[w]ltest" | awk "{print \$1}"); do kill $p; done; i=0; while ps -A -o args | grep -qE "[z]desktop( |$)|[w]lshm|[w]ltest" && [ $i -lt 50 ]; do sleep 0.2; i=$((i+1)); done'
for setup in gpu gpu3 shm shmband; do
	case $setup in
	gpu) others='' ;;
	gpu3) others='/bin/wltest --windowed --size=400x300 --color=ff0000 --frames=3600 --token=r > /tmp/r.log 2>&1 </dev/null & /bin/wltest --windowed --size=300x200 --color=00ff00 --frames=3600 --token=q > /tmp/q.log 2>&1 </dev/null &' ;;
	shm) others='/bin/wlshm --size=400x300 --color=ffff0000 --frames=100000 --token=s > /tmp/s.log 2>&1 </dev/null & /bin/wlshm --size=300x200 --color=80008000 --frames=100000 --token=t > /tmp/t.log 2>&1 </dev/null &' ;;
	shmband) others='/bin/wlshm --size=400x300 --color=ffff0000 --band=ffffffff --frames=100000 --token=s > /tmp/s.log 2>&1 </dev/null & /bin/wlshm --size=300x200 --color=80008000 --band=ffffffff --frames=100000 --token=t > /tmp/t.log 2>&1 </dev/null &' ;;
	esac
	run=1
	while [ $run -le "$runs" ]; do
		guest "$stop_all" >/dev/null
		guest "export XDG_RUNTIME_DIR=/tmp; rm -f /tmp/wayland-0; /bin/zdesktop --timeout=60 --width=1280 --height=800 > /tmp/perf.log 2>&1 </dev/null & sleep 3; $others sleep 2; /bin/wltest --windowed --size=200x150 --color=0000ff --frames=3600 --token=g > /tmp/g.log 2>&1 </dev/null & sleep 5; a=\$(grep -c 'WLTEST FRAME' /tmp/g.log); sleep 30; b=\$(grep -c 'WLTEST FRAME' /tmp/g.log); c=\$(grep 'PERF compose' /tmp/perf.log | tail -1 | sed 's/.*frame_ms=//'); d=\$(grep 'PERF shm' /tmp/perf.log | tail -1 | sed 's/.*copy_ms=//'); echo \"$setup run=$run gpu_frames_30s=\$((b-a)) compose_frame_ms=\$c shm_copy_ms=\${d:-none}\""
		run=$((run + 1))
	done
done
guest "$stop_all" >/dev/null
