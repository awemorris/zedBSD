#!/bin/sh
# ws322-p007 on the VisionFive 2, in one run: chagall.bin put at the driver's path (when the board lacks it), BUILD's
# kernel (plan/ws322/tests/config-vf2-video-test.mk: the Wave511 as the render node's video engine, /dev/wave5-test)
# booted by the board's regression (plan/ws291/tests/vf2-regress.sh with KEEP=1), then on the same boot, each step
# with the engine's counts and the core's health (/dev/wave5-test) and the kernel lines it added:
#   S1  vkvideo-probe --time --no-hash on the 1080p30, 1080p60 and 2160p30 raw streams (the decode's time), then
#       --expect on the first frames of the 1080p60 and 2160p30 ones (the board reads a 1080p frame in about 0.5 s)
#   F1  the 1080p30 stream with a coding reset before every IDR picture after the first (--reset-at-idr: flushes),
#       240 frames against the reference, then the whole stream
#   F2  two sessions at once: 120 frames of the 1080p30 and 1080p60 streams by two vkvideo-probes
#   F3  a picture completion gone unseen ("lose 1") while two sessions decode 180 and 240 frames: the core recovered, both go on
#   F4  the vendor's task buffer ("task 1000"), whose stall is a real timeout, then shares of 1250, 1500 and 1750 and
#       the vendor's size at a 16 MiB alignment, each on the whole 1080p30 stream: the recoveries each took
#   F6  a session killed while it decodes, then another session
#   V   media-probe --video-hash on the 1080p30 MP4; Video Player on the 1080p30 and 1080p60 MP4s in a compositor of
#       its own for 16 s (FRAMES, top, the 1080p60 screen's PNG)
#   J   vkvideo-probe --expect on each JVT stream of JVT (make-jvt.sh)
#   F5  ten completions unseen ("lose 10"): three recoveries in a row stop the core; the device still lists video,
#       a session is refused, the GPU still computes
# At the end the files put on the board are removed, the board's own kernel goes back (plan/tools/vf2/run.sh) and the
# board's files are checked by their cksum.  Everything else the board gets is under /tmp.  The password comes from
# the environment, never from this file.
#   VF2_HOST=kei@10.0.30.2 VF2_PASSWORD=... VF2_PLUG=AC276E432AE6 sh plan/ws322/tests/p007/run-vf2.sh OUT BUILD P004 P007 JVT FIRMWARE
# P004 and P007 are the clips of p004's and p007's make-clips.sh, JVT make-jvt.sh's output, FIRMWARE the directory with
# chagall.bin (an empty JVT leaves J out).  OUT/summary.txt holds what is printed.  SKIP_REGRESS=1 boots BUILD's kernel
# without the regression.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
out=${1:?usage: run-vf2.sh OUT BUILD P004 P007 JVT FIRMWARE}
build=${2:?the build directory}
c4=${3:?the p004 clips}
c7=${4:?the p007 clips}
jvt=${5:?the JVT streams}
firmware=${6:?the firmware directory}
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
kernel=$build/vmunix
H=/tmp/p7
F=/lib/firmware/wave511

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

