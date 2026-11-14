#!/bin/sh
# The render thread's waits on the QEMU Venus guest: the thread waits for one thing at a time and never with a timeout,
# every completion reaches the event loop without a request after it, and drawing goes on as before.  Builds the image
# (plan/ws353/tests/config-amd64-render.mk) unless IMAGE is given, boots it, starts the compositor (--testing --glass
# --log-frames --rotate-policy=any, its log in a guest file) from the serial line, and judges by counting the
# compositor's and the clients' lines in guest files with commands on the serial line (the console's text is never
# read for the verdict).
#
# Steps:
#  - start: window mode.
#  - draw-shm: wlshm draws 600 frames with a moving band (one on each frame callback): all within 90 s.  The
#    compositor's frames meanwhile and their rate (from KWL COMPOSE's at_ms) are printed.
#  - draw-vk: wltest presents 300 frames FIFO (its own swapchain): all within 90 s; WLTEST PRESENT is printed.
#  - rotate: with wlshm drawing, keiland-shot's ROTATE 90 then ROTATE 0 turn the output (KWL OUTPUT rotate ...
#    rotation=90, then rotation=0; the turn parks the render thread) and frames are composed after each turn.
#  - bake: the turns make the layers' pictures stale; one is drawn in the quiet after them (KWL LAYER baked, within
#    40 s); then wlshm draws 60 frames and at least 60 frames are composed after that line (a picture's completion
#    reaches the event loop with nothing asked after it, or no frame could be drawn again).
#  - game: mview spins a model in the game mode for 5 s (KWL SCANOUT: the display thread's question answered) and
#    ends; then wlshm draws 60 frames composed in window mode again.
#  - shot: keiland-shot captures the screen (it waits for a frame's completion).
#  - stop: SIGTERM; KWL EXIT, and the render thread's line (KWL RENDER stopped) has one fence wait for each job
#    (fence_waits = jobs) and at most one for each task (task_waits <= tasks).
#  - errors: no KWL FAILED, VULKAN_ERROR or KWL RENDER stalled line.
#
#   [IMAGE=FILE] sh plan/ws353/tests/qemu-render-wait.sh [OUT]
#
# OUT (default build/ws353-p011-qemu) gets summary.txt (a line per step, the last "qemu-render-wait: PASS" or FAIL),
# the make logs and the guest's log lines.  The guest's runtime is OUT/run.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -eu
cd "$(dirname -- "$0")/../../.."
out=${1:-build/ws353-p011-qemu}
config=plan/ws353/tests/config-amd64-render.mk
# The host's Venus renderer (plan/ws353/tests/build-venus-renderer.sh makes it), on Lavapipe with zink for QEMU's GL.
VENUS_RENDERER=${VENUS_RENDERER:-$PWD/build/ws035-sq-venus/install}
export LIBGL_ALWAYS_SOFTWARE=1
export MESA_LOADER_DRIVER_OVERRIDE=zink
export VK_DRIVER_FILES="${VK_DRIVER_FILES:-/usr/share/vulkan/icd.d/lvp_icd.json}"
export RENDER_SERVER_EXEC_PATH="${RENDER_SERVER_EXEC_PATH:-$VENUS_RENDERER/libexec/virgl_render_server}"
export LD_LIBRARY_PATH="$VENUS_RENDERER/lib/x86_64-linux-gnu${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
if [ ! -x "$RENDER_SERVER_EXEC_PATH" ]; then
	echo "qemu-render-wait: FAIL no Venus renderer at $RENDER_SERVER_EXEC_PATH (plan/ws353/tests/build-venus-renderer.sh)" >&2
	exit 1
fi
. plan/tools/guest/venus-hostmem.sh
. plan/tools/fresh-out.sh
fresh_out "$out"
export GUEST_RUNTIME=$PWD/$out/run
summary=$out/summary.txt
: > "$summary"
guest="python3 plan/tools/guest/guest.py"
failed=0

