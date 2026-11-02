#!/bin/sh
# The video decoder's checks on the RTX 2070 machine (GPU_PERF_MACHINE=rtx, 10.0.10.3: Debian, zedBSD started once
# with UEFI BootNext), in one command, run from the top of the tree in the machine's lock (flock
# /tmp/zedbsd-rdna.lock):
#   1. zedBSD running; its NVMe kernel and boot line read back into OUT (nvme-vmunix, zedbsd.cfg) and their cksums kept
#   2. VMUNIX, nvrtx.video=1 (and EXTRA_CFG's words) put in, the programs and clips copied, zedBSD restarted
#   3. V0: the driver's lines (decoder channel, work channel, copy objects, faults, channel errors) by SSH (dmesg)
#   4. V1: media-probe --video-hash --time on each CLIPS/*.mp4, compared with CLIPS/NAME.ref (nv12-hashes.py)
#   4b. (with H265_DIR) H1: vkvideo-probe --h265 --time --expect=NAME.sha256 on each H265_DIR/NAME.h265 (the
#      references by ffmpeg's framehash, -pix_fmt nv12 sha256); with DXVA_PASS=1 again after a restart with
#      nvrtx.hevc.dxva=1 (the decoder parses the slice header's reference sets itself)
#      With RAW=1 each stream's frames are also written raw (vkvideo-probe --raw) and copied back to
#      OUT/raw-TAG-NAME.nv12 (h265-raw-compare.py compares them with ffmpeg's); H265_FRAMES=N decodes the first N
#      pictures only; H264=0 leaves out step 4; H265_NAMES="A B" takes those streams of H265_DIR only; H265_OPTIONS
#      are vkvideo-probe's own (default --copy: the frames read through a copy, as the decoder's pictures are block
#      linear)
#   4c. with VARIANTS="W1,W2 W3 ..." the H.265 streams again after a restart for each variant, its boot words (comma-
#      separated) in place of the variant before
#   4d. with PLAY_DIR (MP4 clips with their NAME.ref): media-probe --video-hash on each, compared with its reference,
#      then Video Player (PROGRAMS/bin/videoplayer, VP_OPTIONS, default a 1280x720 window) on each for 16 s in the
#      desktop's session: its VIDEOPLAYER lines (the GPU's pictures or the read back ones, frames shown and late) and
#      a shot of the desktop 8 s in (OUT/vp-NAME.png)
#   5. the desktop's session socket
#   6. the NVMe kernel and the boot line put back, checked by cksum against 1, and Debian running again
# OUT/summary.txt has what is printed; the logs stay in OUT.
#   usage: [H265_DIR=DIR [DXVA_PASS=1]] plan/ws324/tests/rtx-video324.sh OUT VMUNIX PROGRAMS CLIPS   (from the top
#          of the tree)
#   PROGRAMS is a build with bin/media-probe, dynamic/libmedia.so, dynamic/libvulkan.so and dynamic/libc.so (the
#   libraries, with dynamic/libwayland-client.so, are found through LD_LIBRARY_PATH, so a machine whose installed
#   userland is older runs them; the machine's /lib/ld.so is the one interpreter the kernel takes).
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
OUT=${1:?usage: rtx-video324.sh OUT VMUNIX PROGRAMS CLIPS}
VMUNIX=${2:?usage: rtx-video324.sh OUT VMUNIX PROGRAMS CLIPS}
PROGRAMS=${3:?usage: rtx-video324.sh OUT VMUNIX PROGRAMS CLIPS}
CLIPS=${4:?usage: rtx-video324.sh OUT VMUNIX PROGRAMS CLIPS}
export GPU_PERF_MACHINE=${GPU_PERF_MACHINE:-rtx}
MEASURE="sh plan/tools/gpu-perf/measure.sh"
top=$(pwd)
. plan/tools/gpu-perf/conf.sh
KEY=${KEY:-$top/plan/tmp/guest/id_ed25519}
SSHOPT="-o BatchMode=yes -o ConnectTimeout=8 -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR"
mkdir -p "$OUT" "$top/build/tmp"
ZSSHOPT="$SSHOPT -o ControlMaster=auto -o ControlPath=$top/build/tmp/ssh-p1v-%h -o ControlPersist=600"
SUMMARY=$OUT/summary.txt
: > "$SUMMARY"
DIR=/root/ws324
PROBE="env LD_LIBRARY_PATH=$DIR $DIR/media-probe"
KLINES="nvdec|copy object|video|work-channel ok|session channel|executor fail|lost|fault|channel error|held for the boot"

