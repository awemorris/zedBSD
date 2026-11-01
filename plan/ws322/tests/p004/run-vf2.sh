#!/bin/sh
# ws322-p004 on the VisionFive 2, in one run: chagall.bin put at the driver's path (/lib/firmware/wave511, when the board
# lacks it), BUILD's kernel (the Wave511 brought up at boot as the render node's video engine) booted by the board's
# regression (plan/ws291/tests/vf2-regress.sh with KEEP=1: vkmatrix, vkcoherency, vkcompute, the desktop's PNG), then on
# the same boot:
#   V0  the decoder's kernel lines and vkvideo-probe --list (the video family, the H.264 extensions)
#   V1  vkvideo-probe --expect on each H.264 stream of CLIPS/streams (the ws083 streams and the probe's 1080p stream), the
#       frames' SHA-256 against the host's ffmpeg's, and --sampled (the planes copied by the GPU) on two of them
#   V2  media-probe --video-hash --time on CLIPS/h264-1080p30.mp4 (libmedia's Vulkan Video, 600 frames, the ring wraps),
#       compared on the host with CLIPS/h264-1080p30.ref, and libmedia's own lines
#   V3  Video Player in a compositor of its own on the same clip for 16 s: whether libmedia gave it pictures the GPU holds,
#       its FRAMES line (shown, late), the screen's PNG 8 s in
# with the kernel lines each step added; at the end the files put on the board are removed, the board's own kernel
# goes back (plan/tools/vf2/run.sh) and the board's files are checked by their cksum.  Everything else the board gets is
# under /tmp (a tmpfs).  CLIPS is make-clips.sh's output, FIRMWARE the directory with chagall.bin.  The password comes
# from the environment, never from this file.
#   VF2_HOST=kei@10.0.30.2 VF2_PASSWORD=... VF2_PLUG=AC276E432AE6 sh plan/ws322/tests/p004/run-vf2.sh OUT BUILD CLIPS FIRMWARE
# BUILD is the tree's build of plan/ws322/tests/config-vf2-video.mk with vf2-regress.sh's programs and bin/media-probe,
# bin/vkvideo-probe, bin/videoplayer.  OUT/summary.txt holds what is printed, OUT/dmesg-end.txt the kernel's lines at
# the end.  SKIP_REGRESS=1 boots BUILD's kernel without the regression, NO_V1=1 leaves V1 out, KERNEL
# names another kernel to boot (BUILD's by default).
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
out=${1:?usage: run-vf2.sh OUT BUILD CLIPS FIRMWARE}
build=${2:?the build directory}
clips=${3:?the clips directory}
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
bcp() { timeout "${BT:-300}" sshpass -e scp $SSHOPT "$@"; }
old=q1825orig
NO_V1=${NO_V1:-0}
kernel=${KERNEL:-$build/vmunix}
H=/tmp/p4
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

# 1. The regression from /tmp/p4, which boots BUILD's kernel and leaves it on the board.
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