say() {
	echo "$*" | tee -a "$summary"
}
fail_step() {
	say "$1: FAIL $2"
	failed=1
}

# The image, unless one is given; its build's warnings outside noct (the toolchain's) are a failure.
image=${IMAGE:-}
if [ -z "$image" ]; then
	# The compositor's package stamp does not follow the configuration: the capture's source made newer than the
	# stamp makes the package build again with it.
	touch userland/desktop/wayland/shot.c
	timeout 5400 make -j4 BUILD_JOBS=4 CONFIGMK=$config userland > "$out/make-userland.txt" 2>&1
	timeout 3000 make -j4 BUILD_JOBS=4 CONFIGMK=$config disk > "$out/make-disk.txt" 2>&1
	sync
	warnings=$(cat "$out/make-userland.txt" "$out/make-disk.txt" | grep 'warning:' | grep -vc 'userland/base/noct/' || true)
	say "build: warnings outside noct ${warnings}"
	[ "$warnings" -eq 0 ] || failed=1
	image=build/x86_64/hdd-image.img
fi

# The guest: Venus with one head on a private D-Bus display, the renderer the host has.
mkdir -p "$GUEST_RUNTIME"
bus=$(timeout 10 dbus-daemon --session --fork --print-address=1 --print-pid=1 --nopidfile)
printf '%s\n' "$bus" | sed -n 1p > "$GUEST_RUNTIME/dbus.address"
printf '%s\n' "$bus" | sed -n 2p > "$GUEST_RUNTIME/dbus.pid"
DBUS_SESSION_BUS_ADDRESS=$(cat "$GUEST_RUNTIME/dbus.address")
export DBUS_SESSION_BUS_ADDRESS
finish() {
	timeout 60 $guest stop >/dev/null 2>&1 || true
	kill "$(cat "$GUEST_RUNTIME/dbus.pid")" 2>/dev/null || true
	sync
}
trap finish EXIT
timeout 900 $guest start "$image" --cpus 4 --qemu-extra \
    "-object memory-backend-memfd,id=mem,size=8G,share=on -machine memory-backend=mem -device virtio-gpu-gl-pci,id=venus,venus=on,blob=on,hostmem=$VENUS_HOSTMEM,max_outputs=1 -display dbus,gl=on,rendernode=/dev/dri/renderD128" \
    > "$out/start.txt"

# One short command on the serial line (it logs in first when needed); a garbled status costs 20 s, not 120.
serial() {
	timeout 30 python3 plan/tools/guest/serial.py --socket "$GUEST_RUNTIME/serial.sock" --timeout 20 run "$1"
}
tries=0
until serial 'uname -a' > "$out/uname.txt" 2>&1; do
	tries=$((tries + 1))
	if [ "$tries" -ge 12 ]; then
		say "qemu-render-wait: FAIL no shell on the serial line (see $out/uname.txt)"
		exit 1
	fi
	sleep 10
done

# How many lines of a guest file match: the last all-digit line of grep -c, tried again when none came.
dir=/tmp/p11
log=$dir/kwl.log
count_in() {
	tries=0
	while [ "$tries" -lt 4 ]; do
		number=$(serial "grep -c '$1' $2" 2>/dev/null | tr -d '\r' | grep -E '^[0-9]+$' | sed -n '$p' || true)
		if [ -n "$number" ]; then
			echo "$number"
			return 0
		fi
		tries=$((tries + 1))
		sleep 1
	done
	echo -1
}
count() {
	count_in "$1" "$log"
}

# Waits until at least MIN lines of a guest file match, at most LIMIT seconds; the check is the command's status.
wait_in() {
	started=$(date +%s)
	while ! serial "test \$(grep -c '$1' $4) -ge $2" >/dev/null 2>&1; do
		if [ $(( $(date +%s) - started )) -ge "$3" ]; then
			return 1
		fi
		sleep 1
	done
}
wait_count() {
	wait_in "$1" "$2" "$3" "$log"
}

