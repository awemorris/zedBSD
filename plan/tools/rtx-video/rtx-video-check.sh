#!/bin/sh
# The video decoder's checks on the RTX 2070 machine (GPU_PERF_MACHINE=rtx, 10.0.10.3, zedBSD started once with UEFI
# BootNext), in one command: the kernel and nvrtx.video=1 put in, zedBSD restarted, then
#   V0  the driver's lines of the decoder's channel, the copy objects and the sessions' channels (dmesg)
#   V1  media-probe --video-hash --time on each clip of CLIPS (720p30 High, 1080p30 High, 1080p60 High, 1080p24 Main), the hashes
#       compared with the software decoder's (nv12-hashes.py on the host, CLIPS/NAME.ref): count, order, times
#   V2  the 1080p30 clip decoded and read back three times: frames, elapsed time, libmedia's own counts
#   V3  mview in a window (composed) for 10 s alone, then while the 1080p30 clip is decoded over and over: its frame
#       times both ways
#   V4  (when PERF_BUILD has bin/videoplayer) Video Player full screen on each clip for 12 s: whether libmedia gave it
#       pictures the GPU holds (sampled NV12 planes, nothing read back), libmedia's picture counts, errors, and a
#       desktop shot 6 s in (OUT/v4-NAME.png)
# then the line taken out again; zedBSD is left running.  The logs stay in OUT, a summary is printed
# (OUT/summary.txt).  The programs come from PERF_BUILD (default build/rtx-video): bin/media-probe,
# dynamic/libmedia.so and dynamic/libvulkan.so (used through LD_LIBRARY_PATH), usb/bin/mview.
# VIDEO_STEPS (default "v1 v2 v3 v4") picks the steps after V0; V4_OPTIONS (default --fullscreen) are the player's
# options (a window, say --width=1280 --height=720, is composed, so its shot shows the video; full screen may be shown
# without composing, and its shot is then empty); V4_CLIP (default every clip) names the one clip V4 plays.
#   usage: plan/tools/rtx-video/rtx-video-check.sh OUT VMUNIX [CLIPS]   (from the top of the tree)
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
OUT=${1:?usage: rtx-video-check.sh OUT VMUNIX [CLIPS]}
VMUNIX=${2:?usage: rtx-video-check.sh OUT VMUNIX [CLIPS]}
export GPU_PERF_MACHINE=${GPU_PERF_MACHINE:-rtx}
export PERF_BUILD=${PERF_BUILD:-build/rtx-video}
CLIPS=${3:-$PERF_BUILD/clips}
MEASURE="sh plan/tools/gpu-perf/measure.sh"
top=$(pwd)
. plan/tools/gpu-perf/conf.sh
KEY=${KEY:-$top/plan/tmp/guest/id_ed25519}
SSHOPT="-o BatchMode=yes -o ConnectTimeout=8 -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR"
mkdir -p "$OUT" "$top/build/tmp"
ZSSHOPT="$SSHOPT -o ControlMaster=auto -o ControlPath=$top/build/tmp/ssh-%h -o ControlPersist=600"
SUMMARY=$OUT/summary.txt
: > "$SUMMARY"
AS_USER='su $(awk -F: '"'"'$3 == 1000 { print $1; exit }'"'"' /etc/passwd) -c'
ENVS="env XDG_RUNTIME_DIR=/run/user/1000 WAYLAND_DISPLAY=wayland-0"
DIR=/root/ws243
PROBE="env LD_LIBRARY_PATH=$DIR $DIR/media-probe"
MVIEW_WINDOW="--spin=10 --windowed --present=fifo --copies=500"
KLINES="nvdec|copy object|video|work-channel ok|session channel|shares the work channel|executor fail|lost|fault|vram clear"
VIDEO_STEPS=${VIDEO_STEPS:-v1 v2 v3 v4}
V4_OPTIONS=${V4_OPTIONS:---fullscreen}
V4_CLIP=${V4_CLIP:-}

