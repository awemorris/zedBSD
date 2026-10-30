#!/bin/sh
# ws291-p012: the GPU's temperature under mview's scene on the RTX 2070, zedBSD and then Linux (NVK), run from the top
# of the tree in the 10.0.10.3 machine's lock:
#   FILES='DEST=SRC ...' [RESTORE_CKSUM=N] [REPS=6] [LINUX=1] sh plan/ws291/tests/rtx-thermal.sh OUT VMUNIX
# VMUNIX is a kernel that writes the thermal sensor into its perf lines (plan/ws291/tests/nvrtx-temp-diag.patch).  The
# NVMe kernel is read back, FILES and VMUNIX put in, the driver's counts on (nvrtx.vk.perf=1), the scene of 5000 copies
# REPS times back to back (12 s each, game content, mailbox, 4 images) with the kernel's temperature lines after each;
# the counts off, the files and the kernel put back, Debian again.  With LINUX=1 this tree's Linux mview (LINUX_MVIEW,
# default build/ws291/linux-mview) is copied to Debian's ~/linux-mview, the RTX moved from vfio-pci to
# nouveau (GSP), the same scene run REPS times with NVK on weston with the hwmon temperature after each, and the RTX
# given back to vfio-pci.  OUT/summary.txt holds what is printed.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
out=${1:?usage: rtx-thermal.sh OUT VMUNIX}
kernel=${2:?the kernel}
export GPU_PERF_MACHINE=rtx
M="sh plan/tools/gpu-perf/measure.sh"
KEY=${KEY:-plan/tmp/guest/id_ed25519}
SSHOPT="-o BatchMode=yes -o ConnectTimeout=8 -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR"
DEB=awe@10.0.10.3
PCI=0000:01:00.0
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

deb() {
	timeout "${DT:-120}" ssh $SSHOPT "$DEB" "$@"
}

. plan/ws291/tests/rtx-files.sh

say "start $(date +%H:%M)"
ip=$(timeout 600 $M zedbsd 2> "$out/zedbsd.txt" | tail -1)
[ -n "$ip" ] || { say "FAIL zedBSD did not come up"; exit 1; }
files_restore_kernel "$out" || exit 1
files_put "$out" || failed=1
[ $failed = 0 ] && { timeout 400 $M deploy --kernel "$kernel" > "$out/deploy.txt" 2>&1 || { say "FAIL deploy"; failed=1; }; }
[ $failed = 0 ] && { timeout 400 $M timing on > "$out/timing-on.txt" 2>&1 || { say "FAIL timing on"; failed=1; }; }
ip=$($M zedbsd 2> /dev/null | tail -1)
if [ $failed = 0 ] && [ -n "$ip" ]; then
	say "kernel $(cksum < "$kernel" | cut -d' ' -f1) idle $(z "dmesg | grep 'DIAG temp' | tail -1" 2>&1)"
	i=1
	while [ $i -le "${REPS:-6}" ]; do
		timeout 180 $M mview "$out/z$i" --token="z$i" --spin=12 --copies=5000 --content=game --present=mailbox --images=4 > "$out/z$i.txt" 2>&1
		ip=$($M zedbsd 2> /dev/null | tail -1)
		say "zedbsd $i $(grep -oE 'fps=[0-9.]+' "$out/z$i.txt" | head -1) | $(z "dmesg | grep -E 'DIAG temp|gpu [0-9]+ avg' | tail -2" 2>&1 | sed 's/.*perf: //' | tr '\n' ' ')"
		i=$((i + 1))
	done
	timeout 400 $M timing off > "$out/timing-off.txt" 2>&1 || say "FAIL timing off"
fi
ip=$(timeout 600 $M zedbsd 2> /dev/null | tail -1)
if [ -n "$ip" ]; then
	files_back "$out"
else
	say "FAIL nothing put back: zedBSD did not answer"
fi
timeout 400 $M linux > "$out/linux.txt" 2>&1 || say "FAIL back to Debian"

if [ "${LINUX:-0}" = 1 ]; then
	# This tree's Linux mview (plan/tools/gpu-perf/linux/build-mview.sh LINUX_MVIEW) where measure.sh linux-mview runs it.
	timeout 120 scp -q -r $SSHOPT "${LINUX_MVIEW:-build/ws291/linux-mview}/." "$DEB:linux-mview/" || say "FAIL the Linux mview was not copied"
	# The RTX to nouveau with the GSP firmware for this run only (the configuration files are not changed).
	deb "echo $PCI | sudo tee /sys/bus/pci/drivers/vfio-pci/unbind > /dev/null; echo nouveau | sudo tee /sys/bus/pci/devices/$PCI/driver_override > /dev/null; sudo modprobe nouveau modeset=1 config=NvGspRm=1; sleep 2; test -e /sys/bus/pci/drivers/nouveau/$PCI || echo $PCI | sudo tee /sys/bus/pci/drivers/nouveau/bind > /dev/null; sleep 5; lspci -k -s $PCI | grep 'in use'; for h in /sys/class/hwmon/hwmon*; do echo \$h \$(cat \$h/name) \$(cat \$h/temp1_input 2>/dev/null); done" > "$out/nouveau.txt" 2>&1
	sed 's/^/linux /' "$out/nouveau.txt" | tee -a "$summary"
	hw=$(awk '$2 == "nouveau" {print $1; exit}' "$out/nouveau.txt")
	i=1
	while [ $i -le "${REPS:-6}" ]; do
		timeout 180 $M linux-mview "$out/l$i" --token="l$i" --spin=12 --copies=5000 --content=game --present=mailbox --images=4 > "$out/l$i.txt" 2>&1
		say "linux $i $(grep -oE 'fps=[0-9.]+' "$out/l$i.txt" | head -1) | temp $(deb "cat $hw/temp1_input 2>/dev/null; cat $hw/fan1_input 2>/dev/null; cat $hw/power1_input 2>/dev/null" 2>&1 | tr '\n' ' ')"
		i=$((i + 1))
	done
	# Back to vfio-pci: weston stopped, nouveau let go of the RTX, vfio-pci given it again (no modprobe -r: its softdep takes vfio_pci out).
	deb "pkill -x weston; sleep 2; echo $PCI | sudo tee /sys/bus/pci/drivers/nouveau/unbind > /dev/null; echo vfio-pci | sudo tee /sys/bus/pci/devices/$PCI/driver_override > /dev/null; echo $PCI | sudo tee /sys/bus/pci/drivers/vfio-pci/bind > /dev/null; echo | sudo tee /sys/bus/pci/devices/$PCI/driver_override > /dev/null; sleep 2; lspci -k -s $PCI | grep 'in use'" > "$out/vfio.txt" 2>&1
fi
say "debian $(deb "uname -sr; lspci -k -d 10de: | grep -c 'in use: vfio-pci'" 2>&1 | tr '\n' ' ') (RTX functions on vfio-pci)"
say "end $(date +%H:%M)"