# The compositor's frames after line FROM of the log, as "frames=N first_ms=A last_ms=B" (the guest's awk).
frames_after() {
	tries=0
	while [ "$tries" -lt 4 ]; do
		line=$(serial "awk -v from=$1 'NR > from && /^KWL COMPOSE frame=/ { n++; split(\$NF, a, \"=\"); if (n == 1) f = a[2]; l = a[2] } END { printf \"frames=%d first_ms=%d last_ms=%d\\n\", n, f, l }' $log" 2>/dev/null |
		    tr -d '\r' | grep -E '^frames=[0-9]+ first_ms=[0-9]+ last_ms=[0-9]+$' | sed -n '$p' || true)
		if [ -n "$line" ]; then
			echo "$line"
			return 0
		fi
		tries=$((tries + 1))
		sleep 1
	done
	echo "frames=0 first_ms=0 last_ms=0"
}
# The frames' times after line FROM of the log (KWL LAT, --log-frames), as one line: how many frames went from
# queued to completed and how long that took (mean and worst, us), from acquired to queued (the walk and the recording,
# mean us), the frame pacing's wait before the walk (mean ms), and how many of the times from one frame's walk to the
# next were at most 50 ms, at most 100 ms, and longer.
timing_after() {
	tries=0
	while [ "$tries" -lt 4 ]; do
		line=$(serial "awk -v from=$1 'NR > from && \$2 == \"LAT\" { split(\$4, f, \"=\"); split(\$NF, t, \"=\"); k = \$3 } NR > from && k == \"acquired\" { a[f[2]] = t[2] } NR > from && k == \"queued\" { q[f[2]] = t[2]; if (f[2] in a) { w += t[2] - a[f[2]]; wn++ } } NR > from && k == \"shown\" && (f[2] in q) { d = t[2] - q[f[2]]; n++; s += d; if (d > m) m = d } NR > from && k == \"pace\" { split(\$6, p, \"=\"); pw += p[2]; pn++ } NR > from && k == \"draw\" { if (dl) { g = t[2] - dl; if (g <= 50000) g50++; else if (g <= 100000) g100++; else gmore++ } dl = t[2] } { k = \"\" } END { printf \"done=%d done_mean_us=%d done_max_us=%d walk_mean_us=%d pace_mean_ms=%d period_le50ms=%d period_le100ms=%d period_more=%d\\n\", n, n ? s / n : 0, m, wn ? w / wn : 0, pn ? pw / pn : 0, g50, g100, gmore }' $log" 2>/dev/null |
		    tr -d '\r' | grep -E '^done=[0-9]+ ' | sed -n '$p' || true)
		if [ -n "$line" ]; then
			echo "$line"
			return 0
		fi
		tries=$((tries + 1))
		sleep 1
	done
	echo "done=0"
}
frames_of() {
	echo "$1" | sed 's/^frames=\([0-9]*\) .*/\1/'
}

# A client in the background on the compositor's socket (/tmp/wayland-0), its output in a guest file.
client() {
	serial "WAYLAND_DISPLAY=/tmp/wayland-0 XDG_RUNTIME_DIR=$dir $1 > $dir/$2.txt 2>&1 < /dev/null &" > /dev/null 2>&1 || true
}

# The compositor, in the background, its log in a guest file.
serial "rm -rf $dir; mkdir -p $dir/home; XDG_RUNTIME_DIR=$dir HOME=$dir/home /bin/wayland --testing --glass --log-frames --rotate-policy=any --timeout=1800 --wallpaper=/usr/share/keiland/wallpaper.png > $log 2>&1 < /dev/null &" \
    > "$out/compositor-start.txt" 2>&1 || true
if wait_count 'KWL MODE window' 1 120 && wait_count 'KWL SHOT listening' 1 30; then
	say "start: PASS window mode, the capture listening"
else
	fail_step start "no window mode or capture"
