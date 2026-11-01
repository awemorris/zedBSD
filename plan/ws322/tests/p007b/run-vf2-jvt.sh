#!/bin/sh
# ws322-p007b on the VisionFive 2, in one run: chagall.bin put at the driver's path (when the board lacks it), BUILD's
# kernel (plan/ws322/tests/config-vf2-video-test.mk) booted, then on that boot:
#   J   vkvideo-probe --expect on every JVT stream of JVT (p007's make-jvt.sh), each run's whole output kept
#       (OUT/j/<name>.txt), with the engine's counts (/dev/wave5-test) and the kernel lines the runs added
#   M   media-probe --video-hash on MP4s of the JVT streams named in MEDIA (the stream as it is, in an MP4 of 25
#       frames a second, made here by ffmpeg), each picture's hash compared in order with the stream's reference
#   VP  Video Player on the MP4 of the first stream of MEDIA in a compositor of its own (FRAMES)
# At the end the files put on the board are removed, the board's own kernel goes back (plan/tools/vf2/run.sh) and the
# board's files are checked by their cksum.  Everything else the board gets is under /tmp.  The password comes from
# the environment, never from this file.
#   VF2_HOST=kei@10.0.30.2 VF2_PASSWORD=... VF2_PLUG=AC276E432AE6 sh plan/ws322/tests/p007b/run-vf2-jvt.sh OUT BUILD JVT FIRMWARE
# OUT/summary.txt holds what is printed.  MEDIA (default below) are names of JVT's streams.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
out=${1:?usage: run-vf2-jvt.sh OUT BUILD JVT FIRMWARE}
build=${2:?the build directory}
jvt=${3:?the JVT streams}
firmware=${4:?the firmware directory}
media=${MEDIA:-CAPM3_Sony_D CAWP1_TOSHIBA_E CACQP3_Sony_D BAMQ1_JVC_C MR2_TANDBERG_E MR3_TANDBERG_B CVSE3_Sony_H}
: "${VF2_HOST:?VF2_HOST=user@host}"
: "${VF2_PASSWORD:?VF2_PASSWORD}"
: "${VF2_PLUG:?VF2_PLUG}"
export SSHPASS="$VF2_PASSWORD" VF2_PLUG
SSHOPT="-o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR -o ConnectTimeout=8"
mkdir -p "$out/j" "$out/mp4"
summary=$out/summary.txt
: > "$summary"
say() { echo "$*" | tee -a "$summary"; }
b() { timeout "${BT:-60}" sshpass -e ssh $SSHOPT "$VF2_HOST" "$@"; }
bsudo() { printf '%s\n' "$VF2_PASSWORD" | b "sudo -S sh -c '$1'" 2> /dev/null; }
bcp() { timeout "${BT:-600}" sshpass -e scp $SSHOPT "$@"; }
old=q1825orig
kernel=$build/vmunix
H=/tmp/p7b
F=/lib/firmware/wave511

# The MP4s of the media streams, on the host.
for n in $media; do
	timeout 120 ffmpeg -hide_banner -loglevel error -y -r 25 -i "$jvt/$n.h264" -c copy "$out/mp4/$n.mp4" ||
		say "FAIL $n.mp4 was not made"
done

say "start $(date +%H:%M) kernel $(cksum < "$kernel")"
say "board before $(b "cksum /boot/boot0/vmunix /boot/boot0/vmunix.$old /etc/group /boot/boot0/zedbsd.cfg; df -k / /tmp; ls $F" 2>&1 | tr '\n' ' ')"

# The firmware at the driver's path, before the boot; remembered when the board did not have it.
added=0
if ! b "test -f $F/chagall.bin" > /dev/null 2>&1; then
	added=1
	bcp "$firmware/chagall.bin" "$VF2_HOST:/tmp/chagall.bin" || { say "FAIL the firmware was not copied"; exit 1; }
	bsudo "mkdir -p $F && cp /tmp/chagall.bin $F/chagall.bin && rm -f /tmp/chagall.bin && sync"
fi
say "firmware added=$added $(b "cksum $F/chagall.bin" 2>&1)"

