#!/bin/sh
# ws291-p024: the Linux reference (NVK on nouveau with the GSP firmware) on the RTX 2070 of 10.0.10.3 taken apart
# stage by stage, run from the top of the tree in the machine's lock with Debian running:
#   [LIST=plan/ws291/tests/nvk-profile.list] [SPIN=8] sh plan/ws291/tests/nvk-profile.sh OUT
# linux-perf and strace are put on Debian (apt) when missing.  The RTX goes from vfio-pci to nouveau for this run
# only; weston (kiosk, DRM) as measure.sh linux-mview starts it.  For each LIST line (LABEL|OPTIONS), Debian's
# ~/linux-mview runs as game content on a mailbox of 4 images four times:
#   plain   the fps and the frame's stages (MVIEW STAGES);
#   stat    under perf stat -I 1000: task-clock, user and system time, the context switches and the system calls by
#           kind (ioctl, poll, futex and all), each a frame (nvk-stat.py: the median second over the fps);
#   trace   under perf trace -s: each system call's count and mean time (the summary);
#   record  under perf record (all CPUs from 4 s in, 4999 Hz, no call graph): the top symbols by DSO (perf report).
# At the end weston is stopped, the RTX given back to vfio-pci (no modprobe -r: its softdep takes vfio_pci out),
# perf runs as root (sudo) and mview under it as the user again.  OUT/summary.txt holds what is printed; each run's files are in OUT/LABEL-*.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
out=${1:?usage: nvk-profile.sh OUT}
export GPU_PERF_MACHINE=rtx
M="sh plan/tools/gpu-perf/measure.sh"
SSHOPT="-o BatchMode=yes -o ConnectTimeout=8 -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR"
DEB=awe@10.0.10.3
PCI=0000:01:00.0
SPIN=${SPIN:-8}
AS="sudo -n"
BACK="sudo -n -u awe env XDG_RUNTIME_DIR=/run/user/1000 WAYLAND_DISPLAY=wayland-1"
list=${LIST:-plan/ws291/tests/nvk-profile.list}
mkdir -p "$out"
summary=$out/summary.txt
: > "$summary"

say() {
	echo "$*" | tee -a "$summary"
}

deb() {
	timeout "${DT:-120}" ssh -n $SSHOPT "$DEB" "$@"
}

say "start $(date +%H:%M)"
timeout 400 $M linux > "$out/linux.txt" 2>&1 || { say "FAIL Debian is not running"; exit 1; }
DT=600 deb "command -v perf > /dev/null && command -v strace > /dev/null || sudo -n env DEBIAN_FRONTEND=noninteractive apt-get install -y linux-perf strace" > "$out/apt.txt" 2>&1 || say "FAIL apt"
say "debian $(deb 'uname -r; perf --version; cksum < ~/linux-mview/bin/mview' 2>&1 | tr '\n' ' ')"
deb "echo $PCI | sudo tee /sys/bus/pci/drivers/vfio-pci/unbind > /dev/null; echo nouveau | sudo tee /sys/bus/pci/devices/$PCI/driver_override > /dev/null; sudo modprobe nouveau modeset=1 config=NvGspRm=1; sleep 2; test -e /sys/bus/pci/drivers/nouveau/$PCI || echo $PCI | sudo tee /sys/bus/pci/drivers/nouveau/bind > /dev/null; sleep 5; lspci -k -s $PCI | grep 'in use'" > "$out/nouveau.txt" 2>&1
say "linux $(cat "$out/nouveau.txt" | tr '\n' ' ')"
# weston started by one short plain run.
timeout 180 $M linux-mview "$out/warm" --token=warm --spin=2 --copies=1 --content=game --present=mailbox --images=4 > "$out/warm.txt" 2>&1