say() {
	echo "$(date +%H:%M:%S) $*" | tee -a "$SUMMARY"
}

# zs IP COMMAND: a command on zedBSD as root, at most ZS_TIMEOUT seconds.
zs() {
	zip=$1
	shift
	timeout "${ZS_TIMEOUT:-60}" ssh -i "$KEY" $ZSSHOPT "root@$zip" "$@"
}

# zcp SOURCE... DEST: files copied either way (scp's own form of the remote side).
zcp() {
	timeout 300 scp -O -i "$KEY" $ZSSHOPT "$@" > /dev/null 2>&1
}

deb() {
	timeout "${ZS_TIMEOUT:-60}" ssh $SSHOPT "$DEBIAN" "$@"
}

# compare HASHES REF: one line on how media-probe's hashes match the reference (count, order).
compare() {
	/usr/bin/python3 -I - "$1" "$2" <<'EOF'
import sys
def read(path):
    rows = []
    with open(path) as f:
        for line in f:
            parts = line.split()
            if len(parts) == 2 and parts[0].lstrip('-').isdigit() and len(parts[1]) == 64:
                rows.append((int(parts[0]), parts[1]))
    return rows
got = read(sys.argv[1])
want = read(sys.argv[2])
same = sum(1 for a, b in zip(got, want) if a[1] == b[1])
first = next((i for i, (a, b) in enumerate(zip(got, want)) if a[1] != b[1]), -1)
verdict = "MATCH" if got and len(got) == len(want) and same == len(want) else "DIFFER"
print("%s %d of %d frames, %d hashes equal in order (first differing %d)" % (verdict, len(got), len(want), same, first))
EOF
}

[ -f "$VMUNIX" ] || { say "FAIL no $VMUNIX"; exit 1; }
[ "$(stat -c %a "$KEY")" = 600 ] || { say "FAIL the key $KEY is not mode 600"; exit 1; }
for program in "$PROGRAMS/bin/media-probe" "$PROGRAMS/dynamic/libmedia.so" "$PROGRAMS/dynamic/libvulkan.so" "$PROGRAMS/dynamic/libc.so" "$PROGRAMS/dynamic/libwayland-client.so"; do
	[ -f "$program" ] || { say "FAIL no $program"; exit 1; }
done

# h265_pass TAG: each H.265 stream of H265_DIR through vkvideo-probe, its result and the kernel's lines.
h265_pass() {
	tag=$1
	[ -n "${H265_DIR:-}" ] || return 0
	for name in $(h265_names); do
		extra=${H265_OPTIONS---copy}
		[ -n "${H265_FRAMES:-}" ] && extra="$extra --frames=$H265_FRAMES"
		[ "${RAW:-0}" = 1 ] && extra="$extra --raw=/tmp/$name.nv12"
		ZS_TIMEOUT=150 zs "$IP" "n=\$(dmesg | wc -l); cd $DIR && timeout 120 env LD_LIBRARY_PATH=$DIR $DIR/vkvideo-probe --h265 --time $extra --expect=$name.sha256 $name.h265 > /tmp/$name.out 2>&1; echo \"vkvideo-probe rc \$?\"; tail -n 12 /tmp/$name.out; echo '--- kernel'; dmesg | tail -n +\$n | grep -E 'nvrtx'" > "$OUT/h1-$tag-$name.log" 2>&1 < /dev/null
		zs "$IP" "cat /tmp/$name.out" > "$OUT/h1-$tag-$name.full" 2>&1 < /dev/null
		if [ "${RAW:-0}" = 1 ]; then
			zcp "root@$IP:/tmp/$name.nv12" "$OUT/raw-$tag-$name.nv12" || say "FAIL copy back the raw frames of $name"
			zs "$IP" "rm -f /tmp/$name.nv12" < /dev/null
		fi
		say "H1 $tag $name: $(grep -m1 'vkvideo-probe rc' "$OUT/h1-$tag-$name.log") | $(grep -ciE 'mismatch|differ' "$OUT/h1-$tag-$name.log") mismatch lines | $(grep -c 'nvrtx: vk: video' "$OUT/h1-$tag-$name.log") video kernel lines"
		grep -E 'mismatch|differ|frames|decodes|refused|skipped|did not finish|held for the boot|nvdec' "$OUT/h1-$tag-$name.log" | head -4 | cut -c1-150 | sed 's/^/    /' | tee -a "$SUMMARY"
	done
}

