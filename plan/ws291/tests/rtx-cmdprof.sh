#!/bin/sh
# ws291 (NVIDIA): one window on the RTX 2070 for the kernel's side of mview's command streams: KERNEL put in, the driver's
# counts on (nvrtx.vk.perf=1, zedBSD restarted), mview big-1000, subs-100 and subs-1000 (game content, mailbox) with
# libvulkan's submission timing and the driver's perf lines, the counts off, RESTORE put back, Debian again.
# Needs build/ws291/usb/bin/mview.  OUT/summary.txt holds what is printed.
#   sh plan/ws291/tests/rtx-cmdprof.sh OUT KERNEL RESTORE   (from the top of the tree, in the RDNA machine's lock)
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
out=${1:?usage: rtx-cmdprof.sh OUT KERNEL RESTORE}
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
timeout 400 $M timing on > "$out/timing-on.txt" 2>&1 || say "FAIL timing on"
ip=$($M zedbsd 2> /dev/null | tail -1)
if [ -n "$ip" ]; then
	timeout 60 scp -i "$KEY" $SSHOPT build/ws291/usb/bin/mview "root@$ip:/tmp/mview" > /dev/null 2>&1 || say "FAIL mview copy"
	timeout 300 ssh -i "$KEY" $SSHOPT "root@$ip" 'U=$(awk -F: "\$3 == 1000 { print \$1; exit }" /etc/passwd)
for a in "big-1000|--draws=each --copies=1000" "subs-100|--draws=each --copies=100 --submits=100" "subs-1000|--draws=each --copies=1000 --submits=1000"; do
	label=${a%%|*}; opts=${a#*|}
	n=$(dmesg | wc -l)
	su $U -c "env XDG_RUNTIME_DIR=/run/user/1000 WAYLAND_DISPLAY=wayland-0 VULKAN_ZEDBSD_SUBMIT_TIMING=1 timeout 40 /tmp/mview --token=p --spin=11 --content=game --present=mailbox --images=4 $opts" 2>&1 |
	    grep -E "MVIEW (SPIN|STAGES|FAILED)|VULKAN TIMING" | tail -4 | sed "s/^/$label /"
	dmesg | tail -n +$n | grep -E "perf|window full|gr after|executor|lost" | tail -18 | sed "s/^/$label /"
done' > "$out/prof.txt" 2>&1
	cat "$out/prof.txt" >> "$summary"
	cat "$out/prof.txt"
fi
timeout 400 $M timing off > "$out/timing-off.txt" 2>&1 || say "FAIL timing off"
ip=$($M zedbsd 2> /dev/null | tail -1)
if [ -n "$ip" ]; then
	timeout 60 scp -i "$KEY" $SSHOPT "$restore" "root@$ip:/tmp/vmunix.restore" &&
	    timeout 60 ssh -i "$KEY" $SSHOPT "root@$ip" 'cp /tmp/vmunix.restore /boot/boot0/vmunix && sync && cksum /boot/boot0/vmunix && grep -c "^nvrtx.vk.perf=" /boot/boot0/zedbsd.cfg' |
	    sed 's/^/restored /' | tee -a "$summary"
fi
timeout 400 $M linux > "$out/linux.txt" 2>&1 || say "FAIL back to Debian"
say "end $(date +%H:%M)"
