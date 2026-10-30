#!/bin/sh
# ws291-p020: mview's scene on the VisionFive 2's PowerVR with one tree's kernel and programs, run from the top of the
# tree while the board is assigned:
#   VF2_HOST=kei@10.0.30.2 VF2_PASSWORD=... VF2_PLUG=14C19FA80F86 [COPIES='1 10 50 100 200'] [SPIN=12]
#       [MVIEW_ARGS='--shading=pixel'] [TIMING=1] [KEEP=1] sh plan/ws291/tests/vf2-mview.sh OUT BUILD
# BUILD is the tree's build of plan/ws291/tests/config-vf2.mk (its vmunix, bin/wayland, bin/mview and the dynamic
# libraries).  The board's kernel is kept on the board as vmunix.p7old (when not kept already: the original) and read
# back from it (OUT/board-vmunix), BUILD's kernel
# put in and the board power-cycled (plan/tools/vf2/run.sh); the programs, the libraries and the qs40 model go to the
# user's ~/p7 (the board's system files are not changed: they run with LD_LIBRARY_PATH), the compositor is started on
# its own socket, mview spins in the game mode (mailbox, 4 images) once for each of COPIES, then the compositor is ended,
# the board's kernel put back (checked by reading it back) and the board power-cycled again.  The password comes from
# the environment, never from this file.  OUT/summary.txt holds what is printed.
# The compositor and mview run as root: the display node opens only for root or its owner.  MVIEW_ARGS are added to
# every mview run; TIMING=1 turns on libvulkan's submission timing for mview and keeps the kernel's timing lines (a
# kernel built with -DPOWERVR_SUBMIT_TIMING=1) of each run; KEEP=1 leaves BUILD's kernel on the board (no put back).
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
out=${1:?usage: vf2-mview.sh OUT BUILD}
build=${2:?the build directory}
: "${VF2_HOST:?VF2_HOST=user@host}"
: "${VF2_PASSWORD:?VF2_PASSWORD}"
: "${VF2_PLUG:?VF2_PLUG (the board's SwitchBot plug)}"
export SSHPASS="$VF2_PASSWORD" VF2_PLUG
SSHOPT="-o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR -o ConnectTimeout=8"
mkdir -p "$out"
summary=$out/summary.txt
: > "$summary"

say() {
	echo "$*" | tee -a "$summary"
}

b() {
	timeout "${BT:-60}" sshpass -e ssh $SSHOPT "$VF2_HOST" "$@"
}

bsudo() {
	printf '%s\n' "$VF2_PASSWORD" | b "sudo -S sh -c '$1'" 2> /dev/null
}

bcp() {
	timeout "${BT:-300}" sshpass -e scp $SSHOPT "$@"
}

say "start $(date +%H:%M)"
say "board $(b 'uname -sm; cksum /boot/boot0/vmunix' 2>&1 | tr '\n' ' ')"
bsudo 'test -f /boot/boot0/vmunix.p7old || cp -p /boot/boot0/vmunix /boot/boot0/vmunix.p7old; sync'
bcp "$VF2_HOST:/boot/boot0/vmunix.p7old" "$out/board-vmunix" || { say "FAIL the board's kernel was not read back"; exit 1; }
say "kept $(b 'cksum /boot/boot0/vmunix.p7old' 2>&1)"

# The tree's kernel, the board power-cycled until its render node is up.
timeout 900 sh plan/tools/vf2/run.sh "$build/vmunix" "$out/boot" > "$out/boot.txt" 2>&1
say "boot $(tail -3 "$out/boot.txt" | tr '\n' ' ')"
say "kernel $(cksum < "$build/vmunix" | cut -d' ' -f1) on the board: $(b 'uname -sm; cksum /boot/boot0/vmunix; sysctl hw.gpu.powervr.stages' 2>&1 | tr '\n' ' ')"

