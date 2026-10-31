#!/bin/sh
# ws291-p025: mview's scene on the Raspberry Pi 4's V3D 4.2 with one tree's kernel and programs, run from the top of
# the tree on a board running zedBSD with root's SSH key of the guest harness (plan/tmp/guest/id_ed25519):
#   [PI=ADDRESS] [KERNEL=VMUNIX] [PROGRAMS=BUILD] [WINDOWED=1] [STILL=1] [COPIES='1 21'] [SPIN=12] [MVIEW_ARGS=...] [MVIEW_ENV='NAME=VALUE ...']
#       [SHOT=1] sh plan/ws291/tests/pi4-mview.sh OUT
# KERNEL, when its checksum differs from the board's /boot/boot0/vmunix, is copied there and the board rebooted
# (the board's first kernel is kept once as vmunix.p025old).  PROGRAMS is a build of the board's config: its
# bin/wayland, bin/mview (and bin/keiland-shot with SHOT=1, a build with ZEDBSD_TEST_SCREEN_CAPTURE=y) and dynamic
# libraries go to /root/p025 and run with LD_LIBRARY_PATH; the board's own files are not changed.  The GPU is started
# when it is still held.  The compositor runs on its own socket with no desktop client; with WINDOWED=1, mview first
# spins once in a window of 1280x720; with STILL=1, it shows the
# first view (the model from the front, not turning) in that window for SPIN seconds; then, once for each of COPIES, in the game mode (full screen, direct scan-out
# of the compositor's lent images, mailbox, 4 images) with libvulkan's chain log (VULKAN_ZEDBSD_WSI_LOG=1).  The kernel's
# lines of each run, the compositor's SCANOUT and MODE lines and OUT/summary.txt are collected.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
out=${1:?usage: pi4-mview.sh OUT}
PI=${PI:-10.0.30.6}
KEY=plan/tmp/guest/id_ed25519
SO="-i $KEY -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR -o ConnectTimeout=8 -o BatchMode=yes"
mkdir -p "$out"
summary=$out/summary.txt
: > "$summary"

say() {
	echo "$*" | tee -a "$summary"
}

b() {
	timeout "${BT:-60}" ssh $SO "root@$PI" "$@"
}

# Waits up to four minutes for the board's SSH after a reboot.
wait_up() {
	n=0
	while [ $n -lt 48 ]; do
		sleep 5
		if b 'uname -s' 2> /dev/null | grep -q zedBSD; then
			return 0
		fi
		n=$((n + 1))
	done
	return 1
}

say "start $(date +%H:%M:%S)"
os=$(b 'uname -sm' 2>&1)
case $os in
zedBSD*) ;;
*) say "FAIL $PI is not zedBSD: $os"; exit 1 ;;
esac

# The kernel, with a reboot when it changes.
if [ -n "${KERNEL:-}" ]; then
	want=$(cksum < "$KERNEL" | cut -d' ' -f1)
	have=$(b 'cksum < /boot/boot0/vmunix' | cut -d' ' -f1)
	if [ "$want" != "$have" ]; then
		b 'test -e /boot/boot0/vmunix.p025old || cp /boot/boot0/vmunix /boot/boot0/vmunix.p025old'
		timeout 300 scp $SO -q "$KERNEL" "root@$PI:/boot/boot0/vmunix.new" &&
		    b 'test "$(cksum < /boot/boot0/vmunix.new | cut -d" " -f1)" = '"$want"' && mv /boot/boot0/vmunix.new /boot/boot0/vmunix && sync' ||
		    { say "FAIL the kernel was not copied"; exit 1; }
		b 'sync; (sleep 1; /sbin/reboot) > /dev/null 2>&1 &'
		sleep 20
		wait_up || { say "FAIL the board did not come back after the reboot"; exit 1; }
		say "rebooted $(date +%H:%M:%S)"
	fi
fi
say "board $PI $(b 'uname -v' 2>&1) vmunix $(b 'cksum < /boot/boot0/vmunix' | cut -d' ' -f1) cmdline $(b 'cat /boot/boot0/cmdline.txt')"

# The GPU, started once per boot; the stage lines name the QPUs and the clock.
if b 'dmesg' | grep -q 'held; root starts it'; then
	if ! b 'dmesg' | grep -q 'v3d: V1 '; then
		BT=120 b 'sysctl hw.gpu.start=1' > "$out/start.txt" 2>&1
		say "started: $(tr '\n' ' ' < "$out/start.txt")"
	fi
fi
b 'dmesg' > "$out/dmesg-boot.txt" 2>&1
grep -E 'v3d: (V2|V3)|N0 fb |R0 mode' "$out/dmesg-boot.txt" | sed 's/^/gpu /' | tee -a "$summary"
say "nodes $(b 'ls /dev/gpu*' 2>&1 | tr '\n' ' ')"

