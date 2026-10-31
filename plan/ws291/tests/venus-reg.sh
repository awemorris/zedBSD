#!/bin/sh
# ws291: libvulkan's regression on QEMU's Venus guest, from the host at the top of the tree: the forgery image
# (plan/tools/gpu-boundary/build-forge-image.sh) started in a new runtime directory OUT/runtime (nothing of an
# earlier guest is touched) and given 30 s past its SSH answer (tests run straight after the boot lose their SSH
# banner), then for each LIBVULKAN in turn: it is put in as /lib/libvulkan.so (the guest's emulator ends on a
# reboot, so the compositor and the clients are only started again, each check starting its own), and the checks
# run: forge-guest.sh, fence-guest.sh (600 frames), and in a compositor of their own wltest in mailbox mode with
# a swapchain made again at frame 100, mview in mailbox mode with 4 images and mview with one draw a copy and 20
# submissions a frame (each must end with status 0; the compositor's VULKAN_ERROR lines are counted).  The guest
# is ended by QEMU's monitor (quit).  Prints one line a check and "venus-reg: PASS" or "venus-reg: FAIL".
#   usage: [START_TIMEOUT=300] plan/ws291/tests/venus-reg.sh OUT IMAGE LIBVULKAN...  (START_TIMEOUT: seconds for the start,
#   which copies the image: a busy host disk needs more)
# The renderer is build/ws035-sq-venus (a link to main's), as plan/ws035/tests/zdesktop-guest.sh needs.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
out=${1:?usage: venus-reg.sh OUT IMAGE LIBVULKAN...}
image=${2:?usage: venus-reg.sh OUT IMAGE LIBVULKAN...}
shift 2
top=$(pwd)
mkdir -p "$out"
GUEST_RUNTIME=$top/$out/runtime
export GUEST_RUNTIME
g() { timeout "${G_TIMEOUT:-300}" python3 -I plan/tools/guest/guest.py "$@"; }
failed=0
check() {
	if [ "$2" = ok ]; then
		echo "venus-reg: $1 ok"
	else
		echo "venus-reg: $1 FAIL ($2)"
		failed=1
	fi
}

timeout "${START_TIMEOUT:-300}" sh plan/ws035/tests/zdesktop-guest.sh start "$image" > "$out/start.log" 2>&1
g wait > "$out/wait.log" 2>&1 || { echo "venus-reg: the guest did not answer"; failed=1; }
sleep 30
stop_all='service stop greeter >/dev/null 2>&1; for p in $(ps -A -o pid,args | grep -E "[w]ayland( |$)|[w]ltest|[m]view" | awk "{print \$1}"); do kill $p; done; i=0; while ps -A -o args | grep -qE "[w]ayland( |$)|[w]ltest|[m]view" && [ $i -lt 50 ]; do sleep 0.2; i=$((i+1)); done'
for lv in "$@"; do
	[ $failed = 0 ] || break
	name=$(basename "$lv" .so)
	echo "== $name $(sha256sum "$lv" | cut -c1-16) $(date +%H:%M:%S)"
	g put "$lv" /lib/libvulkan.so > "$out/$name-put.log" 2>&1 || check "$name put" "put failed"
	g run "$stop_all" > /dev/null 2>&1
	sleep 5
	timeout 600 sh plan/tools/gpu-boundary/forge-guest.sh "$out/$name-forge" > "$out/$name-forge.out" 2>&1
	if grep -q 'forge-guest: PASS' "$out/$name-forge.out"; then check "$name forge-guest" ok; else check "$name forge-guest" "$(tail -1 "$out/$name-forge.out")"; fi
	timeout 900 sh plan/tools/gpu-boundary/fence-guest.sh "$out/$name-fence" 600 > "$out/$name-fence.out" 2>&1
	if grep -q 'fence-guest: PASS' "$out/$name-fence.out"; then check "$name fence-guest" ok; else check "$name fence-guest" "$(tail -1 "$out/$name-fence.out")"; fi
	# The clients in a compositor of their own.
	g run "$stop_all" > /dev/null 2>&1
	g run 'export XDG_RUNTIME_DIR=/tmp; rm -f /tmp/wayland-0; /bin/wayland --testing --timeout=600 --width=1280 --height=800 > /tmp/reg.log 2>&1 </dev/null & for w in $(seq 1 30); do grep -q KWL.READY /tmp/reg.log 2>/dev/null && break; sleep 0.5; done; sleep 1' > /dev/null 2>&1
	for client in \
	    "wltest|/bin/wltest --windowed --size=400x300 --mode=mailbox --recreate-at=100 --frames=300 --delay-ms=0" \
	    "mview-mailbox|/bin/mview --spin=3 --present=mailbox --images=4 --copies=10" \
	    "mview-submits|/bin/mview --spin=3 --draws=each --submits=20 --copies=20"; do
		label=${client%%|*}
		run=${client#*|}
		G_TIMEOUT=200 g run "export XDG_RUNTIME_DIR=/tmp; timeout 120 $run > /tmp/$label.log 2>&1 </dev/null; echo status=\$?" > "$out/$name-$label.out" 2>&1
		g run "tail -3 /tmp/$label.log" >> "$out/$name-$label.out" 2>&1
		if grep -q '^status=0' "$out/$name-$label.out"; then check "$name $label" ok; else check "$name $label" "$(grep status= "$out/$name-$label.out")"; fi
	done
	errors=$(g run 'grep -ac VULKAN_ERROR /tmp/reg.log' 2>/dev/null | tail -1)
	if [ "${errors:-x}" = 0 ]; then check "$name compositor" ok; else check "$name compositor" "VULKAN_ERROR ${errors:-?}"; fi
	g run "$stop_all" > /dev/null 2>&1
done
# The guest ends by its monitor.
python3 -I - "$GUEST_RUNTIME/qmp.sock" <<'EOF' > "$out/quit.log" 2>&1
import socket, sys
s = socket.socket(socket.AF_UNIX)
s.settimeout(10)
s.connect(sys.argv[1])
s.recv(4096)
s.sendall(b'{"execute":"qmp_capabilities"}\n')
s.recv(4096)
s.sendall(b'{"execute":"quit"}\n')
print(s.recv(4096))
EOF
if [ $failed = 0 ]; then echo "venus-reg: PASS"; else echo "venus-reg: FAIL"; fi
