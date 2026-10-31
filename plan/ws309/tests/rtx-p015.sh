#!/bin/sh
# ws309-p015: the game mode's lent images on the RTX 2070 (10.0.10.3), two trees' programs on one kernel, run from the
# top of the tree in the machine's lock (flock /tmp/zedbsd-rdna.lock):
#   [RESTORE_CKSUM=N] [ARMS='base new bl'] [LIST=plan/ws291/tests/rtx-ab.list] [CYCLES=3]
#       sh plan/ws309/tests/rtx-p015.sh OUT KERNEL BASE-BUILD NEW-BUILD
# BASE-BUILD and NEW-BUILD are builds of plan/ws291/tests/config.mk (rtx-files.sh's 14 programs and libraries below).
# The NVMe kernel is read back (OUT/nvme-vmunix, checked against RESTORE_CKSUM), KERNEL put in; then for each arm
# (base: BASE-BUILD's files; new: NEW-BUILD's; bl: NEW-BUILD's with nvrtx.disp.bl=1 on the boot line) the arm's files
# put in and zedBSD restarted with the arm's boot line, the gpu-perf bench LIST run (fps lines printed), a game-mode
# cycle (CYCLES mview runs in the game mode, the compositor's mode lines counted), then the driver's counts on
# (nvrtx.vk.perf=1, restarted) for subs-100 and big-1000 with libvulkan's chain log (VULKAN_ZEDBSD_WSI_LOG: the chain's
# lend, its images' layout, its presents and copies) and the compositor's game lines, and the counts off.  At the end
# nvrtx.disp.bl is taken out, the files and the NVMe kernel put back, checked by cksum, and Debian again.
# OUT/summary.txt holds what is printed.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
out=${1:?usage: rtx-p015.sh OUT KERNEL BASE-BUILD NEW-BUILD}
kernel=${2:?the kernel}
base_build=${3:?the base build}
new_build=${4:?the new build}
export GPU_PERF_MACHINE=rtx
M="sh plan/tools/gpu-perf/measure.sh"
KEY=${KEY:-plan/tmp/guest/id_ed25519}
SSHOPT="-o BatchMode=yes -o ConnectTimeout=8 -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR"
list=${LIST:-plan/ws291/tests/rtx-ab.list}
SESSION=/run/user/1000/session.log
mkdir -p "$out"
summary=$out/summary.txt
: > "$summary"

say() {
	echo "$*" | tee -a "$summary"
}

z() {
	timeout "${ZT:-60}" ssh -i "$KEY" $SSHOPT "root@$ip" "$@"
}

# files_of BUILD: the FILES list of one build.
files_of() {
	echo "/sbin/networkd=$1/bin/networkd /sbin/wifi=$1/bin/wifi /lib/libc.so=$1/dynamic/libc.so \
/lib/libkeiland.so=$1/dynamic/libkeiland.so /lib/libvulkan.so=$1/dynamic/libvulkan.so \
/lib/libwayland-client.so=$1/dynamic/libwayland-client.so /bin/files=$1/bin/files /bin/settings=$1/bin/settings \
/bin/terminal=$1/bin/terminal /usr/libexec/keiland-ime=$1/bin/keiland-ime /sbin/sessiond=$1/bin/sessiond \
/bin/mview=$1/bin/mview /bin/keiland-shot=$1/bin/keiland-shot /bin/wayland=$1/bin/wayland"
}

. plan/ws291/tests/rtx-files.sh

# cycle DIR ARM: CYCLES game-mode runs of mview one after another; the compositor's lines of them counted.
cycle() {
	ZT=600 z "U=\$(awk -F: '\$3 == 1000 { print \$1; exit }' /etc/passwd)
n=\$(wc -l < $SESSION)
i=0
while [ \$i -lt ${CYCLES:-3} ]; do
	i=\$((i + 1))
	su \$U -c 'env XDG_RUNTIME_DIR=/run/user/1000 WAYLAND_DISPLAY=wayland-0 timeout 60 /bin/mview --token=cy\$i --spin=6 --content=game --present=mailbox --images=4 --copies=100' 2>&1 | grep -E 'MVIEW (DONE|FAILED)' | tail -1
	sleep 3
done
tail -n +\$n $SESSION > /tmp/cycle.log
echo direct=\$(grep -c 'KWL SCANOUT direct=1' /tmp/cycle.log) left=\$(grep -c 'KWL SCANOUT direct=0 reason=no-window' /tmp/cycle.log) window=\$(grep -c 'KWL MODE window' /tmp/cycle.log) failed=\$(grep -cE 'KWL (FAILED|MODE_ERROR)' /tmp/cycle.log) refused=\$(grep -c 'lend refused' /tmp/cycle.log) compositor=\$(ps -A -o args | grep -cE '^/bin/wayland')
grep -E 'KWL SCANOUT (direct|lend)' /tmp/cycle.log | tail -6" > "$1/cycle.txt" 2>&1
	sed "s/^/$2 cycle /" "$1/cycle.txt" | tee -a "$summary"
}