# 1. The regression from /tmp/p7, which boots BUILD's kernel and leaves it on the board.
if [ "${SKIP_REGRESS:-0}" = 1 ]; then
	mkdir -p "$out/regress"
	bcp "$VF2_HOST:/boot/boot0/vmunix.$old" "$out/regress/board-vmunix" || { say "FAIL the board's kernel was not read back"; exit 1; }
	timeout 900 sh plan/tools/vf2/run.sh "$kernel" "$out/boot" > "$out/boot.txt" 2>&1
	say "boot $(tail -3 "$out/boot.txt" | tr '\n' ' ')"
	b "mkdir -p $H/bin $H/lib $H/run && chmod 700 $H/run"
	bcp "$build/bin/wayland" "$build/bin/files" "$build/bin/keiland-shot" "$VF2_HOST:$H/bin/" && bcp "$build"/dynamic/*.so "$VF2_HOST:$H/lib/" ||
		say "FAIL the files were not copied"
else
	KERNEL=$kernel VF2_DIR=$H KEEP=1 OLD=$old timeout 7000 sh plan/ws291/tests/vf2-regress.sh "$out/regress" "$build" > "$out/regress.txt" 2>&1
	say "regress exit=$?"
	sed 's/^/regress: /' "$out/regress/summary.txt" >> "$summary"
fi

# 2. The checks on the same boot.
b "mkdir -p $H/bin $H/c4 $H/c7 $H/jvt"
bcp "$build/bin/media-probe" "$build/bin/vkvideo-probe" "$build/bin/videoplayer" "$build/bin/vkcompute" "$VF2_HOST:$H/bin/" &&
	bcp "$c4"/streams/pb-main-352.h264 "$c4"/streams/pb-main-352.sha256 "$c4/h264-1080p30.mp4" "$VF2_HOST:$H/c4/" &&
	bcp "$c7"/*.h264 "$c7"/*.sha256 "$c7/h264-1080p60.mp4" "$VF2_HOST:$H/c7/" ||
	say "FAIL the check files were not copied"
if ls "$jvt"/*.h264 > /dev/null 2>&1; then
	bcp "$jvt"/*.h264 "$jvt"/*.sha256 "$VF2_HOST:$H/jvt/" || say "FAIL the JVT streams were not copied"
fi
cat > "$out/board.sh" <<'BOARD'
H=/tmp/p7
export LD_LIBRARY_PATH=$H/lib XDG_RUNTIME_DIR=$H/run
P=$H/bin/vkvideo-probe
T() { echo "$1 test $(cat /dev/wave5-test)"; }
W() { echo "$1" > /dev/wave5-test; }
kernel() { grep -F -x -v -f $H/k.before $H/k.after | grep -v dw-mmc | grep -iE 'wave5|vf2-vpu|video|fault|lost|fatal|panic|reset|quarantine|hang|abandon|refus|fail|recover|stopped' | head -${2:-8} | sed "s/^/$1 kernel /"; }
mark() { dmesg > $H/k.before; W clear; }
seen() { dmesg > $H/k.after; kernel "$1" "${2:-6}"; }
line() { echo "$1 exit=$2 $(grep -c MISMATCH $3) mismatches $(grep -c ' status ' $3) failed | $(tail -2 $3 | tr '\n' ' ')"; }
probe() {
	tag=$1; shift
	mark
	timeout 900 $P "$@" > $H/o-$tag.txt 2>&1
	line "$tag" $? $H/o-$tag.txt
	T "$tag"; seen "$tag"
}
echo "S0 boot $(ls /boot/boot0/log | tail -1)"
dmesg | grep -E 'vf2-vpu|wave5' | head -6 | sed 's/^/S0 /'
timeout 60 $P --list 2>&1 | grep -iE 'family|video' | head -6 | sed 's/^/S0 list /'
T S0
probe S1-1080p30 --time --no-hash $H/c7/h264-1080p30.h264
probe S1-1080p60 --time --no-hash $H/c7/h264-1080p60.h264
probe S1-2160p30 --time --no-hash $H/c7/h264-2160p30.h264
probe S1-1080p60-hash --frames=120 --expect=$H/c7/h264-1080p60.sha256 $H/c7/h264-1080p60.h264
probe S1-2160p30-hash --frames=30 --expect=$H/c7/h264-2160p30.sha256 $H/c7/h264-2160p30.h264
probe F1-reset --reset-at-idr --frames=240 --expect=$H/c7/h264-1080p30.sha256 $H/c7/h264-1080p30.h264
probe F1-reset-all --reset-at-idr --no-hash $H/c7/h264-1080p30.h264
mark
timeout 900 $P --frames=120 --expect=$H/c7/h264-1080p30.sha256 $H/c7/h264-1080p30.h264 > $H/o-F2a.txt 2>&1 &
pa=$!
timeout 900 $P --frames=120 --expect=$H/c7/h264-1080p60.sha256 $H/c7/h264-1080p60.h264 > $H/o-F2b.txt 2>&1
eb=$?
wait $pa
line F2-a $? $H/o-F2a.txt
line F2-b $eb $H/o-F2b.txt
T F2; seen F2
mark
timeout 900 $P --frames=180 --expect=$H/c7/h264-1080p30.sha256 $H/c7/h264-1080p30.h264 > $H/o-F3a.txt 2>&1 &
pa=$!
timeout 900 $P --frames=240 --expect=$H/c7/h264-1080p60.sha256 $H/c7/h264-1080p60.h264 > $H/o-F3b.txt 2>&1 &
pb=$!
sleep 5
W "lose 1"
wait $pa; ea=$?
wait $pb; eb=$?
line F3-a $ea $H/o-F3a.txt
line F3-b $eb $H/o-F3b.txt
T F3; seen F3 10
probe F3-after --frames=60 --expect=$H/c7/h264-1080p30.sha256 $H/c7/h264-1080p30.h264
for share in "1000" "1250" "1500" "1750" "1000 16384"; do
	tag=F4-$(echo $share | tr ' ' '-')
	mark
	W "task $share"
	timeout 900 $P --time --no-hash $H/c7/h264-1080p30.h264 > $H/o-$tag.txt 2>&1
	line "$tag" $? $H/o-$tag.txt
	T "$tag"; seen "$tag" 4
done
W "task 0"
probe F4-after --frames=60 --expect=$H/c7/h264-1080p30.sha256 $H/c7/h264-1080p30.h264
mark
timeout 900 $P --no-hash $H/c7/h264-1080p60.h264 > $H/o-F6.txt 2>&1 &
pa=$!
sleep 4
kill -9 $pa
wait $pa
echo "F6 killed exit=$? $(grep -c . $H/o-F6.txt) lines"
sleep 2
T F6; seen F6
probe F6-after --expect=$H/c4/pb-main-352.sha256 $H/c4/pb-main-352.h264
for clip in c4/h264-1080p30; do
	n=$(basename $clip)
	mark
	timeout 900 $H/bin/media-probe --video-hash --time $H/$clip.mp4 > $H/$n.hash 2> $H/$n.err
	echo "V-$n media-probe exit=$? $(wc -l < $H/$n.hash) hashes"
	grep -E 'backend|elapsed_us|frames=|GPU holds|read back' $H/$n.err | head -4 | sed "s/^/V-$n /"
	T "V-$n"; seen "V-$n"
done
for clip in c4/h264-1080p30 c7/h264-1080p60; do
	n=$(basename $clip)
	mark
	rm -f $H/run/wayland-0
	$H/bin/wayland --socket=$H/run/wayland-0 "--desktop-client=$H/bin/files --desktop" > $H/w-$n.log 2>&1 < /dev/null &
	wpid=$!
	sleep 15
	WAYLAND_DISPLAY=wayland-0 $H/bin/videoplayer --timeout-s=16 $H/$clip.mp4 > $H/p-$n.log 2>&1 < /dev/null &
	ppid=$!
	sleep 8
	[ $n = h264-1080p60 ] && timeout 60 $H/bin/keiland-shot --socket $H/run/keiland-shot.sock $H/v-$n.png > $H/s-$n.txt 2>&1
	top -b -n 1 2> /dev/null | head -12 | grep -iE 'cpu|videoplayer|wayland' | head -4 | sed "s/^/VP-$n top /"
	wait $ppid
	echo "VP-$n player exit=$?"
	grep -E 'VIDEOPLAYER (PRESENT|FRAMES|FAILED|DONE)|GPU holds|read back' $H/p-$n.log | tail -5 | sed "s/^/VP-$n /"
	kill $wpid 2> /dev/null; sleep 3; kill -9 $wpid 2> /dev/null
	ps -e | grep "$H/bin/files" | grep -v grep | while read pid rest; do kill $pid; done
	echo "VP-$n compositor $(grep -cE 'KWL (FAILED|VULKAN_ERROR)' $H/w-$n.log) errors"
	T "VP-$n"; seen "VP-$n"
done
mark
for s in $H/jvt/*.h264; do
	[ -f "$s" ] || continue
	n=$(basename $s .h264)
	timeout 300 $P --expect=$H/jvt/$n.sha256 $s > $H/j.txt 2>&1
	e=$?
	echo "J $n exit=$e frames=$(wc -l < $H/jvt/$n.sha256) $(grep -c MISMATCH $H/j.txt) mismatches $(grep -c ' status ' $H/j.txt) failed | $(grep -vE 'MISMATCH| status ' $H/j.txt | tail -1)"
done
T J; seen J 12
mark
W "lose 10"
timeout 900 $P --no-hash $H/c7/h264-1080p30.h264 > $H/o-F5.txt 2>&1
line F5 $? $H/o-F5.txt
T F5; seen F5 12
W "lose 0"
timeout 60 $P --list 2>&1 | grep -iE 'family|video' | head -4 | sed 's/^/F5-after list /'
probe F5-after --expect=$H/c4/pb-main-352.sha256 $H/c4/pb-main-352.h264
timeout 300 $H/bin/vkcompute > $H/o-F5c.txt 2>&1
echo "F5-after vkcompute exit=$? $(tail -1 $H/o-F5c.txt)"
dmesg > $H/dmesg-end.txt
BOARD
bcp "$out/board.sh" "$VF2_HOST:$H/check.sh" || say "FAIL the board's script was not copied"
printf '%s\n' "$VF2_PASSWORD" | BT=5400 b "sudo -S sh $H/check.sh" > "$out/board.txt" 2>&1
sed 's/^\[sudo\][^:]*: //' "$out/board.txt" | tee -a "$summary"
bcp "$VF2_HOST:$H/h264-1080p30.hash" "$VF2_HOST:$H/v-h264-1080p60.png" \
	"$VF2_HOST:$H/p-h264-1080p30.log" "$VF2_HOST:$H/p-h264-1080p60.log" "$VF2_HOST:$H/dmesg-end.txt" "$out/" > /dev/null 2>&1
for f in o-S1-1080p30 o-S1-1080p60 o-S1-2160p30 o-F1-reset o-F1-reset-all o-F3a o-F3b o-F4-1000 o-F4-1250 o-F4-1500 o-F4-1750 o-F5; do bcp "$VF2_HOST:$H/$f.txt" "$out/" > /dev/null 2>&1; done
compare() {
	/usr/bin/python3 -I - "$1" "$2" <<'PY'
import sys
def read(path):
    rows = []
    try:
        for line in open(path):
            parts = line.split()
            if len(parts) == 2 and len(parts[1]) == 64:
                rows.append((int(parts[0]), parts[1]))
    except OSError:
        pass
    return rows
got = read(sys.argv[1])
want = read(sys.argv[2])
same = sum(1 for a, b in zip(got, want) if a[1] == b[1])
verdict = "MATCH" if got and len(got) == len(want) and same == len(want) else "DIFFER"
print("%s %d of %d frames, %d equal in order" % (verdict, len(got), len(want), same))
PY
}
say "V-h264-1080p30 hashes $(compare "$out/h264-1080p30.hash" "$c4/h264-1080p30.ref")"
[ -f "$out/v-h264-1080p60.png" ] && say "VP png $(cksum < "$out/v-h264-1080p60.png")"
say "J $(grep -c '^J .* exit=0 ' "$out/board.txt") of $(grep -c '^J ' "$out/board.txt") streams match every frame"

# 3. The board as it was: the files removed, its own kernel back, checked by cksum.
bsudo "rm -rf $H; sync"
[ "$added" = 1 ] && bsudo "rm -rf $F; sync"
timeout 900 sh plan/tools/vf2/run.sh "$out/regress/board-vmunix" "$out/restore" > "$out/restore.txt" 2>&1
say "restore $(tail -2 "$out/restore.txt" | tr '\n' ' ')"
say "board after $(b "cksum /boot/boot0/vmunix /boot/boot0/vmunix.$old /etc/group /boot/boot0/zedbsd.cfg; ls /tmp $F; uname -sm" 2>&1 | tr '\n' ' ')"
say "own kernel $(cksum < "$out/regress/board-vmunix")"
say "end $(date +%H:%M)"
sync