fi
sleep 5

# draw-shm: a CPU client's frames, one on each frame callback.
from=$(count '')
client "wlshm --size=400x300 --band=ff00ff00 --frames=600 --token=shm" shm-a
if wait_in 'WLSHM DONE run=shm frames=600' 1 90 $dir/shm-a.txt; then
	numbers=$(frames_after "$from")
	say "draw-shm: PASS 600 client frames; compositor $numbers"
else
	fail_step draw-shm "wlshm did not draw 600 frames within 90 s ($(frames_after "$from"))"
fi
say "draw-shm-times: $(timing_after "$from")"

# draw-vk: a Vulkan client's FIFO frames.
from=$(count '')
client "wltest --display=/tmp/wayland-0 --mode=fifo --frames=300 --token=vk" vk
if wait_in 'WLTEST DONE run=vk frames=300' 1 90 $dir/vk.txt; then
	present=$(serial "grep 'WLTEST PRESENT' $dir/vk.txt" 2>/dev/null | tr -d '\r' | grep 'WLTEST PRESENT' | sed -n '$p' || true)
	say "draw-vk: PASS ${present}; compositor $(frames_after "$from")"
else
	serial "cat $dir/vk.txt" > "$out/vk.txt" 2>&1 || true
	fail_step draw-vk "wltest did not present 300 frames within 90 s (see $out/vk.txt)"
fi
say "draw-vk-times: $(timing_after "$from")"

# rotate: two turns with a client drawing; the turn parks the render thread.
baked=$(count '^KWL LAYER baked')
client "wlshm --size=300x200 --band=ff0000ff --frames=3000 --token=turning" shm-c
sleep 2
for rotation in 90 0; do
	turned=$(count "KWL OUTPUT rotate key=.* rotation=$rotation ")
	serial "keiland-shot --socket $dir/keiland-shot.sock --request 'ROTATE $rotation' > $dir/rotate-$rotation.txt 2>&1" > /dev/null 2>&1 || true
	if wait_count "KWL OUTPUT rotate key=.* rotation=$rotation " $((turned + 1)) 20; then
		line=$(count '')
		sleep 3
		numbers=$(frames_after "$line")
		if [ "$(frames_of "$numbers")" -ge 10 ]; then
			say "rotate-$rotation: PASS turned, then $numbers"
		else
			fail_step "rotate-$rotation" "turned, then only $numbers"
		fi
	else
		serial "cat $dir/rotate-$rotation.txt" > "$out/rotate-$rotation.txt" 2>&1 || true
		fail_step "rotate-$rotation" "no KWL OUTPUT rotate line within 20 s (see $out/rotate-$rotation.txt)"
	fi
done
serial 'for p in $(ps -A -o pid,args | grep "[w]lshm" | while read a b; do echo $a; done); do kill $p; done' > /dev/null 2>&1 || true
sleep 2

# bake: the turns made the layers' pictures stale; one is drawn in the quiet after them (KWL LAYER baked), then a
# client's frames are composed after that line (a picture's completion reaches the event loop with nothing asked after
# it, or no frame could be drawn again).
if wait_count '^KWL LAYER baked' $((baked + 1)) 40; then
	sleep 2
	line=$(count '')
	client "wlshm --size=200x150 --band=ffff0000 --frames=60 --token=after" shm-b
	if wait_in 'WLSHM DONE run=after frames=60' 1 30 $dir/shm-b.txt; then
		numbers=$(frames_after "$line")
		if [ "$(frames_of "$numbers")" -ge 60 ]; then
			say "bake: PASS a layer's picture drawn in the quiet after the turns, then $numbers"
		else
			fail_step bake "after the picture only $numbers"
		fi
	else
		fail_step bake "wlshm did not draw 60 frames after the picture ($(frames_after "$line"))"
	fi
else
	fail_step bake "no layer's picture drawn within 40 s of the quiet after the turns"
fi

