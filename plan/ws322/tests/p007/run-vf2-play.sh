#!/bin/sh
# ws322-p007's playback check on the VisionFive 2, in one run: chagall.bin put at the driver's path (when the board
# lacks it), BUILD's kernel booted (plan/tools/vf2/run.sh), then Video Player on P007/h264-1080p60.mp4 in a compositor of
# its own for 30 s, twice, with nothing else running (no screenshot, no top): its FRAMES lines (shown and late every
# 100 frames), the engine's counts (/dev/wave5-test) and the kernel lines; at the end the files put on the board are
# removed, the board's own kernel goes back and the board's files are checked by their cksum.  The password comes from
# the environment, never from this file.
#   VF2_HOST=kei@10.0.30.2 VF2_PASSWORD=... VF2_PLUG=AC276E432AE6 sh plan/ws322/tests/p007/run-vf2-play.sh OUT BUILD P007 FIRMWARE
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
out=${1:?usage: run-vf2-play.sh OUT BUILD P007 FIRMWARE}
build=${2:?the build directory}
c7=${3:?the p007 clips}
firmware=${4:?the firmware directory}
: "${VF2_HOST:?VF2_HOST=user@host}"
: "${VF2_PASSWORD:?VF2_PASSWORD}"
: "${VF2_PLUG:?VF2_PLUG}"
export SSHPASS="$VF2_PASSWORD" VF2_PLUG
SSHOPT="-o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR -o ConnectTimeout=8"
mkdir -p "$out"
summary=$out/summary.txt
: > "$summary"
say() { echo "$*" | tee -a "$summary"; }
b() { timeout "${BT:-60}" sshpass -e ssh $SSHOPT "$VF2_HOST" "$@"; }
bsudo() { printf '%s\n' "$VF2_PASSWORD" | b "sudo -S sh -c '$1'" 2> /dev/null; }
bcp() { timeout "${BT:-600}" sshpass -e scp $SSHOPT "$@"; }
old=q1825orig
H=/tmp/p7
F=/lib/firmware/wave511
say "start $(date +%H:%M) kernel $(cksum < "$build/vmunix")"
say "board before $(b "cksum /boot/boot0/vmunix /boot/boot0/vmunix.$old /etc/group /boot/boot0/zedbsd.cfg; ls $F" 2>&1 | tr '\n' ' ')"
bcp "$VF2_HOST:/boot/boot0/vmunix.$old" "$out/board-vmunix" || { say "FAIL the board's kernel was not read back"; exit 1; }
added=0
if ! b "test -f $F/chagall.bin" > /dev/null 2>&1; then
	added=1
	bcp "$firmware/chagall.bin" "$VF2_HOST:/tmp/chagall.bin" || { say "FAIL the firmware was not copied"; exit 1; }
	bsudo "mkdir -p $F && cp /tmp/chagall.bin $F/chagall.bin && rm -f /tmp/chagall.bin && sync"
fi
timeout 900 sh plan/tools/vf2/run.sh "$build/vmunix" "$out/boot" > "$out/boot.txt" 2>&1
say "boot $(tail -2 "$out/boot.txt" | tr '\n' ' ')"
b "mkdir -p $H/bin $H/lib $H/run $H/c7 && chmod 700 $H/run"
bcp "$build/bin/wayland" "$build/bin/files" "$build/bin/videoplayer" "$VF2_HOST:$H/bin/" && bcp "$build"/dynamic/*.so "$VF2_HOST:$H/lib/" &&
	bcp "$c7/h264-1080p60.mp4" "$VF2_HOST:$H/c7/" || say "FAIL the files were not copied"
cat > "$out/board.sh" <<'BOARD'
H=/tmp/p7
export LD_LIBRARY_PATH=$H/lib XDG_RUNTIME_DIR=$H/run
dmesg | grep -E 'vf2-vpu' | head -3 | sed 's/^/P0 /'
for run in 1 2; do
	echo clear > /dev/wave5-test
	rm -f $H/run/wayland-0
	$H/bin/wayland --socket=$H/run/wayland-0 "--desktop-client=$H/bin/files --desktop" > $H/w-$run.log 2>&1 < /dev/null &
	wpid=$!
	sleep 15
	WAYLAND_DISPLAY=wayland-0 $H/bin/videoplayer --timeout-s=30 $H/c7/h264-1080p60.mp4 > $H/p-$run.log 2>&1 < /dev/null
	echo "P$run player exit=$?"
	grep -E 'VIDEOPLAYER (PRESENT|FRAMES|FAILED|DONE)' $H/p-$run.log | sed "s/^/P$run /"
	kill $wpid 2> /dev/null; sleep 3; kill -9 $wpid 2> /dev/null
	ps -e | grep "$H/bin/files" | grep -v grep | while read pid rest; do kill $pid; done
	echo "P$run compositor $(grep -cE 'KWL (FAILED|VULKAN_ERROR)' $H/w-$run.log) errors"
	echo "P$run test $(cat /dev/wave5-test)"
done
dmesg | grep -iE 'wave5|fault|lost' | tail -4 | sed 's/^/P end /'
BOARD
bcp "$out/board.sh" "$VF2_HOST:$H/check.sh" || say "FAIL the board's script was not copied"
printf '%s\n' "$VF2_PASSWORD" | BT=900 b "sudo -S sh $H/check.sh" > "$out/board.txt" 2>&1
sed 's/^\[sudo\][^:]*: //' "$out/board.txt" | tee -a "$summary"
bsudo "rm -rf $H; sync"
[ "$added" = 1 ] && bsudo "rm -rf $F; sync"
timeout 900 sh plan/tools/vf2/run.sh "$out/board-vmunix" "$out/restore" > "$out/restore.txt" 2>&1
say "restore $(tail -2 "$out/restore.txt" | tr '\n' ' ')"
say "board after $(b "cksum /boot/boot0/vmunix /boot/boot0/vmunix.$old /etc/group /boot/boot0/zedbsd.cfg; ls /tmp $F; uname -sm" 2>&1 | tr '\n' ' ')"
say "own kernel $(cksum < "$out/board-vmunix")"
say "end $(date +%H:%M)"
sync
