#!/bin/sh
# ws291-p013: kernels compared on the RTX 2070 in one window, with one tree's programs, run from the top of the tree in the
# 10.0.10.3 machine's lock:
#   FILES='DEST=SRC ...' [RESTORE_CKSUM=N] [LIST=plan/ws291/tests/rtx-ab.list] [ROUNDS=2] [SHOT=1]
#       sh plan/ws291/tests/rtx-p013.sh OUT NAME=VMUNIX...
# The NVMe kernel is read back (OUT/nvme-vmunix, checked against RESTORE_CKSUM), FILES put in (rtx-files.sh); then in
# ROUNDS rounds (the order given, then reversed, and so on) each kernel put in (zedBSD restarted) and the gpu-perf bench
# LIST run (its table in OUT/NAME-ROUND/bench/table.txt, the fps lines printed as "NAME ROUND LABEL ..."); with SHOT=1
# also mview's still views of 100 copies (one instanced draw a group, a draw a copy and group, one submission a copy)
# compared with each other (OUT/NAME-ROUND/{one,each,subs}.png).  Then the files and the NVMe kernel put back, checked
# by cksum, and Debian again.  OUT/summary.txt holds what is printed.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
out=${1:?usage: rtx-p013.sh OUT NAME=VMUNIX...}
shift
export GPU_PERF_MACHINE=rtx
M="sh plan/tools/gpu-perf/measure.sh"
KEY=${KEY:-plan/tmp/guest/id_ed25519}
SSHOPT="-o BatchMode=yes -o ConnectTimeout=8 -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR"
list=${LIST:-plan/ws291/tests/rtx-ab.list}
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

# shots DIR: the three still views of 100 copies, compared.
shots() {
	for v in "one|--copies=100" "each|--copies=100 --draws=each" "subs|--copies=100 --draws=each --submits=100"; do
		view=${v%%|*}; opts=${v#*|}
		ZT=90 z "[ -S /run/aat-input.sock ] || { aat-input start --width 1920 --height 1080 > /dev/null 2>&1; sleep 3; }
rm -f /tmp/shot.png
U=\$(awk -F: '\$3 == 1000 { print \$1; exit }' /etc/passwd)
su \$U -c 'env XDG_RUNTIME_DIR=/run/user/1000 WAYLAND_DISPLAY=wayland-0 /bin/mview --token=shot --frames=900 --content=none --present=fifo $opts' > /tmp/mview-shot.log 2>&1 < /dev/null &
sleep 6
env XDG_RUNTIME_DIR=/run/user/1000 keiland-shot /tmp/shot.png > /dev/null 2>&1 < /dev/null
wait
grep -E 'MVIEW (DONE|FAILED)' /tmp/mview-shot.log" 2>&1 | sed "s/^/$2 $view /" | tee -a "$summary"
		timeout 60 scp -i "$KEY" $SSHOPT "root@$ip:/tmp/shot.png" "$1/$view.png" > /dev/null 2>&1 || say "$2 FAIL $view shot"
	done
	for view in each subs; do
		[ -s "$1/one.png" ] && [ -s "$1/$view.png" ] &&
		    python3 -I plan/tools/frame-ops/shot-diff.py "$1/one.png" "$1/$view.png" 2>&1 | sed "s/^/$2 one-vs-$view /" | tee -a "$summary"
	done
}

say "start $(date +%H:%M)"
ip=$(timeout 700 $M zedbsd 2> "$out/zedbsd.txt" | tail -1)
[ -n "$ip" ] || { say "FAIL zedBSD did not come up"; exit 1; }
files_restore_kernel "$out" || exit 1
failed=0
files_put "$out" || failed=1
order="$*"
round=0
while [ $failed = 0 ] && [ $round -lt "${ROUNDS:-2}" ]; do
	round=$((round + 1))
	for arm in $order; do
		name=${arm%%=*}; kernel=${arm#*=}
		dir=$out/$name-$round
		mkdir -p "$dir"
		timeout 700 $M deploy --kernel "$kernel" > "$dir/deploy.txt" 2>&1 || say "$name $round FAIL deploy"
		ip=$($M zedbsd 2> /dev/null | tail -1)
		[ -n "$ip" ] || { say "$name $round FAIL zedBSD did not come up"; failed=1; break; }
		say "$name $round kernel $(cksum < "$kernel" | cut -d' ' -f1) on zedBSD $(z 'cksum /boot/boot0/vmunix' 2>&1 | cut -d' ' -f1) $(date +%H:%M)"
		timeout 1500 $M bench zedbsd "$dir/bench" "$list" > "$dir/bench.txt" 2>&1
		grep -E "^(big|subs|scene)-[0-9]+ " "$dir/bench.txt" | sed "s/^/$name $round /" | tee -a "$summary"
		ip=$($M zedbsd 2> /dev/null | tail -1)
		[ "${SHOT:-0}" = 1 ] && [ -n "$ip" ] && shots "$dir" "$name $round"
		z "dmesg | grep -E 'fault|lost|MMU|executor fail|refused' | tail -4" 2>&1 | sed "s/^/$name $round kernel /" | tee -a "$summary"
	done
	reversed=""
	for arm in $order; do reversed="$arm $reversed"; done
	order=$reversed
done
ip=$(timeout 700 $M zedbsd 2> /dev/null | tail -1)
if [ -n "$ip" ]; then
	files_back "$out"
else
	say "FAIL nothing put back: zedBSD did not answer"
fi
timeout 400 $M linux > "$out/linux.txt" 2>&1 || say "FAIL back to Debian"
say "debian $(timeout 60 ssh $SSHOPT awe@10.0.10.3 "uname -sr; lspci -k -d 10de: | grep -c 'in use: vfio-pci'" 2>&1 | tr '\n' ' ') (RTX functions on vfio-pci)"
say "end $(date +%H:%M)"
