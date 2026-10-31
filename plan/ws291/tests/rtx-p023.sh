#!/bin/sh
# ws291-p023: the block-linear colour attachments and the block-linear scanout on the RTX 2070 (10.0.10.3), run from the
# top of the tree in the machine's lock (flock /tmp/zedbsd-rdna.lock), one tree's programs (FILES, rtx-files.sh):
#   FILES='DEST=SRC ...' USB_MVIEW=MVIEW [RESTORE_CKSUM=N] [PARTS='ab desk scenes'] sh plan/ws291/tests/rtx-p023.sh OUT BASE NEW
# ab: rtx-p013.sh with base=BASE and new=NEW (SHOT=1 ROUNDS=2 PROF=1, rtx-ab.list); desk: rtx-desk-main.sh with NEW
# (RUNS=1); scenes: for BASE, NEW and NEW with nvrtx.disp.bl=1 each, the desktop still, Notes and the browser opened
# from App Home captured by keiland-shot (OUT/scenes/NAME-*.png, compared with BASE's by shot-diff.py), the kernel's
# block-linear lines and a game-mode mview run (its MVIEW and KWL SCANOUT lines); nvrtx.disp.bl taken out after.
# Each part reads the NVMe kernel back, puts FILES in and back and ends in Debian; OUT/summary.txt holds what is printed.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
out=${1:?usage: rtx-p023.sh OUT BASE NEW}
base=${2:?the base kernel}
new=${3:?the new kernel}
export GPU_PERF_MACHINE=rtx
M="sh plan/tools/gpu-perf/measure.sh"
KEY=${KEY:-plan/tmp/guest/id_ed25519}
SSHOPT="-o BatchMode=yes -o ConnectTimeout=8 -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR"
mkdir -p "$out/scenes"
summary=$out/summary.txt
: > "$summary"
failed=0

say() {
	echo "$*" | tee -a "$summary"
}

z() {
	timeout "${ZT:-60}" ssh -i "$KEY" $SSHOPT "root@$ip" "$@"
}

. plan/ws291/tests/rtx-files.sh

# scene NAME: the desktop still, Notes and the browser from App Home, each captured, on the running zedBSD.
scene() {
	for step in desktop notes browser; do
		case $step in
		desktop) action='true' ;;
		*) action="aat-input key super > /dev/null; aat-input sleep 800 > /dev/null; aat-input type $step > /dev/null; aat-input sleep 500 > /dev/null; aat-input key enter > /dev/null; sleep 6" ;;
		esac
		ZT=90 z "[ -S /run/aat-input.sock ] || { aat-input start --width 1920 --height 1080 > /dev/null 2>&1; sleep 3; }
aat-input move-to 1900 1060 > /dev/null; $action; rm -f /tmp/scene.png
env XDG_RUNTIME_DIR=/run/user/1000 keiland-shot /tmp/scene.png > /dev/null 2>&1 < /dev/null; ls -l /tmp/scene.png | wc -l" > /dev/null 2>&1
		timeout 60 scp -O -i "$KEY" $SSHOPT "root@$ip:/tmp/scene.png" "$out/scenes/$1-$step.png" > /dev/null 2>&1 ||
		    say "$1 FAIL $step shot"
		if [ "$1" != base ] && [ -s "$out/scenes/base-$step.png" ] && [ -s "$out/scenes/$1-$step.png" ]; then
			python3 -I plan/tools/frame-ops/shot-diff.py "$out/scenes/base-$step.png" "$out/scenes/$1-$step.png" \
			    "$out/scenes/$1-$step-diff.png" 2>&1 | sed "s/^/$1 $step vs base /" | tee -a "$summary"
		fi
	done
}

