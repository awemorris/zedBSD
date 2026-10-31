#!/bin/sh
# ws309-p015: the game mode's lent images on the VisionFive 2's PowerVR, each arm a build's kernel and programs, run
# from the top of the tree while the board is assigned:
#   VF2_HOST=kei@10.0.30.2 VF2_PASSWORD=... VF2_PLUG=14C19FA80F86 [COPIES='1 13'] [SPIN=12] [CYCLES=8] [OLD=p2old]
#       sh plan/ws309/tests/vf2-p015.sh OUT NAME:BUILD ...
# Each BUILD is a build of plan/ws291/tests/config-vf2.mk with -DPOWERVR_SUBMIT_TIMING=1 (its vmunix, bin/wayland,
# bin/mview and the dynamic libraries).  The board's kernel is kept on the board as vmunix.OLD (once: a kept one is not
# overwritten) and read back from it (OUT/board-vmunix).  For each arm its kernel is put in and the board power-cycled
# (plan/tools/vf2/run.sh); the programs, the libraries and the qs40 model go to the user's ~/p15 (the board's system
# files are not changed: they run with LD_LIBRARY_PATH); then for each of COPIES a compositor of its own (root, its own
# socket, no desktop client) and one mview spin in the game mode (mailbox, 4 images) with libvulkan's submission timing
# and chain log (VULKAN_ZEDBSD_WSI_LOG: the chain's lend, its images' layout, its presents and the copies among them),
# the kernel's timing lines of the run counted by kind (transfer: the GPU copies); and the game <-> window cycle (BUG-375):
# CYCLES short game-mode runs on one compositor, its mode lines counted and it checked alive.  At the end the board's
# kernel is put back, read back and compared, and the board restarted on it.  The password comes from the environment,
# never from this file.  OUT/summary.txt holds what is printed.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
out=${1:?usage: vf2-p015.sh OUT NAME:BUILD ...}
shift
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

# compositor_start DIR: a compositor as root on its own socket, its pid kept.
compositor_start() {
	printf '%s\n' "$VF2_PASSWORD" | b "sudo -S sh -c 'rm -f $H/run/wayland-0; $ENV nohup $H/bin/wayland --socket=$H/run/wayland-0 --desktop-client=none > $H/wayland.log 2>&1 < /dev/null & echo \$! > $H/wayland.pid'; sleep 8; test -S $H/run/wayland-0 && grep -q 'step=socket' $H/wayland.log && echo compositor up || { echo compositor down; tail -5 $H/wayland.log; }" > "$1" 2>&1
	grep -v '^\[sudo\]' "$1" | tr '\n' ' '
}

# compositor_stop NAME: the compositor ended, its log read back as OUT/ARM/NAME.log.
compositor_stop() {
	bcp "$VF2_HOST:p15/wayland.log" "$dir/$1.log" > /dev/null 2>&1 || say "$arm FAIL the compositor's log was not read back"
	printf '%s\n' "$VF2_PASSWORD" | b "sudo -S sh -c 'kill \$(cat $H/wayland.pid)'; sleep 2" > /dev/null 2>&1
}

# mview_run NAME COPIES SPIN: one game-mode spin as root.
mview_run() {
	printf '%s\n' "$VF2_PASSWORD" | BT=$(($3 + 120)) b "sudo -S sh -c '$ENV VULKAN_ZEDBSD_SUBMIT_TIMING=1 VULKAN_ZEDBSD_WSI_LOG=1 $H/bin/mview --display=$H/run/wayland-0 --model=$H/qs40 --token=$1 --spin=$3 --copies=$2 --content=game --present=mailbox --images=4'" > "$dir/$1.txt" 2>&1
}

say "start $(date +%H:%M)"
say "board $(b 'uname -sm; cksum /boot/boot0/vmunix' 2>&1 | tr '\n' ' ')"
old=/boot/boot0/vmunix.${OLD:-p2old}
bsudo "test -f $old || cp -p /boot/boot0/vmunix $old; sync"
bcp "$VF2_HOST:$old" "$out/board-vmunix" || { say "FAIL the board's kernel was not read back"; exit 1; }
say "kept $(b "cksum $old" 2>&1)"

