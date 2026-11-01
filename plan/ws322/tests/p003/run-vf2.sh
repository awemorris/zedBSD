#!/bin/sh
# ws322-p003 on the VisionFive 2: the regression of a tree's kernel (plan/ws291/tests/vf2-regress.sh, which boots
# BUILD's kernel and keeps the board's own one as vmunix.OLD), then on the same boot the NV12 board check
# (nv12-board: plane views sampled, plane copies, export and import, no video without an engine) and vkimage,
# with the kernel lines they added; at the end the board's own kernel goes back (plan/tools/vf2/run.sh) and is
# checked by its cksum.  The programs run as root from ~/p3 with BUILD's libraries; ~/p3 is removed at the end.
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
old=${OLD:-p003orig}

say "start $(date +%H:%M) build $(cksum < "$build/vmunix")"
# 1. The regression; BUILD's kernel stays on the board for the check.
KEEP=1 OLD=$old timeout 7000 sh plan/ws291/tests/vf2-regress.sh "$out/regress" "$build" > "$out/regress.txt" 2>&1
say "regress exit=$?"
sed 's/^/regress: /' "$out/regress/summary.txt" >> "$summary"
grep -E "^(vkmatrix|vkcoherency|vkcompute|desk|kernel|board|kept|boot)" "$out/regress/summary.txt" | sed 's/^/  /'

# 2. The NV12 check and vkimage on the same boot.
b 'mkdir -p p3/bin p3/lib'
bcp "$build/bin/nv12-board" "$build/bin/vkimage" "$VF2_HOST:p3/bin/" && bcp "$build"/dynamic/*.so "$VF2_HOST:p3/lib/" || say "FAIL the files were not copied"
H=$(b 'echo $HOME')/p3
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
bcp "$out/board.sh" "$VF2_HOST:p3/check.sh" || say "FAIL the board's script was not copied"
printf '%s\n' "$VF2_PASSWORD" | BT=300 b "sudo -S sh $H/check.sh" > "$out/board.txt" 2>&1
sed 's/^\[sudo\][^:]*: //' "$out/board.txt" | tee -a "$summary"
b "rm -rf $H" 2> /dev/null

# 3. The board's own kernel back, checked by its cksum.
timeout 900 sh plan/tools/vf2/run.sh "$out/regress/board-vmunix" "$out/restore" > "$out/restore.txt" 2>&1
say "restore $(tail -2 "$out/restore.txt" | tr '\n' ' ')"
say "board $(b 'cksum /boot/boot0/vmunix /boot/boot0/vmunix.'"$old"'; uname -sm' 2>&1 | tr '\n' ' ')"
say "own kernel $(cksum < "$out/regress/board-vmunix")"
say "end $(date +%H:%M)"
