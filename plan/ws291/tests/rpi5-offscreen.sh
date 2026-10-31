#!/bin/sh
# ws291-p021: mview on the Raspberry Pi 5 with no display: the viewer draws offscreen (--offscreen) into its own images,
# timed, and reads its last frame back, so the GPU's work and its picture are measured without a monitor or compositor.
#   [PI5=ADDRESS] [TFU=0|1] [COPIES='1 21 74'] [FRAMES=61] [SIZE=1920x1080] [MVIEW_ARGS=...] MVIEW=BUILD/bin/mview \
#       sh plan/ws291/tests/rpi5-offscreen.sh OUT
# MVIEW (a build of the board's config) is copied to /root/p021/bin/mview-off and the qs40 model (the arm64 image has
# no mview package data) to /root/p021/qs40; the image's libraries are used.  TFU=0 boots the board with v3d.tfu=0 added to cmdline.txt (the CPU converts textures), TFU=1 without it (the original
# kept once as cmdline.txt.p021orig); a change reboots the board.  The GPU is started when still held.  For each of
# COPIES the viewer draws FRAMES frames of its first view at SIZE (the first pays for first use and is not timed),
# and its last frame comes back as OUT/cN.ppm and OUT/cN.png; the summary keeps the viewer's rate, stages and
# checksum, and the kernel's last cost report and board line (v3d.perf=1).  Nothing is power-cycled.
# OUT/summary.txt holds what is printed.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
out=${1:?usage: rpi5-offscreen.sh OUT}
: "${MVIEW:?MVIEW names the viewer to copy}"
KEY=${KEY:-plan/tmp/guest/id_ed25519}
PI5=${PI5:-10.0.30.7}
SO="-i $KEY -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR -o ConnectTimeout=8 -o BatchMode=yes"
mkdir -p "$out"
summary=$out/summary.txt
: > "$summary"

say() {
	echo "$*" | tee -a "$summary"
}

b() {
	timeout "${BT:-60}" ssh $SO "root@$PI5" "$@"
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

say "start $(date +%H:%M)"
os=$(b 'uname -sm' 2>&1)
case $os in
zedBSD*) ;;
*) say "FAIL $PI5 is not zedBSD: $os"; exit 1 ;;
esac
say "board $PI5 $os"

# The texture conversion asked for, with a reboot when the boot parameters change.
if [ -n "${TFU:-}" ]; then
	b 'test -e /boot/boot0/cmdline.txt.p021orig || cp /boot/boot0/cmdline.txt /boot/boot0/cmdline.txt.p021orig'
	have=$(b 'grep -c "v3d.tfu=0" /boot/boot0/cmdline.txt' 2>/dev/null)
	want=0
	[ "$TFU" = 0 ] && want=1
	if [ "${have:-0}" != "$want" ]; then
		if [ "$want" = 1 ]; then
			b 'printf "%s v3d.tfu=0\n" "$(grep -v "^$" /boot/boot0/cmdline.txt.p021orig | sed "s/ v3d.tfu=0//g")" > /boot/boot0/cmdline.new && mv /boot/boot0/cmdline.new /boot/boot0/cmdline.txt && sync'
		else
			b 'cp /boot/boot0/cmdline.txt.p021orig /boot/boot0/cmdline.new && mv /boot/boot0/cmdline.new /boot/boot0/cmdline.txt && sync'
		fi
		b 'sync; (sleep 1; /sbin/reboot) > /dev/null 2>&1 &'
		sleep 20
		wait_up || { say "FAIL the board did not come back after the reboot"; exit 1; }
		say "rebooted $(date +%H:%M:%S)"
	fi
fi
say "kernel vmunix $(b 'cksum < /boot/boot0/vmunix' | cut -d' ' -f1) cmdline $(b 'cat /boot/boot0/cmdline.txt' 2>&1)"

# The GPU, started once per boot.
if b 'dmesg' | grep -q 'held; root starts it'; then
	if ! b 'dmesg' | grep -q 'V1 ok'; then
		BT=120 b 'sysctl hw.gpu.start=1' > "$out/start.txt" 2>&1
		say "started: $(tr '\n' ' ' < "$out/start.txt")"
	fi
fi
b 'dmesg' | grep -E 'v3d: V[0-9]|texture conversion' > "$out/stages.txt"
grep -E 'V2 version|clock|texture conversion' "$out/stages.txt" | head -6 | sed 's/^/gpu /' | tee -a "$summary"

# The viewer.
D=/root/p021
b "mkdir -p $D/bin"
timeout 300 scp $SO -q "$MVIEW" "root@$PI5:$D/bin/mview-off" || { say "FAIL the viewer was not copied"; exit 1; }
say "viewer $(b "cksum < $D/bin/mview-off" | cut -d' ' -f1)"

# The qs40 model, copied once.
if ! b "test -s $D/qs40/model.txt"; then
	b "mkdir -p $D/qs40/tex"
	timeout 600 scp $SO -q userland/tests/mview/models/qs40/tex/*.pam "root@$PI5:$D/qs40/tex/" &&
	    timeout 300 scp $SO -q userland/tests/mview/models/qs40/model.txt "root@$PI5:$D/qs40/model.txt" ||
	    { say "FAIL the model was not copied"; exit 1; }
fi
say "model $(b "cksum < $D/qs40/model.txt" | cut -d' ' -f1) textures $(b "ls $D/qs40/tex | wc -l" | tr -d ' ')"

for copies in ${COPIES:-1 21 74}; do
	b 'dmesg' > "$out/c$copies.before" 2>&1
	BT=600 b "$D/bin/mview-off --offscreen --size=${SIZE:-1920x1080} --model=$D/qs40 --token=c$copies --copies=$copies --frames=${FRAMES:-61} --shot=$D/c$copies.ppm ${MVIEW_ARGS:-}" > "$out/c$copies.txt" 2>&1
	say "c$copies $(grep -E 'MVIEW (OFFSCREEN|STAGES|SHOT|FAILED)' "$out/c$copies.txt" | tr '\n' ' ')"
	b 'dmesg' > "$out/c$copies.after" 2>&1
	grep -F -x -v -f "$out/c$copies.before" "$out/c$copies.after" > "$out/c$copies.kernel"
	grep -vE 'v3d: (perf|gpu|board)|display: perf' "$out/c$copies.kernel" | head -6 | sed "s/^/c$copies kernel /" | tee -a "$summary"
	grep -E 'v3d: perf' "$out/c$copies.kernel" | tail -1 | sed "s/^/c$copies perf /" | tee -a "$summary"
	grep -E 'v3d: board' "$out/c$copies.kernel" | tail -1 | sed "s/^/c$copies /" | tee -a "$summary"
	if timeout 300 scp $SO -q "root@$PI5:$D/c$copies.ppm" "$out/c$copies.ppm"; then
		python3 -I -c 'import sys; from PIL import Image; Image.open(sys.argv[1]).save(sys.argv[2])' "$out/c$copies.ppm" "$out/c$copies.png" 2>/dev/null
	fi
done
b "dmesg | grep -iE 'v3d.*(fault|lost|hang|reset|error)' | tail -8" > "$out/kernel.txt" 2>&1
sed 's/^/kernel /' "$out/kernel.txt" | tee -a "$summary"
say "end $(date +%H:%M)"