for spec in "$@"; do
	arm=${spec%%:*}
	build=${spec#*:}
	dir=$out/$arm
	mkdir -p "$dir"
	timeout 900 sh plan/tools/vf2/run.sh "$build/vmunix" "$dir/boot" > "$dir/boot.txt" 2>&1
	say "$arm boot $(tail -2 "$dir/boot.txt" | tr '\n' ' ')"
	say "$arm kernel $(cksum < "$build/vmunix" | cut -d' ' -f1) on the board: $(b 'cksum /boot/boot0/vmunix; sysctl hw.gpu.powervr.stages' 2>&1 | tr '\n' ' ')"

	# The programs, the libraries and the model in the user's ~/p15.
	b 'mkdir -p p15/bin p15/lib p15/qs40/tex p15/run && chmod 700 p15/run'
	bcp "$build/bin/wayland" "$build/bin/mview" "$VF2_HOST:p15/bin/" &&
	    bcp "$build"/dynamic/*.so "$VF2_HOST:p15/lib/" &&
	    bcp userland/tests/mview/models/qs40/model.txt "$VF2_HOST:p15/qs40/" &&
	    bcp userland/tests/mview/models/qs40/tex/*.pam "$VF2_HOST:p15/qs40/tex/" || say "$arm FAIL the files were not copied"
	H=$(b 'echo $HOME')/p15
	ENV="env LD_LIBRARY_PATH=$H/lib XDG_RUNTIME_DIR=$H/run"
	say "$arm files $(b 'cd p15 && cksum bin/wayland bin/mview lib/libvulkan.so' 2>&1 | awk '{print $1, $3}' | tr '\n' ' ')"

	# One compositor for each spin.
	for copies in ${COPIES:-1 13}; do
		say "$arm c$copies $(compositor_start "$dir/c$copies.compositor")"
		b 'dmesg' > "$dir/c$copies.before" 2>&1
		mview_run "c$copies" "$copies" "${SPIN:-12}"
		b 'dmesg' > "$dir/c$copies.after" 2>&1
		grep -F -x -v -f "$dir/c$copies.before" "$dir/c$copies.after" | grep -v 'dw-mmc' > "$dir/c$copies.kernel"
		compositor_stop "c$copies"
		say "$arm c$copies $(grep -E 'MVIEW (SPIN|FAILED)' "$dir/c$copies.txt" | sed 's/frames=[0-9]* seconds=[0-9.]* //' | tr '\n' ' ')"
		grep -E 'VULKAN WSI' "$dir/c$copies.txt" | sed "s/^/$arm c$copies /" | tee -a "$summary"
		say "$arm c$copies kernel timing (kind: sum of counts) $(sed -n 's/.*powervr: timing: \([^:]*\): \([0-9]*\), average.*/\1 \2/p' "$dir/c$copies.kernel" | awk '{n[$1] += $2} END {for (k in n) printf "%s: %d; ", k, n[k]}')"
		grep -v 'timing:' "$dir/c$copies.kernel" | head -4 | sed "s/^/$arm c$copies kernel /" | tee -a "$summary"
		grep -E 'KWL SCANOUT (direct|lend)|lendable' "$dir/c$copies.log" | tail -4 | sed "s/^/$arm c$copies /" | tee -a "$summary"
	done

	# The game <-> window cycle on one compositor.
	say "$arm cycle $(compositor_start "$dir/cycle.compositor")"
	i=0
	while [ $i -lt "${CYCLES:-8}" ]; do
		i=$((i + 1))
		mview_run "cy$i" 10 4
		grep -E 'MVIEW (DONE|FAILED)' "$dir/cy$i.txt" | tail -1 | sed "s/^/$arm cycle $i /" >> "$summary"
		sleep 3
	done
	# The compositor runs as root, which the user's ps does not list.
	alive=$(printf '%s\n' "$VF2_PASSWORD" | b "sudo -S sh -c 'kill -0 \$(cat $H/wayland.pid) && echo 1 || echo 0'" 2> /dev/null | tail -1)
	compositor_stop cycle
	say "$arm cycle compositor alive $alive direct=$(grep -c 'KWL SCANOUT direct=1' "$dir/cycle.log") left=$(grep -c 'KWL SCANOUT direct=0 reason=no-window' "$dir/cycle.log") window=$(grep -c 'KWL MODE window' "$dir/cycle.log") refused=$(grep -c 'lend refused' "$dir/cycle.log") failed=$(grep -cE 'KWL (FAILED|MODE_ERROR|VULKAN_ERROR)' "$dir/cycle.log") done=$(grep -c "^$arm cycle [0-9]* MVIEW DONE" "$summary")"
	b "dmesg | grep -iE 'powervr|fault|lost|hang|EBUSY' | grep -v timing: | tail -4" 2>&1 | sed "s/^/$arm kernel /" | tee -a "$summary"
done

# The board's own kernel again, read back and compared, and the board restarted on it.
timeout 900 sh plan/tools/vf2/run.sh "$out/board-vmunix" "$out/back" > "$out/back.txt" 2>&1
say "back $(tail -2 "$out/back.txt" | tr '\n' ' ')"
say "board now $(b "uname -sm; cksum /boot/boot0/vmunix $old" 2>&1 | tr '\n' ' ') expected $(cksum < "$out/board-vmunix" | cut -d' ' -f1)"
say "end $(date +%H:%M)"
