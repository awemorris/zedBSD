#!/bin/sh
# ws291-p027: one tree's kernel and libvulkan on the VisionFive 2 (PowerVR), run from the top of the tree while the
# board is assigned:
#   VF2_HOST=kei@10.0.30.2 VF2_PASSWORD=... [VF2_PLUG=AC276E432AE6] [OLD=q1860old] sh plan/ws291/tests/vf2-reg.sh OUT BUILD
# BUILD is the tree's build of plan/ws291/tests/config-vf2-shot.mk.  First vf2-regress.sh (vkmatrix, -p 3, -r 3,
# vkcoherency, vkcompute and the desktop's PNG) with BUILD's kernel left on the board, then vf2-mview.sh (the game mode
# of qs40 x 1 and x 13) on the same boot, after which the board's own kernel (kept as vmunix.OLD) is put back, read
# back and the board power-cycled.  The programs run from VF2_DIR (/tmp/q1860 by default, a tmpfs: the board's root file system is
# nearly full and qs40's textures fill it) with LD_LIBRARY_PATH
# (BUILD's libraries); the board's system files are not changed.  OUT/summary.txt holds both summaries, the board's
# kernel, /etc/group and zedbsd.cfg checksums before and after, and the verdict lines.  The password comes from the
# environment, never from this file.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
out=${1:?usage: vf2-reg.sh OUT BUILD}
build=${2:?the build directory}
: "${VF2_HOST:?VF2_HOST=user@host}"
: "${VF2_PASSWORD:?VF2_PASSWORD}"
VF2_PLUG=${VF2_PLUG:-AC276E432AE6}
OLD=${OLD:-q1860old}
VF2_DIR=${VF2_DIR:-/tmp/q1860}
export VF2_HOST VF2_PASSWORD VF2_PLUG OLD VF2_DIR
mkdir -p "$out"
summary=$out/summary.txt
: > "$summary"

say() {
	echo "$*" | tee -a "$summary"
}

state() {
	SSHPASS="$VF2_PASSWORD" timeout 60 sshpass -e ssh -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null \
	    -o LogLevel=ERROR -o ConnectTimeout=8 "$VF2_HOST" 'cksum /boot/boot0/vmunix /etc/group /boot/boot0/zedbsd.cfg' 2>&1 | awk '{print $1, $3}' | tr '\n' ' '
}

say "before $(state)"
say "start $(date +%H:%M:%S) kernel $(cksum < "$build/vmunix" | cut -d' ' -f1) libvulkan $(cksum < "$build/dynamic/libvulkan.so" | cut -d' ' -f1)"
timeout 5400 env KEEP=1 sh plan/ws291/tests/vf2-regress.sh "$out/regress" "$build" > /dev/null 2>&1
sed 's/^/regress /' "$out/regress/summary.txt" | tee -a "$summary"
timeout 2400 env COPIES='1 13' SPIN=12 sh plan/ws291/tests/vf2-mview.sh "$out/mview" "$build" > /dev/null 2>&1
sed 's/^/mview /' "$out/mview/summary.txt" | tee -a "$summary"

# libvulkan's and the compositor's failure lines (a TLS mistake would end the program or fail here).
say "failures $(cat "$out"/regress/r-*.txt "$out"/regress/desk-wayland.log "$out"/mview/c*.txt "$out"/mview/wayland.log 2>/dev/null | grep -ciE 'device lost|handoff failed|VULKAN_ERROR|MVIEW FAILED|KWL FAILED|undefined symbol|relocation|Segmentation')"
say "kernel lines $(cat "$out"/mview/c*.kernel 2>/dev/null | grep -iE 'fault|lost|hang|reset|hwr|quarantine' | grep -v timing: | wc -l) fault/lost/hang/reset/HWR in mview's runs"
say "after $(state)"
say "end $(date +%H:%M:%S)"
sync