# The programs, the libraries and the model in the user's ~/p7.
b 'mkdir -p p7/bin p7/lib p7/qs40/tex p7/run && chmod 700 p7/run'
bcp "$build/bin/wayland" "$build/bin/mview" "$VF2_HOST:p7/bin/" &&
    bcp "$build"/dynamic/*.so "$VF2_HOST:p7/lib/" &&
    bcp userland/tests/mview/models/qs40/model.txt "$VF2_HOST:p7/qs40/" &&
    bcp userland/tests/mview/models/qs40/tex/*.pam "$VF2_HOST:p7/qs40/tex/" || say "FAIL the files were not copied"
say "files $(b 'cd p7 && cksum bin/wayland bin/mview lib/libc.so lib/libvulkan.so' 2>&1 | awk '{print $1, $3}' | tr '\n' ' ')"

# The compositor as root on its own socket (a stale socket of an earlier run removed first), its pid kept.
H=$(b 'echo $HOME')/p7
ENV="env LD_LIBRARY_PATH=$H/lib XDG_RUNTIME_DIR=$H/run"
printf '%s\n' "$VF2_PASSWORD" | b "sudo -S sh -c 'rm -f $H/run/wayland-0; $ENV nohup $H/bin/wayland --socket=$H/run/wayland-0 --desktop-client=none > $H/wayland.log 2>&1 < /dev/null & echo \$! > $H/wayland.pid'; sleep 8; test -S $H/run/wayland-0 && grep -q 'step=socket' $H/wayland.log && echo compositor up || { echo compositor down; tail -5 $H/wayland.log; }" > "$out/compositor.txt" 2>&1
say "$(grep -v '^\[sudo\]' "$out/compositor.txt" | tr '\n' ' ')"

timing=
[ "${TIMING:-0}" = 1 ] && timing='VULKAN_ZEDBSD_SUBMIT_TIMING=1'
for copies in ${COPIES:-1 10 50 100 200}; do
	b 'dmesg' > "$out/c$copies.before" 2>&1
	printf '%s\n' "$VF2_PASSWORD" | BT=$((${SPIN:-12} + 120)) b "sudo -S $ENV $timing $H/bin/mview --display=$H/run/wayland-0 --model=$H/qs40 --token=c$copies --spin=${SPIN:-12} --copies=$copies --content=game --present=mailbox --images=4 ${MVIEW_ARGS:-}" > "$out/c$copies.txt" 2>&1
	say "c$copies $(grep -E 'MVIEW (SPIN|STAGES|FAILED|HITCHES)' "$out/c$copies.txt" | sed 's/frames=[0-9]* seconds=[0-9.]* //' | tr '\n' ' ')"
	b 'dmesg' > "$out/c$copies.after" 2>&1
	grep -F -x -v -f "$out/c$copies.before" "$out/c$copies.after" | grep -v 'dw-mmc' > "$out/c$copies.kernel"
	grep -v 'timing:' "$out/c$copies.kernel" | head -6 | sed "s/^/c$copies kernel /" | tee -a "$summary"
	if [ "${TIMING:-0}" = 1 ]; then
		grep 'timing:' "$out/c$copies.kernel" > "$out/c$copies.ktiming"
		grep -E 'VULKAN TIMING|vulkan: timing|TIMING' "$out/c$copies.txt" > "$out/c$copies.utiming"
		say "c$copies timing kernel $(wc -l < "$out/c$copies.ktiming") lines, libvulkan $(wc -l < "$out/c$copies.utiming") lines"
	fi
done
b "grep -E 'KWL (SCANOUT|MODE [a-z]|FAILED|VULKAN_ERROR|DIRECT)' $H/wayland.log | sort | uniq -c | head -12" > "$out/scanout.txt" 2>&1
sed 's/^/compositor /' "$out/scanout.txt" | tee -a "$summary"
printf '%s\n' "$VF2_PASSWORD" | b "sudo -S sh -c 'kill \$(cat $H/wayland.pid)'; sleep 1; dmesg | grep -iE 'powervr|fault|lost|hang' | grep -v timing: | tail -8" > "$out/kernel.txt" 2>&1
grep -v '^\[sudo\]' "$out/kernel.txt" | sed 's/^/kernel /' | tee -a "$summary"

if [ "${KEEP:-0}" = 1 ]; then
	say "kept BUILD's kernel on the board (KEEP=1); board $(b 'cksum /boot/boot0/vmunix /boot/boot0/vmunix.p7old' 2>&1 | tr '\n' ' ')"
	say "end $(date +%H:%M)"
	exit 0
fi

# The board's own kernel again, read back and compared, and the board restarted on it.
timeout 900 sh plan/tools/vf2/run.sh "$out/board-vmunix" "$out/back" > "$out/back.txt" 2>&1
say "back $(tail -3 "$out/back.txt" | tr '\n' ' ')"
say "board now $(b 'uname -sm; cksum /boot/boot0/vmunix /boot/boot0/vmunix.p7old; grep -c login /boot/boot0/zedbsd.cfg' 2>&1 | tr '\n' ' ') expected $(cksum < "$out/board-vmunix" | cut -d' ' -f1)"
say "end $(date +%H:%M)"