run() {
	# run LABEL KIND WRAP OPTIONS...: mview SPIN seconds under WRAP (empty: none) on weston, its log and WRAP's in
	# OUT/LABEL-KIND.*
	rl=$1; rk=$2; rw=$3
	shift 3
	DT=$((SPIN + 120)) deb "export XDG_RUNTIME_DIR=/run/user/1000 WAYLAND_DISPLAY=wayland-1; $rw ~/linux-mview/bin/mview --model=\$HOME/linux-mview/share/mview/qs40 --token=$rl --spin=$SPIN --content=game --present=mailbox --images=4 $* > /tmp/nvkp.mview 2> /tmp/nvkp.err < /dev/null; cat /tmp/nvkp.mview" > "$out/$rl-$rk.txt" 2>&1
	timeout 120 scp -q $SSHOPT "$DEB:/tmp/nvkp.err" "$out/$rl-$rk.err" > /dev/null 2>&1
}

frames() {
	grep 'MVIEW SPIN' "$1" | grep -o 'frames=[0-9]*' | head -1 | cut -d= -f2
}

grep -v '^#' "$list" | while IFS='|' read -r label opts; do
	[ -n "$label" ] || continue
	run "$label" plain "" "$opts"
	say "$label plain $(grep -E 'MVIEW (SPIN|STAGES)' "$out/$label-plain.txt" | sed -E 's/.*(fps=[0-9.]+).*/\1/; s/.*(frame=.*)/\1/' | tr '\n' ' ')"
	run "$label" stat "$AS perf stat -I 1000 -x, -e task-clock,user_time,system_time,context-switches,raw_syscalls:sys_enter,syscalls:sys_enter_ioctl,syscalls:sys_enter_ppoll,syscalls:sys_enter_poll,syscalls:sys_enter_futex,syscalls:sys_enter_write,syscalls:sys_enter_read -o /tmp/nvkp.stat -- $BACK" "$opts"
	deb "cat /tmp/nvkp.stat" > "$out/$label-stat.csv" 2>&1
	fps=$(grep -oE 'fps=[0-9.]+' "$out/$label-stat.txt" | head -1 | cut -d= -f2)
	say "$label stat fps=$fps (each a frame: the median second's count over the fps)"
	python3 -I plan/ws291/tests/nvk-stat.py "$out/$label-stat.csv" "${fps:-0}" | sed 's/^/  /' | tee -a "$summary"
	run "$label" trace "$AS perf trace -s -o /tmp/nvkp.trace -- $BACK" "$opts"
	deb "cat /tmp/nvkp.trace" > "$out/$label-trace.txt.s" 2>&1
	say "$label trace $(grep -oE 'fps=[0-9.]+' "$out/$label-trace.txt" | head -1) frames=$(frames "$out/$label-trace.txt")"
	grep -E '^ +(ioctl|ppoll|poll|futex|write|read|sendmsg|recvmsg|epoll_wait|sched_yield|clock_nanosleep) ' "$out/$label-trace.txt.s" | sed 's/^/  /' | tee -a "$summary"
	run "$label" record "$AS perf record -a -D 4000 -F 4999 -o /tmp/nvkp.data -- $BACK" "$opts"
	DT=300 deb "sudo -n perf report -i /tmp/nvkp.data --stdio --sort comm,dso -q 2> /dev/null | head -30; echo ===; sudo -n perf report -i /tmp/nvkp.data --stdio --sort dso,sym -q --comm mview 2> /dev/null | head -60" > "$out/$label-record.report" 2>&1
	say "$label record $(grep -oE 'fps=[0-9.]+' "$out/$label-record.txt" | head -1)"
	sed -n '1,12p' "$out/$label-record.report" | sed 's/^/  /' | tee -a "$summary"
done

# Back to vfio-pci.
deb "pkill -x weston; sleep 2; echo $PCI | sudo tee /sys/bus/pci/drivers/nouveau/unbind > /dev/null; echo vfio-pci | sudo tee /sys/bus/pci/devices/$PCI/driver_override > /dev/null; echo $PCI | sudo tee /sys/bus/pci/drivers/vfio-pci/bind > /dev/null; echo | sudo tee /sys/bus/pci/devices/$PCI/driver_override > /dev/null; sleep 2; lspci -k -s $PCI | grep 'in use'" > "$out/vfio.txt" 2>&1
say "debian $(deb "lspci -k -d 10de: | grep -c 'in use: vfio-pci'" 2>&1 | tr '\n' ' ') (RTX functions on vfio-pci)"
say "end $(date +%H:%M)"