# h265_names: the names of the H.265 streams to run (H265_NAMES, else every stream of H265_DIR).
h265_names() {
	if [ -n "${H265_NAMES:-}" ]; then
		echo "$H265_NAMES"
		return
	fi
	for stream in "$H265_DIR"/*.h265; do
		basename "$stream" .h265
	done
}

# 1. zedBSD running, its kernel and boot line kept.
IP=$(timeout 900 $MEASURE zedbsd 2> "$OUT/zedbsd.log") || { say "FAIL zedBSD not up"; exit 1; }
zs "$IP" "uname -v; cksum < /boot/boot0/vmunix; cksum < /boot/boot0/zedbsd.cfg; grep -v '^#' /boot/boot0/zedbsd.cfg" > "$OUT/before.txt" 2>&1 < /dev/null
kernel_before=$(sed -n 2p "$OUT/before.txt" | cut -d' ' -f1)
cfg_before=$(sed -n 3p "$OUT/before.txt" | cut -d' ' -f1)
zcp "root@$IP:/boot/boot0/vmunix" "$OUT/nvme-vmunix" || { say "FAIL read back the NVMe kernel"; exit 1; }
zcp "root@$IP:/boot/boot0/zedbsd.cfg" "$OUT/zedbsd.cfg" || { say "FAIL read back the boot line"; exit 1; }
sync
kept_kernel=$(cksum < "$OUT/nvme-vmunix" | cut -d' ' -f1)
kept_cfg=$(cksum < "$OUT/zedbsd.cfg" | cut -d' ' -f1)
say "before: zedbsd $IP kernel $kernel_before (kept $kept_kernel) cfg $cfg_before (kept $kept_cfg)"
[ -n "$kernel_before" ] && [ "$kernel_before" = "$kept_kernel" ] && [ "$cfg_before" = "$kept_cfg" ] || { say "FAIL the kept copies differ; nothing changed"; exit 1; }

# 2. The kernel, the line, the programs and the clips in, zedBSD restarted.
zcp "$VMUNIX" "root@$IP:/tmp/vmunix.new" || { say "FAIL copy kernel"; exit 1; }
zs "$IP" "cp /tmp/vmunix.new /boot/boot0/vmunix && sed -i -e '/^nvrtx\\.video=/d' /boot/boot0/zedbsd.cfg && echo nvrtx.video=1 >> /boot/boot0/zedbsd.cfg && mkdir -p $DIR && sync" < /dev/null || say "FAIL kernel and line"
for w in ${EXTRA_CFG:-}; do
	zs "$IP" "echo $w >> /boot/boot0/zedbsd.cfg && sync" < /dev/null || say "FAIL extra line $w"
done
zcp "$PROGRAMS/bin/media-probe" "$PROGRAMS/dynamic/libmedia.so" "$PROGRAMS/dynamic/libc.so" "$PROGRAMS/dynamic/libwayland-client.so" "root@$IP:$DIR/" || say "FAIL copy programs"
if [ -n "${PLAY_DIR:-}" ]; then
	zcp "$PROGRAMS/bin/videoplayer" "$PROGRAMS"/dynamic/*.so "$PLAY_DIR"/*.mp4 "root@$IP:$DIR/" || say "FAIL copy the player, its libraries and the clips"
fi
if [ -n "${H265_DIR:-}" ]; then
	zcp "$PROGRAMS/bin/vkvideo-probe" "$H265_DIR"/*.h265 "$H265_DIR"/*.sha256 "root@$IP:$DIR/" || say "FAIL copy the H.265 probe and streams"
fi
zcp "$PROGRAMS/dynamic/libvulkan.so" "root@$IP:$DIR/libvulkan.so.1" || say "FAIL copy libvulkan"
zcp "$CLIPS"/*.mp4 "root@$IP:$DIR/" || say "FAIL copy clips"
zs "$IP" "cp $DIR/libvulkan.so.1 $DIR/libvulkan.so && chmod 755 $DIR/media-probe && (test ! -f $DIR/vkvideo-probe || chmod 755 $DIR/vkvideo-probe) && sync" < /dev/null
timeout 900 $MEASURE linux > "$OUT/restart.log" 2>&1 || say "FAIL Debian not up"
IP=$(timeout 900 $MEASURE zedbsd 2>> "$OUT/zedbsd.log")
if [ -n "$IP" ]; then
	zs "$IP" "uname -v; cksum < /boot/boot0/vmunix; grep -E '^nvrtx\\.' /boot/boot0/zedbsd.cfg" > "$OUT/after.txt" 2>&1 < /dev/null
	say "under test: zedbsd $IP kernel $(sed -n 2p "$OUT/after.txt" | cut -d' ' -f1) (wanted $(cksum < "$VMUNIX" | cut -d' ' -f1)) | video line $(grep -c '^nvrtx.video=1' "$OUT/after.txt")"

	# 3. V0: the driver's lines.
	zs "$IP" "dmesg | grep 'nvrtx'" > "$OUT/nvrtx.log" 2>&1 < /dev/null
	grep -E "$KLINES" "$OUT/nvrtx.log" > "$OUT/v0.log"
	say "V0 $(grep -E 'nvdec-channel (ok|fail)' "$OUT/nvrtx.log" | tail -1 | cut -c1-140) | $(grep -c 'work-channel ok' "$OUT/nvrtx.log") work-channel ok | copy objects $(grep -c 'copy object on engine' "$OUT/nvrtx.log")"

	# 4. V1: each clip's hashes (unless H264=0).
	[ "${H264:-1}" = 1 ] || CLIPS_RUN=
	for clip in ${CLIPS_RUN-"$CLIPS"/*.mp4}; do
		name=$(basename "$clip" .mp4)
		ZS_TIMEOUT=330 zs "$IP" "n=\$(dmesg | wc -l); cd $DIR && timeout 300 $PROBE --video-hash --time $name.mp4 > /tmp/$name.hash 2> /tmp/$name.err; echo \"media-probe rc \$?\"; cat /tmp/$name.err; echo '--- hashes'; cat /tmp/$name.hash; echo '--- kernel'; dmesg | tail -n +\$n | grep -E 'nvrtx'" > "$OUT/v1-$name.log" 2>&1 < /dev/null
		sed -n '/^--- hashes/,/^--- kernel/p' "$OUT/v1-$name.log" > "$OUT/v1-$name.hash"
		say "V1 $name: $(grep -m1 'media-probe rc' "$OUT/v1-$name.log") | $(grep -m1 'codec=' "$OUT/v1-$name.log" | cut -c1-70) | $(compare "$OUT/v1-$name.hash" "$CLIPS/$name.ref")"
		say "    $(grep -m1 'elapsed_us' "$OUT/v1-$name.log" | cut -c1-80) | $(grep -c 'nvrtx: vk: video' "$OUT/v1-$name.log") video kernel lines"
	done

	# 4b. H1: the H.265 streams, then (DXVA_PASS=1) again with the decoder parsing the reference sets itself.
	h265_pass skip
	if [ "${DXVA_PASS:-0}" = 1 ] && [ -n "${H265_DIR:-}" ]; then
		zs "$IP" "dmesg | grep 'nvrtx'" > "$OUT/nvrtx-skip.log" 2>&1 < /dev/null
		zs "$IP" "echo nvrtx.hevc.dxva=1 >> /boot/boot0/zedbsd.cfg && sync" < /dev/null || say "FAIL the dxva line"
		timeout 900 $MEASURE linux >> "$OUT/restart.log" 2>&1 || say "FAIL Debian not up"
		IP=$(timeout 900 $MEASURE zedbsd 2>> "$OUT/zedbsd.log")
		if [ -n "$IP" ]; then
			say "dxva: zedbsd $IP $(zs "$IP" "grep -c '^nvrtx.hevc.dxva=1' /boot/boot0/zedbsd.cfg" < /dev/null) dxva line"
			h265_pass dxva
		else
			say "FAIL zedBSD not up with the dxva line"
		fi
	fi

	# 4c. (with VARIANTS) H1 again after a restart for each variant: its boot words (comma-separated) in place of the
	#     last variant's.
	last=
	for variant in ${VARIANTS:-}; do
		[ -n "${H265_DIR:-}" ] || break
		words=$(echo "$variant" | tr ',' ' ')
		for w in $last; do
			zs "$IP" "sed -i -e '/^$(echo "$w" | sed 's/\./\\./g')\$/d' /boot/boot0/zedbsd.cfg && sync" < /dev/null
		done
		for w in $words; do
			zs "$IP" "echo $w >> /boot/boot0/zedbsd.cfg && sync" < /dev/null || say "FAIL the line $w"
		done
		last=$words
		timeout 900 $MEASURE linux >> "$OUT/restart.log" 2>&1 || say "FAIL Debian not up"
		IP=$(timeout 900 $MEASURE zedbsd 2>> "$OUT/zedbsd.log")
		if [ -n "$IP" ]; then
			say "variant $variant: zedbsd $IP, lines: $(zs "$IP" "grep -E '^nvrtx\\.' /boot/boot0/zedbsd.cfg | tr '\\n' ' '" < /dev/null)"
			h265_pass "$(echo "$variant" | tr ',=.' '+__')"
		else
			say "FAIL zedBSD not up with the variant $variant"
			break
		fi
	done

	# 4d. (with PLAY_DIR) libmedia's hashes and Video Player on each clip.
	for clip in ${PLAY_DIR:+"$PLAY_DIR"/*.mp4}; do
		name=$(basename "$clip" .mp4)
		ZS_TIMEOUT=330 zs "$IP" "cd $DIR && timeout 300 $PROBE --video-hash --time $name.mp4 > /tmp/$name.hash 2> /tmp/$name.err; echo \"media-probe rc \$?\"; cat /tmp/$name.err; echo '--- hashes'; cat /tmp/$name.hash" > "$OUT/p1-$name.log" 2>&1 < /dev/null
		sed -n '/^--- hashes/,$p' "$OUT/p1-$name.log" > "$OUT/p1-$name.hash"
		say "P1 $name: $(grep -m1 'media-probe rc' "$OUT/p1-$name.log") | $(grep -m1 'codec=' "$OUT/p1-$name.log" | cut -c1-70) | $(compare "$OUT/p1-$name.hash" "$PLAY_DIR/$name.ref")"
		ZS_TIMEOUT=90 zs "$IP" "chmod 755 $DIR/videoplayer; n=\$(dmesg | wc -l); u=\$(awk -F: '\$3 == 1000 { print \$1; exit }' /etc/passwd); su \$u -c 'env XDG_RUNTIME_DIR=/run/user/1000 WAYLAND_DISPLAY=wayland-0 LD_LIBRARY_PATH=$DIR $DIR/videoplayer ${VP_OPTIONS:---width=1280 --height=720} --timeout-s=16 $DIR/$name.mp4' > /tmp/vp-$name.err 2>&1 & sleep 8; env XDG_RUNTIME_DIR=/run/user/1000 keiland-shot /tmp/vp-$name.png > /tmp/vp-shot.err 2>&1; echo \"keiland-shot rc \$?\"; cat /tmp/vp-shot.err; wait; echo '--- player'; grep -E 'VIDEOPLAYER|GPU holds|read back' /tmp/vp-$name.err | tail -12; echo '--- kernel'; dmesg | tail -n +\$n | grep -E 'nvrtx'" > "$OUT/vp-$name.log" 2>&1 < /dev/null
		ZS_TIMEOUT=30 zs "$IP" "cat /tmp/vp-$name.png" > "$OUT/vp-$name.png" 2> /dev/null < /dev/null
		say "VP $name: $(grep -c 'GPU holds' "$OUT/vp-$name.log") gpu-holds lines, $(grep -c 'read back' "$OUT/vp-$name.log") read-back lines, $(grep -ciE 'fail|error|lost' "$OUT/vp-$name.log") failure lines, shot $(wc -c < "$OUT/vp-$name.png") bytes"
		grep -E 'PRESENT|FRAMES|DONE|FAILED' "$OUT/vp-$name.log" | tail -4 | cut -c1-200 | sed 's/^/    /' | tee -a "$SUMMARY"
	done

	# 5. The desktop's session, the faults and the channel errors of the whole run.
	zs "$IP" "dmesg | grep 'nvrtx'" > "$OUT/nvrtx-end.log" 2>&1 < /dev/null
	zs "$IP" "test -S /run/user/1000/wayland-0 && echo session-socket" > "$OUT/session.txt" 2>&1 < /dev/null
	say "channel errors $(grep -c 'channel-stop channel error' "$OUT/nvrtx-end.log"), faults outside the boot's record of an earlier fault $(grep -i 'fault' "$OUT/nvrtx-end.log" | grep -vc 'before the tables'), lost $(grep -c 'lost' "$OUT/nvrtx-end.log"), skipped pictures $(grep -c 'a picture is skipped' "$OUT/nvrtx-end.log"), session $(grep -c session-socket "$OUT/session.txt")"
	grep -E 'channel-stop channel error|fault|skipped|held for the boot' "$OUT/nvrtx-end.log" | grep -v 'before the tables' | head -6 | cut -c1-160 | sed 's/^/    /' | tee -a "$SUMMARY"
else
	say "FAIL zedBSD not up with the kernel under test"
fi

# 6. The machine given back: the kept kernel and line in, checked, then Debian.
IP=$(timeout 900 $MEASURE zedbsd 2>> "$OUT/zedbsd.log") || { say "FAIL zedBSD not up for the restore (the NVMe kernel is the one under test)"; exit 1; }
zcp "$OUT/nvme-vmunix" "root@$IP:/tmp/vmunix.back" || say "FAIL copy the kept kernel"
zcp "$OUT/zedbsd.cfg" "root@$IP:/tmp/zedbsd.cfg.back" || say "FAIL copy the kept boot line"
zs "$IP" "cp /tmp/vmunix.back /boot/boot0/vmunix && cp /tmp/zedbsd.cfg.back /boot/boot0/zedbsd.cfg && sync && cksum < /boot/boot0/vmunix && cksum < /boot/boot0/zedbsd.cfg" > "$OUT/restore.txt" 2>&1 < /dev/null
kernel_back=$(sed -n 1p "$OUT/restore.txt" | cut -d' ' -f1)
cfg_back=$(sed -n 2p "$OUT/restore.txt" | cut -d' ' -f1)
verdict=FAIL
[ "$kernel_back" = "$kernel_before" ] && [ "$cfg_back" = "$cfg_before" ] && verdict=MATCH
say "restore $verdict: kernel $kernel_back (before $kernel_before), cfg $cfg_back (before $cfg_before)"
timeout 900 $MEASURE linux >> "$OUT/restart.log" 2>&1 || say "FAIL Debian not up after the restore"
say "Debian: $(deb "uname -r; lspci -nnk -d 10de: | grep 'driver in use' | sort | uniq -c | tr -s ' '" 2> /dev/null | tr '\n' ' ')"
say "where: $(timeout 120 $MEASURE where 2> /dev/null)"
sync
say "done: $OUT"