# wanted STEP: whether VIDEO_STEPS names the step.
wanted() {
	case " $VIDEO_STEPS " in
	*" $1 "*) return 0 ;;
	esac
	return 1
}

say() {
	echo "$(date +%H:%M:%S) $*" | tee -a "$SUMMARY"
}

# zs IP COMMAND: a command on zedBSD as root, at most ZS_TIMEOUT seconds.
zs() {
	zip=$1
	shift
	timeout "${ZS_TIMEOUT:-60}" ssh -i "$KEY" $ZSSHOPT "root@$zip" "$@"
}

# zput FILE... DEST: files copied to zedBSD.
zput() {
	timeout 300 scp -i "$KEY" $ZSSHOPT "$@" > /dev/null 2>&1
}

# run NAME LIMIT: the script on stdin run on zedBSD as root (at most LIMIT seconds), its output in OUT/NAME.log.
run() {
	rname=$1
	rlimit=$2
	ZS_TIMEOUT=$((rlimit + 30)) zs "$IP" "cat > /tmp/ws243-$rname.sh && timeout $rlimit sh /tmp/ws243-$rname.sh" > "$OUT/$rname.log" 2>&1
}

# compare HASHES REF: one line on how media-probe's hashes match the reference (count, order, times).
compare() {
	python3 -I - "$1" "$2" <<'EOF'
import sys
def read(path):
    rows = []
    for line in open(path):
        parts = line.split()
        if len(parts) == 2 and len(parts[1]) == 64:
            rows.append((int(parts[0]), parts[1]))
    return rows
got = read(sys.argv[1])
want = read(sys.argv[2])
same = sum(1 for a, b in zip(got, want) if a[1] == b[1])
first = next((i for i, (a, b) in enumerate(zip(got, want)) if a[1] != b[1]), -1)
times = sum(1 for a, b in zip(got, want) if a[0] == b[0])
verdict = "MATCH" if got and len(got) == len(want) and same == len(want) else "DIFFER"
print("%s %d of %d frames, %d hashes equal in order (first differing %d), %d times equal" % (verdict, len(got), len(want), same, first, times))
EOF
}

for program in "$PERF_BUILD/bin/media-probe" "$PERF_BUILD/dynamic/libmedia.so" "$PERF_BUILD/dynamic/libvulkan.so" "$PERF_BUILD/usb/bin/mview"; do
	[ -f "$program" ] || { say "FAIL no $program"; exit 1; }
done

