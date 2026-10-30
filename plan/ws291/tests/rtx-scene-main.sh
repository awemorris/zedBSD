#!/bin/sh
# ws291-p012: mview's scene on the RTX 2070 with one tree's kernel and programs, for each boot setting of SETS, run
# from the top of the tree in the 10.0.10.3 machine's lock:
#   FILES='DEST=SRC ...' [RESTORE_CKSUM=N] [SETS='base nvrtx.boost=1'] [REPS=3] [RUNS='scene cull one']
#       sh plan/ws291/tests/rtx-scene-main.sh OUT VMUNIX
# The NVMe kernel is read back (OUT/nvme-vmunix), FILES put in (rtx-files.sh), VMUNIX put in (zedBSD restarted).  For
# each set ("base": the boot line as it is; NAME=VALUE: that line added to zedbsd.cfg, zedBSD restarted, and taken out
# after) the runs of RUNS, each REPS times, 12 s, game content, mailbox, 4 images: scene (5000 copies), cull (5000 copies,
# every triangle culled: the vertex work alone), one (1 copy), big (scene of 6500 copies); their MVIEW SPIN and
# STAGES lines and the kernel's performance lines.  Then the files and the NVMe kernel put back, checked by cksum, and
# Debian again.  OUT/summary.txt holds what is printed.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
out=${1:?usage: rtx-scene-main.sh OUT VMUNIX}
kernel=${2:?the kernel}
export GPU_PERF_MACHINE=rtx
M="sh plan/tools/gpu-perf/measure.sh"
KEY=${KEY:-plan/tmp/guest/id_ed25519}
SSHOPT="-o BatchMode=yes -o ConnectTimeout=8 -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR"
mkdir -p "$out"
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

# run SET NAME OPTIONS...: one mview run, its fps and stages.
run() {
	set_name=$1
	name=$2
	shift 2
	timeout 180 $M mview "$out/$set_name/$name" --token="$name" --spin=12 --content=game --present=mailbox --images=4 "$@" > "$out/$set_name/$name.txt" 2>&1
	line=$(grep -E 'MVIEW (SPIN|FAILED)' "$out/$set_name/$name.txt" | head -1)
	stages=$(grep -E 'MVIEW STAGES' "$out/$set_name/$name.txt" | head -1)
	scan=$(grep -c 'KWL SCANOUT' "$out/$set_name/$name.txt")
	say "$set_name $name ${line:-no result} | ${stages:-} | scanout lines $scan"
}

say "start $(date +%H:%M)"
ip=$(timeout 600 $M zedbsd 2> "$out/zedbsd.txt" | tail -1)
[ -n "$ip" ] || { say "FAIL zedBSD did not come up"; exit 1; }
files_restore_kernel "$out" || exit 1
files_put "$out" || failed=1
if [ $failed = 0 ]; then
	timeout 400 $M deploy --kernel "$kernel" > "$out/deploy.txt" 2>&1 || { say "FAIL deploy"; failed=1; }
fi
ip=$($M zedbsd 2> /dev/null | tail -1)
[ -n "$ip" ] || failed=1
if [ $failed = 0 ]; then
	say "kernel $(cksum < "$kernel" | cut -d' ' -f1) on zedBSD $(z 'uname -s; cksum /boot/boot0/vmunix' 2>&1 | tr '\n' ' ')"
	for set in ${SETS:-base}; do
		mkdir -p "$out/$set"
		if [ "$set" != base ]; then
			timeout 400 $M cfg "${set%%=*}" "${set#*=}" > "$out/$set/cfg.txt" 2>&1 || { say "$set FAIL cfg"; continue; }
		fi
		ip=$($M zedbsd 2> /dev/null | tail -1)
		z "dmesg | grep -E 'performance' | tail -3" 2>&1 | sed "s/^/$set kernel /" | tee -a "$summary"
		for kind in ${RUNS:-scene cull one}; do
			i=1
			while [ $i -le "${REPS:-3}" ]; do
				case $kind in
				scene) run "$set" "scene-5000-$i" --copies=5000 ;;
				cull) run "$set" "cull-5000-$i" --copies=5000 --cull=all ;;
				one) run "$set" "one-$i" --copies=1 ;;
				big) run "$set" "scene-6500-$i" --copies=6500 ;;
				esac
				i=$((i + 1))
			done
		done
		ip=$($M zedbsd 2> /dev/null | tail -1)
		z "dmesg | grep -E 'performance|fault|lost|MMU' | tail -5" 2>&1 | sed "s/^/$set kernel /" | tee -a "$summary"
		if [ "$set" != base ]; then
			timeout 400 $M cfg "${set%%=*}" > "$out/$set/cfg-off.txt" 2>&1 || say "$set FAIL cfg off"
		fi
	done
fi
ip=$(timeout 600 $M zedbsd 2> /dev/null | tail -1)
if [ -n "$ip" ]; then
	z "grep -v '^#' /boot/boot0/zedbsd.cfg" > "$out/cfg-end.txt" 2>&1
	files_back "$out"
else
	say "FAIL nothing put back: zedBSD did not answer"
fi
timeout 400 $M linux > "$out/linux.txt" 2>&1 || say "FAIL back to Debian"
say "debian $(timeout 60 ssh $SSHOPT awe@10.0.10.3 "uname -sr; lspci -k -d 10de: | grep -c 'in use: vfio-pci'" 2>&1 | tr '\n' ' ') (RTX functions on vfio-pci)"
say "end $(date +%H:%M)"