# game: the game mode's way in and out, then window mode's frames again.
asked=$(count '^KWL SCANOUT')
client "timeout 60 mview --token=spin --spin=5 --copies=1 --content=game --present=mailbox" mview
if wait_in 'MVIEW DONE run=spin' 1 60 $dir/mview.txt && [ "$(count '^KWL SCANOUT')" -gt "$asked" ]; then
	sleep 2
	line=$(count '')
	client "wlshm --size=200x150 --band=ff00ffff --frames=60 --token=back" shm-d
	if wait_in 'WLSHM DONE run=back frames=60' 1 30 $dir/shm-d.txt; then
		say "game: PASS the game mode asked and left; then $(frames_after "$line")"
	else
		fail_step game "wlshm did not draw 60 frames after the game mode ($(frames_after "$line"))"
	fi
else
	serial "cat $dir/mview.txt" > "$out/mview.txt" 2>&1 || true
	fail_step game "mview did not end or no KWL SCANOUT line (see $out/mview.txt)"
fi

# shot: a capture, which waits for a frame's completion.
sent=$(count 'KWL SHOT sent')
serial "keiland-shot --socket $dir/keiland-shot.sock $dir/shot.png > $dir/shot.txt 2>&1 < /dev/null &" > /dev/null 2>&1 || true
if wait_count 'KWL SHOT sent' $((sent + 1)) 30; then
	say "shot: PASS written"
else
	fail_step shot "no capture within 30 s"
fi

# stop: SIGTERM, the render thread's counts.
sleep 3
serial 'for p in $(ps -A -o pid,args | grep "[/]bin/wayland" | while read a b; do echo $a; done); do kill $p; done' \
    > "$out/kill.txt" 2>&1 || true
if wait_count '^KWL EXIT' 1 60 && wait_count '^KWL RENDER stopped' 1 10; then
	counts=$(serial "grep '^KWL RENDER stopped' $log" 2>/dev/null | tr -d '\r' | grep '^KWL RENDER stopped' | sed -n '$p' || true)
	jobs=$(echo "$counts" | sed -n 's/.* jobs=\([0-9]*\).*/\1/p')
	tasks=$(echo "$counts" | sed -n 's/.* tasks=\([0-9]*\).*/\1/p')
	fence_waits=$(echo "$counts" | sed -n 's/.* fence_waits=\([0-9]*\).*/\1/p')
	task_waits=$(echo "$counts" | sed -n 's/.* task_waits=\([0-9]*\).*/\1/p')
	if [ -n "$jobs" ] && [ "$jobs" -gt 0 ] && [ "$fence_waits" -eq "$jobs" ] && [ "$task_waits" -le "$tasks" ]; then
		say "stop: PASS ${counts#KWL RENDER stopped }"
	else
		fail_step stop "counts '${counts}'"
	fi
else
	fail_step stop "no KWL EXIT and KWL RENDER stopped within 70 s"
fi
bad=$(count '^KWL \(FAILED\|VULKAN_ERROR\|RENDER stalled\)')
if [ "$bad" -eq 0 ]; then
	say "errors: PASS no KWL FAILED, VULKAN_ERROR or RENDER stalled"
else
	fail_step errors "${bad} lines"
fi

# The guest's log for the record (read over the serial line: kernel records may be mixed in; not used for the verdict).
serial "grep -e 'KWL PERF pass' -e 'KWL CURSOR_PLANE' -e 'KWL MODE' -e 'KWL LAYER baked' -e 'KWL OUTPUT rotate' -e 'KWL SCANOUT' -e 'KWL SHOT' -e 'KWL RENDER' -e 'KWL EXIT' -e 'KWL FAILED' -e 'VULKAN_ERROR' $log" > "$out/kwl.log" 2>&1 || true

if [ "$failed" -eq 0 ]; then
	say "qemu-render-wait: PASS"
else
	say "qemu-render-wait: FAIL"
	exit 1
fi