# The kernel, the line and the programs in place, then zedBSD restarted.
IP=$($MEASURE zedbsd 2> "$OUT/zedbsd.log") || { say "FAIL zedBSD not up"; exit 1; }
zs "$IP" "uname -v; cksum < /boot/boot0/vmunix; grep -v '^#' /boot/boot0/zedbsd.cfg" > "$OUT/before.txt" 2>&1 < /dev/null
zput "$VMUNIX" "root@$IP:/tmp/vmunix.new" || { say "FAIL copy kernel"; exit 1; }
zs "$IP" "cp /boot/boot0/vmunix /boot/boot0/vmunix.old && cp /tmp/vmunix.new /boot/boot0/vmunix && sed -i -e '/^nvrtx\\.video=/d' /boot/boot0/zedbsd.cfg && echo nvrtx.video=1 >> /boot/boot0/zedbsd.cfg && mkdir -p $DIR && sync" < /dev/null || { say "FAIL kernel and line"; exit 1; }
zput "$PERF_BUILD/bin/media-probe" "$PERF_BUILD/dynamic/libmedia.so" "$PERF_BUILD/usb/bin/mview" "root@$IP:$DIR/" || { say "FAIL copy programs"; exit 1; }
zput "$PERF_BUILD/dynamic/libvulkan.so" "root@$IP:$DIR/libvulkan.so.1" || { say "FAIL copy libvulkan"; exit 1; }
zput "$CLIPS"/*.mp4 "root@$IP:$DIR/" || { say "FAIL copy clips"; exit 1; }
zs "$IP" "cp $DIR/libvulkan.so.1 $DIR/libvulkan.so && chmod 755 $DIR/media-probe $DIR/mview && sync" < /dev/null
$MEASURE linux > "$OUT/restart.log" 2>&1 || { say "FAIL Debian not up"; exit 1; }
IP=$($MEASURE zedbsd 2>> "$OUT/zedbsd.log") || { say "FAIL zedBSD not up after the restart"; exit 1; }
zs "$IP" "uname -v; cksum < /boot/boot0/vmunix; grep -E '^nvrtx\\.' /boot/boot0/zedbsd.cfg" > "$OUT/after.txt" 2>&1 < /dev/null
say "zedbsd $IP kernel $(sed -n 2p "$OUT/after.txt" | cut -d' ' -f1) (wanted $(cksum < "$VMUNIX" | cut -d' ' -f1)) | line $(grep -c '^nvrtx.video=1' "$OUT/after.txt")"

# V0: the driver's lines.
zs "$IP" "dmesg | grep -E 'nvrtx' | grep -E '$KLINES'" > "$OUT/v0.log" 2>&1 < /dev/null
say "V0 $(grep -E 'nvdec-channel (ok|fail)' "$OUT/v0.log" | tail -1 | cut -c1-160)"
say "V0 copy objects $(grep -c 'copy object on engine' "$OUT/v0.log"), refused $(grep -c 'copy object.*refused\|no copy object' "$OUT/v0.log")"

# V1: each clip's hashes.
for clip in "$CLIPS"/*.mp4; do
	wanted v1 || break
	name=$(basename "$clip" .mp4)
	run "v1-$name" 300 <<EOS
n=\$(dmesg | wc -l)
cd $DIR
$PROBE --video-hash --time $name.mp4 > /tmp/$name.hash 2> /tmp/$name.err
echo "media-probe rc \$?"
cat /tmp/$name.err
echo "--- hashes"
cat /tmp/$name.hash
echo "--- kernel"
dmesg | tail -n +\$n | grep -E 'nvrtx'
EOS
	sed -n '/^--- hashes/,/^--- kernel/p' "$OUT/v1-$name.log" > "$OUT/v1-$name.hash"
	say "V1 $name: $(grep -m1 'media-probe rc' "$OUT/v1-$name.log") | $(grep -m1 'codec=' "$OUT/v1-$name.log" | cut -c1-80) | $(compare "$OUT/v1-$name.hash" "$CLIPS/$name.ref")"
	say "    $(grep -m1 'elapsed_us' "$OUT/v1-$name.log") | $(grep -m1 'frames=' "$OUT/v1-$name.log") | $(grep -c 'nvrtx: vk: video' "$OUT/v1-$name.log") video kernel lines"
done

# V2: the 1080p30 readback's speed, three runs.
if wanted v2; then
run v2 300 <<EOS
cd $DIR
for i in 1 2 3; do
	$PROBE --video-hash --time h264-high-1080p30.mp4 2>&1 > /dev/null | grep -E 'elapsed_us|frames=|pictures in'
done
EOS
grep -E 'elapsed_us|frames=|pictures in' "$OUT/v2.log" | sed 's/^/    /' | tee -a "$SUMMARY" > /dev/null
python3 -I - "$OUT/v2.log" <<'EOF' | sed 's/^/V2 /' | tee -a "$SUMMARY"
import re, sys
text = open(sys.argv[1]).read()
elapsed = [int(x) for x in re.findall(r"elapsed_us=(\d+)", text)]
frames = [int(x) for x in re.findall(r"frames=(\d+) sound", text)]
for e, f in zip(elapsed, frames):
    print("%d frames in %.1f ms: %.1f fps" % (f, e / 1000.0, f * 1e6 / e if e else 0.0))
EOF
fi

# V3: mview's frame times in a window, alone and while the clip is decoded over and over.
if wanted v3; then
run v3 120 <<EOS
cd $DIR
$AS_USER "$ENVS $DIR/mview --token=alone $MVIEW_WINDOW" 2>&1 | grep -E 'MVIEW (SPIN|HITCHES|FAILED)'
( end=\$((\$(date +%s) + 14)); while [ \$(date +%s) -lt \$end ]; do $PROBE --video-hash h264-high-1080p30.mp4 > /dev/null 2>&1; done ) &
sleep 1
$AS_USER "$ENVS $DIR/mview --token=decoding $MVIEW_WINDOW" 2>&1 | grep -E 'MVIEW (SPIN|HITCHES|FAILED)'
wait
EOS
grep -E 'MVIEW (SPIN|HITCHES|FAILED)' "$OUT/v3.log" | cut -c1-200 | sed 's/^/V3 /' | tee -a "$SUMMARY"
fi

# V4: Video Player on the pictures the GPU holds, a shot while it plays.
if wanted v4 && [ -f "$PERF_BUILD/bin/videoplayer" ]; then
	zput "$PERF_BUILD/bin/videoplayer" "root@$IP:$DIR/" && zs "$IP" "chmod 755 $DIR/videoplayer" < /dev/null
	for clip in "$CLIPS"/*.mp4; do
		name=$(basename "$clip" .mp4)
		[ -z "$V4_CLIP" ] || [ "$V4_CLIP" = "$name" ] || continue
		run "v4-$name" 60 <<EOS
cd $DIR
n=\$(dmesg | wc -l)
$AS_USER "$ENVS LD_LIBRARY_PATH=$DIR $DIR/videoplayer $V4_OPTIONS --timeout-s=12 $DIR/$name.mp4" > /tmp/v4.err 2>&1 &
sleep 6
env XDG_RUNTIME_DIR=/run/user/1000 keiland-shot /tmp/v4-$name.png > /tmp/v4-shot.err 2>&1
echo "keiland-shot rc \$?"
cat /tmp/v4-shot.err
wait
echo "--- player"
grep -E 'VIDEOPLAYER' /tmp/v4.err
echo "--- kernel"
dmesg | tail -n +\$n | grep -E 'nvrtx'
EOS
		ZS_TIMEOUT=30 zs "$IP" "cat /tmp/v4-$name.png" > "$OUT/v4-$name.png" 2> /dev/null < /dev/null
		say "V4 $name: $(grep -c 'pictures the GPU holds' "$OUT/v4-$name.log") gpu-holds lines, $(grep -c 'read back' "$OUT/v4-$name.log") read-back lines, $(grep -ci 'fail\|error\|lost' "$OUT/v4-$name.log") failure lines, shot $(wc -c < "$OUT/v4-$name.png") bytes"
		{ grep -E 'PRESENT|GPU holds|read back|FAILED' "$OUT/v4-$name.log" | head -4; grep -E 'VIDEOPLAYER FRAMES' "$OUT/v4-$name.log" | tail -1;
		  grep -E 'VIDEOPLAYER DONE' "$OUT/v4-$name.log"; grep -E 'pictures in' "$OUT/v4-$name.log" | tail -2; } | cut -c1-160 | sed 's/^/    /' | tee -a "$SUMMARY" > /dev/null
	done
fi

# The line taken out again; zedBSD stays up.
zs "$IP" "sed -i -e '/^nvrtx\\.video=/d' /boot/boot0/zedbsd.cfg && sync && grep -c '^nvrtx.video' /boot/boot0/zedbsd.cfg" > "$OUT/line-back.txt" 2>&1 < /dev/null
say "line back: $(cat "$OUT/line-back.txt") video lines left"
say "done: $OUT"