# 1. BUILD's kernel booted, the board's own read back first.
mkdir -p "$out/regress"
bcp "$VF2_HOST:/boot/boot0/vmunix.$old" "$out/regress/board-vmunix" || { say "FAIL the board's kernel was not read back"; exit 1; }
sync
timeout 900 sh plan/tools/vf2/run.sh "$kernel" "$out/boot" > "$out/boot.txt" 2>&1
say "boot $(tail -3 "$out/boot.txt" | tr '\n' ' ')"
b "mkdir -p $H/bin $H/lib $H/run $H/jvt $H/mp4 $H/j && chmod 700 $H/run"
bcp "$build/bin/wayland" "$build/bin/files" "$build/bin/vkvideo-probe" "$build/bin/media-probe" "$build/bin/videoplayer" "$VF2_HOST:$H/bin/" &&
	bcp "$build"/dynamic/*.so "$VF2_HOST:$H/lib/" &&
	bcp "$jvt"/*.h264 "$jvt"/*.sha256 "$VF2_HOST:$H/jvt/" &&
	bcp "$out"/mp4/*.mp4 "$VF2_HOST:$H/mp4/" ||
	say "FAIL the files were not copied"

# 2. The checks on the board.
cat > "$out/board.sh" <<'BOARD'
H=/tmp/p7b
export LD_LIBRARY_PATH=$H/lib XDG_RUNTIME_DIR=$H/run
P=$H/bin/vkvideo-probe
T() { echo "$1 test $(cat /dev/wave5-test)"; }
W() { echo "$1" > /dev/wave5-test; }
mark() { dmesg > $H/k.before; W clear; }
seen() { dmesg > $H/k.after; grep -F -x -v -f $H/k.before $H/k.after | grep -v dw-mmc | grep -iE 'wave5|vf2-vpu|video|fault|lost|fatal|panic|reset|hang|refus|fail|recover|stopped|differs' | head -${2:-8} | sed "s/^/$1 kernel /"; }
echo "S0 boot $(ls /boot/boot0/log | tail -1)"
dmesg | grep -E 'vf2-vpu|wave5' | head -4 | sed 's/^/S0 /'
for s in $H/jvt/*.h264; do
	[ -f "$s" ] || continue
	n=$(basename $s .h264)
	mark
	timeout 300 $P --expect=$H/jvt/$n.sha256 $s > $H/j/$n.txt 2>&1
	e=$?
	echo "J $n exit=$e frames=$(wc -l < $H/jvt/$n.sha256) $(grep -c MISMATCH $H/j/$n.txt) mismatches $(grep -c ' status ' $H/j/$n.txt) failed | $(grep -vE 'MISMATCH| match$| status ' $H/j/$n.txt | tail -2 | tr '\n' ' ')"
	T "J-$n" | grep -v ' errors 0 ' ; seen "J-$n" 4
done
for f in $H/mp4/*.mp4; do
	n=$(basename $f .mp4)
	mark
	timeout 600 $H/bin/media-probe --video-hash $f > $H/mp4/$n.hash 2> $H/mp4/$n.err
	echo "M $n media-probe exit=$? $(wc -l < $H/mp4/$n.hash) hashes | $(grep -E 'backend|frames=' $H/mp4/$n.err | head -2 | tr '\n' ' ')"
	T "M-$n"; seen "M-$n" 4
done
for n in VPCLIP; do
	mark
	rm -f $H/run/wayland-0
	$H/bin/wayland --socket=$H/run/wayland-0 "--desktop-client=$H/bin/files --desktop" > $H/w.log 2>&1 < /dev/null &
	wpid=$!
	sleep 15
	WAYLAND_DISPLAY=wayland-0 timeout 60 $H/bin/videoplayer --timeout-s=16 $H/mp4/$n.mp4 > $H/p-$n.log 2>&1 < /dev/null
	echo "VP-$n player exit=$?"
	grep -E 'VIDEOPLAYER (PRESENT|FRAMES|FAILED|DONE)|backend' $H/p-$n.log | tail -5 | sed "s/^/VP-$n /"
	kill $wpid 2> /dev/null; sleep 3; kill -9 $wpid 2> /dev/null
	ps -e | grep "$H/bin/files" | grep -v grep | while read pid rest; do kill $pid; done
	echo "VP-$n compositor $(grep -cE 'KWL (FAILED|VULKAN_ERROR)' $H/w.log) errors"
	T "VP-$n"; seen "VP-$n"
done
dmesg > $H/dmesg-end.txt
BOARD
first=${media%% *}
sed -i "s/VPCLIP/$first/" "$out/board.sh"
bcp "$out/board.sh" "$VF2_HOST:$H/check.sh" || say "FAIL the board's script was not copied"
printf '%s\n' "$VF2_PASSWORD" | BT=5400 b "sudo -S sh $H/check.sh" > "$out/board.txt" 2>&1
sed 's/^\[sudo\][^:]*: //' "$out/board.txt" | tee -a "$summary"
bcp "$VF2_HOST:$H/j/*.txt" "$out/j/" > /dev/null 2>&1
bcp "$VF2_HOST:$H/mp4/*.hash" "$VF2_HOST:$H/mp4/*.err" "$out/mp4/" > /dev/null 2>&1
bcp "$VF2_HOST:$H/dmesg-end.txt" "$VF2_HOST:$H/p-$first.log" "$out/" > /dev/null 2>&1
for n in $media; do
	got=$(awk 'NF == 2 && length($2) == 64 { print $2 }' "$out/mp4/$n.hash" 2> /dev/null)
	want=$(cat "$jvt/$n.sha256")
	same=$(printf '%s\n' "$got" > "$out/mp4/$n.got"; printf '%s\n' "$want" > "$out/mp4/$n.want"; paste -d' ' "$out/mp4/$n.got" "$out/mp4/$n.want" | awk '$1 == $2 { c++ } END { print c + 0 }')
	say "M $n $(grep -c . "$out/mp4/$n.got") of $(grep -c . "$out/mp4/$n.want") frames, $same equal in order"
done
say "J $(grep -c '^J .* exit=0 ' "$out/board.txt") of $(grep -c '^J .* exit=' "$out/board.txt") streams match every frame"

# 3. The board as it was: the files removed, its own kernel back, checked by cksum.
bsudo "rm -rf $H; sync"
[ "$added" = 1 ] && bsudo "rm -rf $F; sync"
sync
timeout 900 sh plan/tools/vf2/run.sh "$out/regress/board-vmunix" "$out/restore" > "$out/restore.txt" 2>&1
say "restore $(tail -2 "$out/restore.txt" | tr '\n' ' ')"
say "board after $(b "cksum /boot/boot0/vmunix /boot/boot0/vmunix.$old /etc/group /boot/boot0/zedbsd.cfg; ls /tmp $F; uname -sm" 2>&1 | tr '\n' ' ')"
say "own kernel $(cksum < "$out/regress/board-vmunix")"
say "end $(date +%H:%M)"
sync