# 2. The decode checks on the same boot.
b "mkdir -p $H/bin $H/clips"
bcp "$build/bin/media-probe" "$build/bin/vkvideo-probe" "$build/bin/videoplayer" "$VF2_HOST:$H/bin/" &&
	bcp "$clips"/streams/*.h264 "$clips"/streams/*.sha256 "$clips/h264-1080p30.mp4" "$VF2_HOST:$H/clips/" ||
	say "FAIL the decode files were not copied"
cat > "$out/board.sh" <<BOARD
H=$H
export LD_LIBRARY_PATH=\$H/lib XDG_RUNTIME_DIR=\$H/run
lines() { dmesg | wc -l | tr -d ' '; }
kernel() { grep -F -x -v -f \$H/k.before \$H/k.after | grep -v dw-mmc | grep -iE 'wave5|vf2-vpu|video|fault|lost|fatal|panic|reset|quarantine|hang|abandon|refus|fail' | head -\${2:-8} | sed "s/^/\$1 kernel /"; }
echo "V0 boot \$(ls /boot/boot0/log | tail -1)"
dmesg | grep -E 'vf2-vpu|wave5' | head -6 | sed 's/^/V0 /'
timeout 60 \$H/bin/vkvideo-probe --list 2>&1 | sed 's/^/V0 list /'
for s in \$( [ "$NO_V1" = 1 ] || ls \$H/clips/*.h264 ); do
	n=\$(basename \$s .h264)
	dmesg > \$H/k.before
	timeout 300 \$H/bin/vkvideo-probe --expect=\$H/clips/\$n.sha256 --time \$s > \$H/v1-\$n.txt 2>&1
	echo "V1 \$n exit=\$? \$(grep -c MISMATCH \$H/v1-\$n.txt) mismatches \$(grep -ciE 'not complete|did not complete' \$H/v1-\$n.txt) incomplete | \$(tail -2 \$H/v1-\$n.txt | tr '\n' ' ')"
	dmesg > \$H/k.after; kernel "V1 \$n" 4
done
for n in \$( [ "$NO_V1" = 1 ] || echo pb-high-352-pyramid h264-1080 ); do
	dmesg > \$H/k.before
	timeout 300 \$H/bin/vkvideo-probe --sampled --expect=\$H/clips/\$n.sha256 --time \$H/clips/\$n.h264 > \$H/v1s-\$n.txt 2>&1
	echo "V1 sampled \$n exit=\$? \$(grep -c MISMATCH \$H/v1s-\$n.txt) mismatches | \$(tail -2 \$H/v1s-\$n.txt | tr '\n' ' ')"
	dmesg > \$H/k.after; kernel "V1 sampled \$n" 4
done
dmesg > \$H/k.before
timeout 600 \$H/bin/media-probe --video-hash --time \$H/clips/h264-1080p30.mp4 > \$H/v2.hash 2> \$H/v2.err
echo "V2 media-probe exit=\$? \$(wc -l < \$H/v2.hash) hashes"
grep -E 'codec=|backend|elapsed_us|frames=|Vulkan Video|pictures|read back|GPU holds' \$H/v2.err | head -8 | sed 's/^/V2 /'
dmesg > \$H/k.after; kernel V2 8
dmesg > \$H/k.before
rm -f \$H/run/wayland-0 \$H/v3.png
\$H/bin/wayland --socket=\$H/run/wayland-0 "--desktop-client=\$H/bin/files --desktop" > \$H/v3-wayland.log 2>&1 < /dev/null &
wpid=\$!
sleep 15
WAYLAND_DISPLAY=wayland-0 \$H/bin/videoplayer --timeout-s=16 \$H/clips/h264-1080p30.mp4 > \$H/v3-player.log 2>&1 < /dev/null &
ppid=\$!
sleep 8
timeout 60 \$H/bin/keiland-shot --socket \$H/run/keiland-shot.sock \$H/v3.png > \$H/v3-shot.txt 2>&1
echo "V3 shot exit=\$? \$(tr '\n' ' ' < \$H/v3-shot.txt)"
top -b -n 1 2> /dev/null | head -12 | sed 's/^/V3 top /'
wait \$ppid
echo "V3 player exit=\$?"
grep -E 'VIDEOPLAYER (PRESENT|FRAMES|FAILED|DONE)|GPU holds|read back|Vulkan Video' \$H/v3-player.log | tail -8 | sed 's/^/V3 /'
kill \$wpid 2> /dev/null; sleep 3; kill -9 \$wpid 2> /dev/null; pkill -f "\$H/bin/files" 2> /dev/null
echo "V3 compositor \$(grep -cE 'KWL (FAILED|VULKAN_ERROR)' \$H/v3-wayland.log) errors"
dmesg > \$H/k.after; kernel V3 8
dmesg | grep -E 'wave5: video' | tail -4 | sed 's/^/V end /'
dmesg > \$H/dmesg-end.txt
BOARD
bcp "$out/board.sh" "$VF2_HOST:$H/check.sh" || say "FAIL the board's script was not copied"
printf '%s\n' "$VF2_PASSWORD" | BT=2400 b "sudo -S sh $H/check.sh" > "$out/board.txt" 2>&1
sed 's/^\[sudo\][^:]*: //' "$out/board.txt" | tee -a "$summary"
bcp "$VF2_HOST:$H/v2.hash" "$VF2_HOST:$H/v2.err" "$VF2_HOST:$H/v3.png" "$VF2_HOST:$H/v3-player.log" "$VF2_HOST:$H/dmesg-end.txt" "$out/" > /dev/null 2>&1
for f in v1-pb-high-352-pyramid v1-h264-1080 v1s-h264-1080; do bcp "$VF2_HOST:$H/$f.txt" "$out/" > /dev/null 2>&1; done
/usr/bin/python3 -I - "$out/v2.hash" "$clips/h264-1080p30.ref" <<'PY' | sed 's/^/V2 /' | tee -a "$summary"
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
first = next((i for i, (a, b) in enumerate(zip(got, want)) if a[1] != b[1]), -1)
verdict = "MATCH" if got and len(got) == len(want) and same == len(want) else "DIFFER"
print("%s %d of %d frames, %d hashes equal in order (first differing %d)" % (verdict, len(got), len(want), same, first))
PY
[ -f "$out/v3.png" ] && say "V3 png $(cksum < "$out/v3.png")"

# 3. The board as it was: the files removed, its own kernel back, checked by cksum.
bsudo "rm -rf $H; sync"
[ "$added" = 1 ] && bsudo "rm -rf $F; sync"
timeout 900 sh plan/tools/vf2/run.sh "$out/regress/board-vmunix" "$out/restore" > "$out/restore.txt" 2>&1
say "restore $(tail -2 "$out/restore.txt" | tr '\n' ' ')"
say "board after $(b "cksum /boot/boot0/vmunix /boot/boot0/vmunix.$old /etc/group /boot/boot0/zedbsd.cfg; ls /tmp $F; uname -sm" 2>&1 | tr '\n' ' ')"
say "own kernel $(cksum < "$out/regress/board-vmunix")"
say "end $(date +%H:%M)"
