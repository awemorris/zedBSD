#!/bin/sh
# ws322-p003 on the VisionFive 2: the regression of a tree's kernel (plan/ws291/tests/vf2-regress.sh, which boots
# BUILD's kernel), then on the same boot the NV12 board check (nv12-board: plane views sampled, plane copies, export
# and import, no video without an engine) and vkimage, with the kernel lines they added; at the end the board's own
# kernel goes back (plan/tools/vf2/run.sh) and is checked by its cksum.  The board's root file system has little
# room, so everything placed goes to /tmp (tmpfs): the regression runs from a copy of vf2-regress.sh whose ~/p7 is
# /tmp/p3r, the check from /tmp/p3c; both are removed at the end.  OLD names the board's kept copy of its own
# kernel; the default, q1825orig, is already there, so nothing new is written to /boot but the kernels put in.
#   VF2_HOST=kei@10.0.30.2 VF2_PASSWORD=... VF2_PLUG=AC276E432AE6 sh plan/ws322/tests/p003/run-vf2.sh OUT BUILD
# BUILD is the tree's build of plan/ws291/tests/config-vf2-shot.mk with bin/nv12-board (build-board.sh BUILD).
# OUT/summary.txt holds what is printed.  The password comes from the environment, never from this file.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
out=${1:?usage: run-vf2.sh OUT BUILD}
build=${2:?the build directory}
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
bcp() { timeout "${BT:-300}" sshpass -e scp $SSHOPT "$@"; }
old=${OLD:-q1825orig}

say "start $(date +%H:%M) build $(cksum < "$build/vmunix")"
say "board before $(b "cksum /boot/boot0/vmunix /boot/boot0/vmunix.$old /etc/group /boot/boot0/zedbsd.cfg; df -k / /tmp" 2>&1 | tr '\n' ' ')"

# 1. The regression from /tmp; BUILD's kernel stays on the board for the check.
sed -e 's#^H=$(b .echo $HOME.)/p7$#H=/tmp/p3r#' -e 's#p7/#/tmp/p3r/#g' \
	-e "s#'mkdir -p p7/bin p7/lib p7/run && chmod 700 p7/run'#'mkdir -p /tmp/p3r/bin /tmp/p3r/lib /tmp/p3r/run \&\& chmod 700 /tmp/p3r/run'#" \
	plan/ws291/tests/vf2-regress.sh > "$out/vf2-regress-tmp.sh"
KEEP=1 OLD=$old timeout 7000 sh "$out/vf2-regress-tmp.sh" "$out/regress" "$build" > "$out/regress.txt" 2>&1
say "regress exit=$?"
sed 's/^/regress: /' "$out/regress/summary.txt" >> "$summary"

# 2. The NV12 check and vkimage on the same boot.
H=/tmp/p3c
b "mkdir -p $H/bin $H/lib"
bcp "$build/bin/nv12-board" "$build/bin/vkimage" "$VF2_HOST:$H/bin/" && bcp "$build"/dynamic/*.so "$VF2_HOST:$H/lib/" ||
	say "FAIL the files were not copied"
cat > "$out/board.sh" <<BOARD
H=$H
export LD_LIBRARY_PATH=\$H/lib
dmesg > \$H/before.txt
timeout 120 \$H/bin/nv12-board > \$H/nv12.txt 2>&1; echo "nv12-board exit=\$?"
cat \$H/nv12.txt
timeout 60 \$H/bin/vkimage > \$H/vkimage.txt 2>&1; echo "vkimage exit=\$?"
cat \$H/vkimage.txt
dmesg > \$H/after.txt
grep -F -x -v -f \$H/before.txt \$H/after.txt | grep -v dw-mmc | head -20 | sed 's/^/kernel: /'
sysctl hw.gpu.powervr.stages
BOARD
bcp "$out/board.sh" "$VF2_HOST:$H/check.sh" || say "FAIL the board's script was not copied"
printf '%s\n' "$VF2_PASSWORD" | BT=300 b "sudo -S sh $H/check.sh" > "$out/board.txt" 2>&1
sed 's/^\[sudo\][^:]*: //' "$out/board.txt" | tee -a "$summary"
printf '%s\n' "$VF2_PASSWORD" | b "sudo -S rm -rf $H /tmp/p3r" > /dev/null 2>&1

# 3. The board's own kernel back, checked by its cksum.
timeout 900 sh plan/tools/vf2/run.sh "$out/regress/board-vmunix" "$out/restore" > "$out/restore.txt" 2>&1
say "restore $(tail -2 "$out/restore.txt" | tr '\n' ' ')"
say "board after $(b "cksum /boot/boot0/vmunix /boot/boot0/vmunix.$old /etc/group /boot/boot0/zedbsd.cfg; ls /tmp; uname -sm" 2>&1 | tr '\n' ' ')"
say "own kernel $(cksum < "$out/regress/board-vmunix")"
say "end $(date +%H:%M)"
