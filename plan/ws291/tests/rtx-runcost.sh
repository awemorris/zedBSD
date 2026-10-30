#!/bin/sh
# ws291-p014: the GPU's fixed cost of one small run (mview --draws=each --submits=N), split by a diagnostic kernel whose
# executor takes a variant per session in turn (plan/ws291/tests/nvrtx-run-cost-diag.patch: 0 as built, 1 no shader
# cache invalidation at a push buffer's start, 2 no wait for idle in the barriers, 3 no wait for idle in a run's fence,
# 4 = 1+2+3, 5 = 4 and no copy at a pass's end; the images of 1 to 5 may be wrong), run from the top of the tree in the
# 10.0.10.3 machine's lock:
#   KERNEL=VMUNIX FILES='DEST=SRC ...' [RESTORE_CKSUM=N] [RUNS=12] [OPTS='--draws=each --copies=100 --submits=100']
#       sh plan/ws291/tests/rtx-runcost.sh OUT
# The NVMe kernel is read back (checked against RESTORE_CKSUM), FILES put in (rtx-files.sh), KERNEL put in (zedBSD
# restarted; the driver's counts are turned on only after it), then RUNS mview runs of OPTS with the counts on, each a
# new session: its variant (the kernel's DIAG line), fps, stages and the GPU's time a run ("run N variant V ..."); then
# the files and the kernel put back, checked by cksum, and Debian again.  OUT/summary.txt holds what is printed.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
out=${1:?usage: rtx-runcost.sh OUT}
: "${KERNEL:?KERNEL names the diagnostic kernel}"
export GPU_PERF_MACHINE=rtx
M="sh plan/tools/gpu-perf/measure.sh"
KEY=${KEY:-plan/tmp/guest/id_ed25519}
SSHOPT="-o BatchMode=yes -o ConnectTimeout=8 -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR"
opts=${OPTS:---draws=each --copies=100 --submits=100}
mkdir -p "$out"
summary=$out/summary.txt
: > "$summary"

say() {
	echo "$*" | tee -a "$summary"
}

z() {
	timeout "${ZT:-60}" ssh -i "$KEY" $SSHOPT "root@$ip" "$@"
}

. plan/ws291/tests/rtx-files.sh

say "start $(date +%H:%M)"
ip=$(timeout 700 $M zedbsd 2> "$out/zedbsd.txt" | tail -1)
[ -n "$ip" ] || { say "FAIL zedBSD did not come up"; exit 1; }
files_restore_kernel "$out" || exit 1
failed=0
files_put "$out" || failed=1
# The kernel first: the tree's programs need it (the NVMe's old kernel with them leaves zedBSD without a network).
if [ $failed = 0 ]; then
	timeout 700 $M deploy --kernel "$KERNEL" > "$out/deploy.txt" 2>&1 || say "FAIL deploy"
	ip=$($M zedbsd 2> /dev/null | tail -1)
	[ -n "$ip" ] || { say "FAIL zedBSD did not come up"; failed=1; }
fi
if [ $failed = 0 ]; then
	timeout 700 $M timing on > "$out/timing-on.txt" 2>&1 || say "FAIL timing on"
	ip=$($M zedbsd 2> /dev/null | tail -1)
	[ -n "$ip" ] || { say "FAIL zedBSD did not come up"; failed=1; }
fi
[ $failed = 0 ] && say "kernel $(cksum < "$KERNEL" | cut -d' ' -f1) on zedBSD $(z 'cksum /boot/boot0/vmunix' 2>&1 | cut -d' ' -f1)"
i=0
while [ $failed = 0 ] && [ $i -lt "${RUNS:-12}" ]; do
	i=$((i + 1))
	ZT=120 z 'U=$(awk -F: "\$3 == 1000 { print \$1; exit }" /etc/passwd)
n=$(dmesg | wc -l)
su $U -c "env XDG_RUNTIME_DIR=/run/user/1000 WAYLAND_DISPLAY=wayland-0 timeout 40 /bin/mview --token=r --spin=8 --content=game --present=mailbox --images=4 '"$opts"'" 2>&1 |
    grep -E "MVIEW (SPIN|STAGES|FAILED)" | tail -2
dmesg | tail -n +$n | grep -E "DIAG|perf: gpu|perf [0-9]|fault|lost" | tail -6' > "$out/run$i.txt" 2>&1
	variant=$(grep -o 'variant [0-9]*' "$out/run$i.txt" | head -1)
	sed "s/^/run $i $variant /" "$out/run$i.txt" | tee -a "$summary"
done
timeout 700 $M timing off > "$out/timing-off.txt" 2>&1 || say "FAIL timing off"
ip=$(timeout 700 $M zedbsd 2> /dev/null | tail -1)
if [ -n "$ip" ]; then
	files_back "$out"
else
	say "FAIL nothing put back: zedBSD did not answer"
fi
timeout 400 $M linux > "$out/linux.txt" 2>&1 || say "FAIL back to Debian"
say "debian $(timeout 60 ssh $SSHOPT awe@10.0.10.3 "uname -sr; lspci -k -d 10de: | grep -c 'in use: vfio-pci'" 2>&1 | tr '\n' ' ') (RTX functions on vfio-pci)"
say "end $(date +%H:%M)"