# scenes: each kernel's scene, the block-linear lines and a game-mode run, the files and the kernel put back.
scenes() {
	ip=$(timeout 700 $M zedbsd 2> "$out/scenes/zedbsd.txt" | tail -1)
	[ -n "$ip" ] || { say "scenes FAIL zedBSD did not come up"; return 1; }
	files_restore_kernel "$out/scenes" || return 1
	files_put "$out/scenes" || return 1
	for arm in base new bl; do
		kernel=$new
		[ $arm = base ] && kernel=$base
		timeout 700 $M deploy --kernel "$kernel" > "$out/scenes/$arm-deploy.txt" 2>&1 || say "$arm FAIL deploy"
		if [ $arm = bl ]; then
			timeout 900 $M cfg nvrtx.disp.bl 1 > "$out/scenes/bl-cfg.txt" 2>&1 || say "bl FAIL cfg"
		fi
		ip=$(timeout 700 $M zedbsd 2> /dev/null | tail -1)
		[ -n "$ip" ] || { say "$arm FAIL zedBSD did not come up"; continue; }
		say "$arm kernel $(z 'cksum /boot/boot0/vmunix' 2>&1 | cut -d' ' -f1) bl lines $(z "dmesg | grep -c 'block-linear scanout'" 2>&1) $(date +%H:%M)"
		scene "$arm"
		timeout 300 $M mview "$out/scenes/$arm-game" --content=game --copies=100 --spin=10 > "$out/scenes/$arm-game.txt" 2>&1
		grep -E 'MVIEW (SPIN|DONE|FAILED)|KWL SCANOUT' "$out/scenes/$arm-game.txt" | tail -4 | sed "s/^/$arm game /" | tee -a "$summary"
		ip=$(timeout 700 $M zedbsd 2> /dev/null | tail -1)
		z "dmesg | grep -E 'block-linear|fault|lost|MMU|executor fail|refused' | tail -6" 2>&1 | sed "s/^/$arm kernel /" | tee -a "$summary"
	done
	timeout 900 $M cfg nvrtx.disp.bl > "$out/scenes/bl-cfg-off.txt" 2>&1 || say "FAIL nvrtx.disp.bl not taken out"
	ip=$(timeout 700 $M zedbsd 2> /dev/null | tail -1)
	if [ -n "$ip" ]; then
		say "cfg $(z "grep -c '^nvrtx.disp.bl=' /boot/boot0/zedbsd.cfg" 2>&1) nvrtx.disp.bl lines"
		files_back "$out/scenes"
	else
		say "scenes FAIL nothing put back: zedBSD did not answer"
	fi
	timeout 400 $M linux > "$out/scenes/linux.txt" 2>&1 || say "FAIL back to Debian"
}

say "start $(date +%H:%M) base $(cksum < "$base" | cut -d' ' -f1) new $(cksum < "$new" | cut -d' ' -f1)"
for part in ${PARTS:-ab desk scenes}; do
	case $part in
	ab)
		SHOT=1 ROUNDS=2 PROF=1 sh plan/ws291/tests/rtx-p013.sh "$out/ab" base="$base" new="$new" > "$out/ab.txt" 2>&1
		sed 's/^/ab /' "$out/ab/summary.txt" >> "$summary"
		grep -E ' (big|subs|scene)-[0-9]+ |one-vs|prof .*perf|FAIL|back|expected|debian' "$out/ab/summary.txt" | sed 's/^/ab /' | head -80
		;;
	desk)
		RUNS=1 sh plan/ws291/tests/rtx-desk-main.sh "$out/desk" "$new" > "$out/desk.txt" 2>&1
		sed 's/^/desk /' "$out/desk/summary.txt" | tee -a "$summary"
		;;
	scenes)
		scenes || failed=1
		;;
	esac
done
say "debian $(timeout 60 ssh $SSHOPT awe@10.0.10.3 "uname -sr; lspci -k -d 10de: | grep -c 'in use: vfio-pci'" 2>&1 | tr '\n' ' ') (RTX functions on vfio-pci)"
say "end $(date +%H:%M)"
