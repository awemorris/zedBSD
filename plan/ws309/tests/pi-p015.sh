#!/bin/sh
# ws309-p015: the game mode's lent images on a Raspberry Pi 4 or Pi 5 running zedBSD with root's SSH key of the guest
# harness (plan/tmp/guest/id_ed25519), from the top of the tree:
#   PI=ADDRESS PROGRAMS=BUILD [SESSION=1] [CYCLES=3] [COPIES=10] [SPIN=8] [VIDEO=FILE.mp4] sh plan/ws309/tests/pi-p015.sh OUT
# PROGRAMS is a build of the board's config (bin/wayland, bin/mview, bin/videoplayer and the dynamic libraries), copied
# with mview's qs40 model to /root/p015 and run with LD_LIBRARY_PATH (the board's own files are not changed).  The GPU
# is started when it is still held (sysctl hw.gpu.start=1).  The compositor runs on its own socket with no desktop
# client; mview runs in the
# game mode CYCLES times with libvulkan's chain log (VULKAN_ZEDBSD_WSI_LOG: whether its chain drew into lent images,
# their layout, its presents and the copies among them); with VIDEO, videoplayer plays the file full screen for 15 s
# (its PLAYBACK and FRAMES lines, the kernel's firmware decoder and display lines of the run).  The compositor's
# STARTUP, SCANOUT and MODE lines are counted; then the compositor is ended.  Nothing is power-cycled or rebooted.
# OUT/summary.txt holds what is printed.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
out=${1:?usage: pi-p015.sh OUT}
: "${PI:?PI=the board's address}"
: "${PROGRAMS:?PROGRAMS=the build}"
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

say "start $(date +%H:%M)"
os=$(b 'uname -sm' 2>&1)
case $os in
zedBSD*) ;;
*) say "FAIL $PI is not zedBSD: $os"; exit 1 ;;
esac
say "board $PI $os $(b 'cksum /boot/vmunix 2> /dev/null || cksum /boot/boot0/vmunix' 2>&1 | cut -d' ' -f1)"

# The GPU, started once per boot.
if b 'dmesg' | grep -q 'held; root starts it'; then
	if ! b 'dmesg' | grep -q 'V1 ok'; then
		BT=120 b 'sysctl hw.gpu.start=1' > "$out/start.txt" 2>&1
		say "started: $(tr '\n' ' ' < "$out/start.txt")"
	fi
fi
say "nodes $(b 'ls /dev/gpu*' 2>&1 | tr '\n' ' ')"

# The programs, copied to /root/p015 (/tmp/p015 for the session's user).
D=/root/p015
[ "${SESSION:-0}" = 1 ] && D=/tmp/p015
ENV="XDG_RUNTIME_DIR=$D/run WAYLAND_DISPLAY=$D/run/wayland-0 LD_LIBRARY_PATH=$D/lib VULKAN_ZEDBSD_WSI_LOG=1"
# SESSION=1: the board's own session (its compositor on /run/user/1000/wayland-0, its log session.log) instead of a
# compositor of the script's; the clients run as the session's user.
RUN="sh -c"
LOG=$D/wayland.log
if [ "${SESSION:-0}" = 1 ]; then
	U=$(b "awk -F: '\$3 == 1000 { print \$1; exit }' /etc/passwd")
	ENV="XDG_RUNTIME_DIR=/run/user/1000 WAYLAND_DISPLAY=wayland-0 LD_LIBRARY_PATH=$D/lib VULKAN_ZEDBSD_WSI_LOG=1"
	RUN="su $U -c"
	LOG=/run/user/1000/session.log
