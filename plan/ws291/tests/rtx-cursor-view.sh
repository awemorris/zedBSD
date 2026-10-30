#!/bin/sh
# BUG-374: the RTX 2070 machine (10.0.10.3) put on a kernel for the user to look at its desktop's cursor, then put back;
# run from the top of the tree in the 10.0.10.3 machine's lock:
#   sh plan/ws291/tests/rtx-cursor-view.sh build TREE-BUILD           the kernel (nvrtx-cursor-recolor.patch applied by
#                                                                     the caller) and the programs FILES names
#   RESTORE_CKSUM=1463007375 sh plan/ws291/tests/rtx-cursor-view.sh up OUT TREE-BUILD
#   sh plan/ws291/tests/rtx-cursor-view.sh back OUT TREE-BUILD
# up: the NVMe kernel read back (OUT/nvme-vmunix), the tree's programs put in (rtx-files.sh, the old ones kept as
# *.p7old; the NVMe's 10-28 networkd cannot run a main kernel's network), TREE-BUILD/vmunix put in, zedBSD restarted and
# left on its desktop; the compositor's cursor-plane lines printed.  back: the programs and the kernel put back, checked
# by cksum, and Debian again.  OUT/summary-MODE.txt holds what is printed.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
mode=${1:?usage: rtx-cursor-view.sh build|up|back ...}
if [ "$mode" = build ]; then
	b=${2:?the build directory}
	exec nice make -j16 ZEDBSD_CONFIG=plan/ws291/tests/config.mk BUILD="$b" "$b/vmunix" "$b/bin/wayland" "$b/bin/files" \
	    "$b/bin/settings" "$b/bin/terminal" "$b/bin/keiland-ime" "$b/bin/sessiond" "$b/bin/networkd" "$b/bin/wifi" \
	    "$b/dynamic/libc.so" "$b/dynamic/libkeiland.so"
fi
out=${2:?the output directory}
b=${3:?the build directory}
FILES="/sbin/networkd=$b/bin/networkd /sbin/wifi=$b/bin/wifi /lib/libc.so=$b/dynamic/libc.so \
/lib/libkeiland.so=$b/dynamic/libkeiland.so /bin/wayland=$b/bin/wayland /bin/files=$b/bin/files \
/bin/settings=$b/bin/settings /bin/terminal=$b/bin/terminal /usr/libexec/keiland-ime=$b/bin/keiland-ime \
/sbin/sessiond=$b/bin/sessiond"
export GPU_PERF_MACHINE=rtx
M="sh plan/tools/gpu-perf/measure.sh"
KEY=${KEY:-plan/tmp/guest/id_ed25519}
SSHOPT="-o BatchMode=yes -o ConnectTimeout=8 -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR"
mkdir -p "$out"
summary=$out/summary-$mode.txt
: > "$summary"

say() {
	echo "$*" | tee -a "$summary"
}

z() {
	timeout "${ZT:-60}" ssh -i "$KEY" $SSHOPT "root@$ip" "$@"
}

. plan/ws291/tests/rtx-files.sh

say "start $mode $(date +%H:%M)"
ip=$(timeout 600 $M zedbsd 2> "$out/zedbsd-$mode.txt" | tail -1)
[ -n "$ip" ] || { say "FAIL zedBSD did not come up"; exit 1; }
if [ "$mode" = up ]; then
	files_restore_kernel "$out" || exit 1
	files_put "$out" || exit 1
	timeout 400 $M deploy --kernel "$b/vmunix" > "$out/deploy.txt" 2>&1 || { say "FAIL deploy"; exit 1; }
	ip=$(timeout 600 $M zedbsd 2> /dev/null | tail -1)
	[ -n "$ip" ] || { say "FAIL zedBSD did not come back"; exit 1; }
	say "kernel $(cksum < "$b/vmunix" | cut -d' ' -f1) on $(z 'uname -s; cksum /boot/boot0/vmunix' 2>&1 | tr '\n' ' ')"
	sleep 20
	z 'grep -E "KWL CURSOR_PLANE" /run/user/1000/session.log | tail -3' 2>&1 | sed 's/^/zedbsd: /' | tee -a "$summary"
	z 'dmesg | grep -i "cursor" | tail -3' 2>&1 | sed 's/^/dmesg: /' | tee -a "$summary"
else
	files_back "$out"
	timeout 400 $M linux > "$out/linux.txt" 2>&1 || say "FAIL back to Debian"
	say "debian $(timeout 60 ssh $SSHOPT awe@10.0.10.3 "uname -sr; lspci -k -d 10de: | grep -c 'in use: vfio-pci'" 2>&1 | tr '\n' ' ') (RTX functions on vfio-pci)"
fi
say "end $mode $(date +%H:%M)"