# prof DIR ARM: subs-100 and big-1000 with the driver's counts and libvulkan's chain log.
prof() {
	timeout 900 $M timing on > "$1/timing-on.txt" 2>&1 || say "$2 FAIL timing on"
	ip=$(timeout 700 $M zedbsd 2> /dev/null | tail -1)
	ZT=300 z 'U=$(awk -F: "\$3 == 1000 { print \$1; exit }" /etc/passwd)
for a in "subs-100|--draws=each --copies=100 --submits=100" "big-1000|--draws=each --copies=1000"; do
	label=${a%%|*}; opts=${a#*|}
	n=$(dmesg | wc -l)
	s=$(wc -l < '"$SESSION"')
	su $U -c "env XDG_RUNTIME_DIR=/run/user/1000 WAYLAND_DISPLAY=wayland-0 VULKAN_ZEDBSD_WSI_LOG=1 timeout 40 /bin/mview --token=p --spin=11 --content=game --present=mailbox --images=4 $opts" 2>&1 |
	    grep -E "MVIEW (SPIN|FAILED)|VULKAN WSI" | tail -12 | sed "s/^/$label /"
	dmesg | tail -n +$n | grep -E "perf|executor|lost|fault" | tail -14 | sed "s/^/$label /"
	tail -n +$s '"$SESSION"' | grep -E "KWL SCANOUT|KWL MODE" | tail -8 | sed "s/^/$label /"
done' > "$1/prof.txt" 2>&1
	sed "s/^/$2 prof /" "$1/prof.txt" | tee -a "$summary"
	timeout 900 $M timing off > "$1/timing-off.txt" 2>&1 || say "$2 FAIL timing off"
}

say "start $(date +%H:%M) kernel $(cksum < "$kernel" | cut -d' ' -f1)"
ip=$(timeout 700 $M zedbsd 2> "$out/zedbsd.txt" | tail -1)
[ -n "$ip" ] || { say "FAIL zedBSD did not come up"; exit 1; }
files_restore_kernel "$out" || exit 1
timeout 900 $M deploy --kernel "$kernel" > "$out/deploy.txt" 2>&1 || say "FAIL deploy"
for arm in ${ARMS:-base new bl}; do
	dir=$out/$arm
	mkdir -p "$dir"
	build=$new_build
	[ $arm = base ] && build=$base_build
	FILES=$(files_of "$build")
	ip=$(timeout 700 $M zedbsd 2> /dev/null | tail -1)
	[ -n "$ip" ] || { say "$arm FAIL zedBSD did not come up"; break; }
	files_put "$dir" || { say "$arm FAIL files"; break; }
	# The arm's boot line, zedBSD restarted (the new compositor and libraries load).
	if [ $arm = bl ]; then
		timeout 900 $M cfg nvrtx.disp.bl 1 > "$dir/cfg.txt" 2>&1 || say "$arm FAIL cfg"
	else
		timeout 900 $M cfg nvrtx.disp.bl > "$dir/cfg.txt" 2>&1 || say "$arm FAIL cfg"
	fi
	ip=$(timeout 700 $M zedbsd 2> /dev/null | tail -1)
	[ -n "$ip" ] || { say "$arm FAIL zedBSD did not come up"; break; }
	say "$arm kernel $(z 'cksum /boot/boot0/vmunix' 2>&1 | cut -d' ' -f1) wayland $(z 'cksum /bin/wayland' 2>&1 | cut -d' ' -f1) bl lines $(z "dmesg | grep -c 'block-linear scanout'" 2>&1) lendable $(z "grep -o 'lendable=[0-9]*' $SESSION | tail -1" 2>&1) $(date +%H:%M)"
	USB_MVIEW=$build/bin/mview timeout 1500 $M bench zedbsd "$dir/bench" "$list" > "$dir/bench.txt" 2>&1
	grep -E "^(big|subs|scene)-[0-9]+ " "$dir/bench.txt" | sed "s/^/$arm /" | tee -a "$summary"
	ip=$(timeout 700 $M zedbsd 2> /dev/null | tail -1)
	[ -n "$ip" ] && cycle "$dir" "$arm"
	[ -n "$ip" ] && prof "$dir" "$arm"
	ip=$(timeout 700 $M zedbsd 2> /dev/null | tail -1)
	z "dmesg | grep -E 'block-linear|fault|lost|MMU|executor fail|refused' | tail -6" 2>&1 | sed "s/^/$arm kernel /" | tee -a "$summary"
done
timeout 900 $M cfg nvrtx.disp.bl > "$out/cfg-off.txt" 2>&1 || say "FAIL nvrtx.disp.bl not taken out"
ip=$(timeout 700 $M zedbsd 2> /dev/null | tail -1)
if [ -n "$ip" ]; then
	say "cfg $(z "grep -c '^nvrtx.disp.bl=' /boot/boot0/zedbsd.cfg" 2>&1) nvrtx.disp.bl lines"
	files_back "$out"
else
	say "FAIL nothing put back: zedBSD did not answer"
fi
timeout 400 $M linux > "$out/linux.txt" 2>&1 || say "FAIL back to Debian"
say "debian $(timeout 60 ssh $SSHOPT awe@10.0.10.3 "uname -sr; lspci -k -d 10de: | grep -c 'in use: vfio-pci'" 2>&1 | tr '\n' ' ') (RTX functions on vfio-pci)"
say "end $(date +%H:%M)"