fi
b "mkdir -p $D/run $D/bin $D/lib && chmod 700 $D/run && chmod 755 $D"
timeout 300 scp $SO -q "$PROGRAMS/bin/wayland" "$PROGRAMS/bin/mview" "$PROGRAMS/bin/videoplayer" "root@$PI:$D/bin/" &&
    timeout 300 scp $SO -q "$PROGRAMS"/dynamic/*.so "root@$PI:$D/lib/" || say "FAIL the programs were not copied"
say "programs $(b "cksum $D/bin/wayland $D/bin/mview $D/lib/libvulkan.so" 2>&1 | awk '{print $1}' | tr '\n' ' ')"
if [ -n "${VIDEO:-}" ]; then
	timeout 300 scp $SO -q "$VIDEO" "root@$PI:$D/video.mp4" || say "FAIL the video was not copied"
fi
# mview's model (the image may not carry it).
timeout 300 scp $SO -q -r userland/tests/mview/models/qs40 "root@$PI:$D/" || say "FAIL the model was not copied"

# The compositor on its own socket (a socket of an earlier run removed first), or the session's.
lines=1
[ "${SESSION:-0}" = 1 ] && lines=$(($(b "wc -l < $LOG" 2>/dev/null || echo 0) + 1))
[ "${SESSION:-0}" = 1 ] || b "rm -f $D/run/wayland-0; env $ENV nohup $D/bin/wayland --socket=$D/run/wayland-0 --desktop-client=none > $D/wayland.log 2>&1 < /dev/null & echo \$! > $D/wayland.pid; sleep 8; test -S $D/run/wayland-0 && grep -q 'step=socket' $D/wayland.log && echo compositor up || { echo compositor down; tail -5 $D/wayland.log; }" > "$out/compositor.txt" 2>&1
[ "${SESSION:-0}" = 1 ] && echo "session compositor (its log from line $lines)" > "$out/compositor.txt"
say "$(tr '\n' ' ' < "$out/compositor.txt")"
say "probe $(b "grep -E 'step=output-swapchain' $LOG | tail -1" 2>&1)"

# mview in the game mode, CYCLES times.
i=0
while [ $i -lt "${CYCLES:-3}" ]; do
	i=$((i + 1))
	BT=$((${SPIN:-8} + 120)) b "$RUN 'env $ENV $D/bin/mview --model=$D/qs40 --token=g$i --spin=${SPIN:-8} --copies=${COPIES:-10} --content=game --present=mailbox --images=4'" > "$out/game$i.txt" 2>&1
	say "game $i $(grep -E 'MVIEW (SPIN|FAILED)' "$out/game$i.txt" | sed 's/frames=[0-9]* seconds=[0-9.]* //' | tr '\n' ' ')"
	grep -E 'VULKAN WSI' "$out/game$i.txt" | sed "s/^/game $i /" | tee -a "$summary"
	sleep 3
done

# The video, full screen.
if [ -n "${VIDEO:-}" ]; then
	b 'dmesg' > "$out/video.before" 2>&1
	BT=90 b "$RUN 'env $ENV timeout 40 $D/bin/videoplayer --timeout-s=15 --fullscreen $D/video.mp4'" > "$out/video.txt" 2>&1
	b 'dmesg' > "$out/video.after" 2>&1
	grep -F -x -v -f "$out/video.before" "$out/video.after" > "$out/video.kernel"
	say "video $(grep -E 'PLAYBACK|DONE|FRAMES' "$out/video.txt" | tail -3 | tr '\n' ' ')"
	grep -E 'VULKAN WSI' "$out/video.txt" | sed 's/^/video /' | tee -a "$summary"
	say "video kernel unread $(grep -c 'came back unread' "$out/video.kernel") unanswered $(grep -c 'not answered' "$out/video.kernel") no-decoder $(grep -c 'no firmware decoder' "$out/video.kernel") failed $(grep -c 'execution failed' "$out/video.kernel") lease $(grep -c 'display lease' "$out/video.kernel")"
	grep -E 'v3d: video|vchiq|mmal|decoder' "$out/video.kernel" | tail -4 | sed 's/^/video kernel /' | tee -a "$summary"
fi

# The compositor's lines.
b "tail -n +$lines $LOG" > "$out/wayland.log" 2>&1
say "compositor lendable $(grep -o 'lendable=[0-9]' "$out/wayland.log" | tail -1) direct=1 $(grep -c 'KWL SCANOUT direct=1' "$out/wayland.log") lend $(grep -c 'KWL SCANOUT lend images' "$out/wayland.log") no-window $(grep -c 'KWL SCANOUT direct=0 reason=no-window' "$out/wayland.log") window $(grep -c 'KWL MODE window' "$out/wayland.log") refused $(grep -c 'lend refused' "$out/wayland.log") failed $(grep -cE 'KWL (FAILED|MODE_ERROR|VULKAN_ERROR)' "$out/wayland.log")"
grep -E 'KWL SCANOUT (direct|lend)' "$out/wayland.log" | tail -8 | sed 's/^/compositor /' | tee -a "$summary"
[ "${SESSION:-0}" = 1 ] || b "kill \$(cat $D/wayland.pid)"
b "sleep 1; dmesg | grep -iE 'v3d|hvs|fault|lost|hang' | tail -8" > "$out/kernel.txt" 2>&1
sed 's/^/kernel /' "$out/kernel.txt" | tee -a "$summary"
say "end $(date +%H:%M)"
