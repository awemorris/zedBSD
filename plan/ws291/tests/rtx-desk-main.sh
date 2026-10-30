#!/bin/sh
# ws291-p010: the desktop's frames on the RTX 2070 with one tree's kernel and desktop programs, run from the top of
# the tree in the 10.0.10.3 machine's lock:
#   FILES='/lib/libc.so=SRC ... /bin/wayland=SRC' [RESTORE_CKSUM=N] [RUNS=2] sh plan/ws291/tests/rtx-desk-main.sh OUT VMUNIX
# The NVMe kernel is read back (OUT/nvme-vmunix, checked against RESTORE_CKSUM), FILES put in (plan/ws291/tests/
# rtx-files.sh: the libraries first, the compositor last, so that it loads with them), VMUNIX put in (zedBSD
# restarted), then frame-ops RUNS times, each on a fresh session (OUT/deskN: the table and every frame of the slow
# steps), the files and the NVMe kernel put back, checked by cksum, and Debian again (its RTX functions on vfio-pci
# counted).  OUT/summary.txt holds what is printed.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
out=${1:?usage: rtx-desk-main.sh OUT VMUNIX}
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

desk() {
	timeout 600 $M desktop "$out/$1" > "$out/$1.txt" 2>&1 || say "$1 FAIL desktop"
	sed "s/^/$1 /" "$out/$1/table.txt" | tee -a "$summary"
	python3 -I plan/tools/frame-ops/frame-times.py "$out/$1/session.log" "$out/$1/marks" --frames > "$out/$1/frames.txt" 2>&1
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
if [ $failed = 0 ] && [ -n "$ip" ]; then
	say "kernel $(cksum < "$kernel" | cut -d' ' -f1) on zedBSD $(z 'uname -s; cksum /boot/boot0/vmunix' 2>&1 | tr '\n' ' ')"
	i=1
	while [ $i -le "${RUNS:-2}" ]; do
		desk "desk$i"
		i=$((i + 1))
	done
fi
ip=$(timeout 600 $M zedbsd 2> /dev/null | tail -1)
if [ -n "$ip" ]; then
	files_back "$out"
else
	say "FAIL nothing put back: zedBSD did not answer"
fi
timeout 400 $M linux > "$out/linux.txt" 2>&1 || say "FAIL back to Debian"
say "debian $(timeout 60 ssh $SSHOPT awe@10.0.10.3 "uname -sr; lspci -k -d 10de: | grep -c 'in use: vfio-pci'" 2>&1 | tr '\n' ' ') (RTX functions on vfio-pci)"
say "end $(date +%H:%M)"
