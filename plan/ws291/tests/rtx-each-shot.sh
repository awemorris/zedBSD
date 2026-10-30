#!/bin/sh
# ws291 (NVIDIA): still views of mview's 100 copies on the RTX 2070 with KERNEL: one instanced draw a group
# (OUT/one.png), a draw a copy and group in one submission (OUT/each.png: 1600 draws one after another, each writing only
# the state that differs from the draw before) and in 100 submissions of one copy each (OUT/subs.png), the latter two
# compared with the first by shot-diff.py; RESTORE put back, Debian again.  Needs build/ws291/usb/bin/mview.
# OUT/summary.txt holds what is printed.
#   sh plan/ws291/tests/rtx-each-shot.sh OUT KERNEL RESTORE   (from the top of the tree, in the RDNA machine's lock)
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
out=${1:?usage: rtx-each-shot.sh OUT KERNEL RESTORE}
kernel=${2:?the kernel}
restore=${3:?the kernel to put back}
export GPU_PERF_MACHINE=rtx
M="sh plan/tools/gpu-perf/measure.sh"
KEY=${KEY:-plan/tmp/guest/id_ed25519}
SSHOPT="-o BatchMode=yes -o ConnectTimeout=8 -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR"
mkdir -p "$out"
summary=$out/summary.txt
: > "$summary"
say() {
	echo "$*" | tee -a "$summary"
}
timeout 400 $M deploy --kernel "$kernel" > "$out/deploy.txt" 2>&1 || say "FAIL deploy"
say "kernel $(cksum < "$kernel")"
ip=$($M zedbsd 2> /dev/null | tail -1)
if [ -n "$ip" ]; then
	timeout 60 scp -i "$KEY" $SSHOPT build/ws291/usb/bin/mview "root@$ip:/tmp/mview" > /dev/null 2>&1 || say "FAIL mview copy"
	for v in "one|--copies=100" "each|--copies=100 --draws=each" "subs|--copies=100 --draws=each --submits=100"; do
		name=${v%%|*}; opts=${v#*|}
		timeout 90 ssh -i "$KEY" $SSHOPT "root@$ip" "[ -S /run/aat-input.sock ] || { aat-input start --width 1920 --height 1080 > /dev/null 2>&1; sleep 3; }
rm -f /tmp/shot.png
U=\$(awk -F: '\$3 == 1000 { print \$1; exit }' /etc/passwd)
su \$U -c 'env XDG_RUNTIME_DIR=/run/user/1000 WAYLAND_DISPLAY=wayland-0 /tmp/mview --token=shot --frames=900 --content=none --present=fifo $opts' > /tmp/mview-shot.log 2>&1 < /dev/null &
sleep 6
env XDG_RUNTIME_DIR=/run/user/1000 keiland-shot /tmp/shot.png > /dev/null 2>&1 < /dev/null
wait
grep -E 'MVIEW (DONE|FAILED)' /tmp/mview-shot.log" 2>&1 | sed "s/^/$name /" | tee -a "$summary"
		timeout 60 scp -i "$KEY" $SSHOPT "root@$ip:/tmp/shot.png" "$out/$name.png" > /dev/null 2>&1 || say "FAIL $name shot"
	done
	for v in each subs; do
		[ -s "$out/one.png" ] && [ -s "$out/$v.png" ] && python3 -I plan/tools/frame-ops/shot-diff.py "$out/one.png" "$out/$v.png" 2>&1 | sed "s/^/one-vs-$v /" | tee -a "$summary"
	done
	timeout 60 scp -i "$KEY" $SSHOPT "$restore" "root@$ip:/tmp/vmunix.restore" &&
	    timeout 60 ssh -i "$KEY" $SSHOPT "root@$ip" 'cp /tmp/vmunix.restore /boot/boot0/vmunix && sync && cksum /boot/boot0/vmunix' | sed 's/^/restored /' | tee -a "$summary"
fi
timeout 400 $M linux > "$out/linux.txt" 2>&1 || say "FAIL back to Debian"
say "end $(date +%H:%M)"