# The programs, copied to /root/p025.
D=/root/p025
ENV="XDG_RUNTIME_DIR=$D/run VULKAN_ZEDBSD_WSI_LOG=1"
MVIEW_ENV=${MVIEW_ENV:-}
WAYLAND=/bin/wayland
MVIEW=/bin/mview
b "mkdir -p $D/run $D/bin $D/lib && chmod 700 $D/run"
if [ -n "${PROGRAMS:-}" ]; then
	shot=
	[ "${SHOT:-0}" = 1 ] && shot=$PROGRAMS/bin/keiland-shot
	timeout 300 scp $SO -q "$PROGRAMS/bin/wayland" "$PROGRAMS/bin/mview" $shot "root@$PI:$D/bin/" &&
	    timeout 300 scp $SO -q "$PROGRAMS"/dynamic/*.so "root@$PI:$D/lib/" || say "FAIL the programs were not copied"
	ENV="$ENV LD_LIBRARY_PATH=$D/lib"
	WAYLAND=$D/bin/wayland
	MVIEW=$D/bin/mview
fi
b "test -f $D/qs40/model.txt" || timeout 300 scp $SO -q -r userland/tests/mview/models/qs40 "root@$PI:$D/" || say "FAIL the model was not copied"
say "programs $(b "cksum $WAYLAND $MVIEW $D/lib/libvulkan.so" 2>&1 | awk '{print $1}' | tr '\n' ' ')"

# The compositor on its own socket (a stale socket of an earlier run removed first).
b "rm -f $D/run/wayland-0 $D/run/keiland-shot.sock; env $ENV nohup $WAYLAND --socket=$D/run/wayland-0 --desktop-client=none > $D/wayland.log 2>&1 < /dev/null & echo \$! > $D/wayland.pid; sleep 8; test -S $D/run/wayland-0 && grep -q 'step=socket' $D/wayland.log && echo compositor up || { echo compositor down; tail -5 $D/wayland.log; }" > "$out/compositor.txt" 2>&1
say "$(tr '\n' ' ' < "$out/compositor.txt")"
say "probe $(b "grep -E 'step=output-swapchain' $D/wayland.log | tail -1" 2>&1)"

# One mview run: NAME OPTIONS...
run() {
	name=$1
	shift
	b 'dmesg' > "$out/$name.before" 2>&1
	take=
	[ "${SHOT:-0}" = 1 ] && take="& sleep $((${SPIN:-12} / 2 + 6)); $D/bin/keiland-shot --socket $D/run/keiland-shot.sock $D/shot-$name.png; wait"
	BT=$((${SPIN:-12} + 150)) b "env $ENV $MVIEW_ENV $MVIEW --display=$D/run/wayland-0 --model=$D/qs40 --token=$name $* ${MVIEW_ARGS:-} $take" > "$out/$name.txt" 2>&1
	say "$name $(grep -E 'MVIEW (PRESENT|SPIN|STAGES|FAILED|HITCHES)' "$out/$name.txt" | sed 's/frames=[0-9]* seconds=[0-9.]* //' | tr '\n' ' ')"
	grep -E 'VULKAN WSI' "$out/$name.txt" | sed "s/^/$name /" | tee -a "$summary"
	grep -vE 'MVIEW|VULKAN WSI' "$out/$name.txt" | head -4 | sed "s/^/$name other /" | tee -a "$summary"
	b 'dmesg' > "$out/$name.after" 2>&1
	grep -F -x -v -f "$out/$name.before" "$out/$name.after" > "$out/$name.kernel"
	grep -vE 'v3d: perf|display: perf|flip report' "$out/$name.kernel" | head -6 | sed "s/^/$name kernel /" | tee -a "$summary"
	grep -E 'v3d: perf' "$out/$name.kernel" | tail -2 | sed "s/^/$name perf /" | tee -a "$summary"
	if [ "${SHOT:-0}" = 1 ]; then
		timeout 120 scp $SO -q "root@$PI:$D/shot-$name.png" "$out/" > /dev/null 2>&1 && say "$name shot $out/shot-$name.png"
	fi
	sleep 3
}

[ "${WINDOWED:-0}" = 1 ] && run windowed --spin="${SPIN:-12}" --windowed --size=1280x720 --copies=1
[ "${STILL:-0}" = 1 ] && run still --timeout-s="${SPIN:-12}" --windowed --size=1280x720 --copies=1
for copies in ${COPIES-1 21}; do
	run "c$copies" --spin="${SPIN:-12}" --copies="$copies" --content=game --present=mailbox --images=4
done

b "tail -n +1 $D/wayland.log" > "$out/wayland.log" 2>&1
say "compositor lendable $(grep -o 'lendable=[0-9]' "$out/wayland.log" | tail -1) direct=1 $(grep -c 'KWL SCANOUT direct=1' "$out/wayland.log") lend $(grep -c 'KWL SCANOUT lend images' "$out/wayland.log") no-window $(grep -c 'KWL SCANOUT direct=0 reason=no-window' "$out/wayland.log") window $(grep -c 'KWL MODE window' "$out/wayland.log") refused $(grep -c 'lend refused' "$out/wayland.log") failed $(grep -cE 'KWL (FAILED|MODE_ERROR|VULKAN_ERROR)' "$out/wayland.log")"
grep -E 'KWL SCANOUT (direct|lend)' "$out/wayland.log" | tail -6 | sed 's/^/compositor /' | tee -a "$summary"
b "kill \$(cat $D/wayland.pid); sleep 1; dmesg | grep -iE 'fault|lost|hang|panic|abort' | tail -8" > "$out/kernel.txt" 2>&1
sed 's/^/kernel /' "$out/kernel.txt" | tee -a "$summary"
say "end $(date +%H:%M:%S)"
