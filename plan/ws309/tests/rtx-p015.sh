#!/bin/sh
# ws309-p015: the game mode's lent images on the RTX 2070 (10.0.10.3), each arm a kernel and a tree's programs, run from
# the top of the tree in the machine's lock (flock /tmp/zedbsd-rdna.lock):
#   [RESTORE_CKSUM=N] [NVME_KERNEL=VMUNIX] [LIST=plan/ws291/tests/rtx-ab.list] [CYCLES=3] [PARTS='bench cycle scenes desk prof']
#       sh plan/ws309/tests/rtx-p015.sh OUT NAME:KERNEL:BUILD[:bl] ...
# Each BUILD is a build of plan/ws291/tests/config.mk (rtx-files.sh's 14 programs and libraries below).  The NVMe
# kernel is read back (OUT/nvme-vmunix, checked against RESTORE_CKSUM); then for each arm the arm's files and KERNEL
# put in, the boot line given nvrtx.disp.bl=1 for an arm marked bl and none otherwise (zedBSD restarted), and the
# PARTS run: bench, the gpu-perf bench LIST (fps lines printed); cycle, CYCLES mview runs in the game mode (the
# compositor's mode lines counted); scenes, the desktop still, Notes and the browser opened from App Home captured by
# keiland-shot (OUT/NAME/scene-*.png, compared with the first arm's by shot-diff.py); desk, frame-ops on a fresh
# session (OUT/NAME/desk/table.txt, printed); prof, the driver's counts on (nvrtx.vk.perf=1, restarted) for subs-100
# and big-1000 with libvulkan's chain log (VULKAN_ZEDBSD_WSI_LOG: the chain's lend, its images' layout, its presents
# and copies) and the compositor's game lines, and the counts off.  At the end nvrtx.disp.bl is taken out, the files
# and the NVMe kernel put back, checked by cksum, and Debian again.  OUT/summary.txt holds what is printed.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
out=${1:?usage: rtx-p015.sh OUT NAME:KERNEL:BUILD[:bl] ...}
shift
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

# scenes DIR ARM FIRST: the desktop still, Notes and the browser from App Home, each captured and compared with FIRST's.
scenes() {
	for step in desktop notes browser; do
		case $step in
		desktop) action='true' ;;
		*) action="aat-input key super > /dev/null; aat-input sleep 800 > /dev/null; aat-input type $step > /dev/null; aat-input sleep 500 > /dev/null; aat-input key enter > /dev/null; sleep 6" ;;
		esac
		ZT=90 z "[ -S /run/aat-input.sock ] || { aat-input start --width 1920 --height 1080 > /dev/null 2>&1; sleep 3; }
aat-input move-to 1900 1060 > /dev/null; $action; rm -f /tmp/scene.png
env XDG_RUNTIME_DIR=/run/user/1000 keiland-shot /tmp/scene.png > /dev/null 2>&1 < /dev/null; ls -l /tmp/scene.png | wc -l" > /dev/null 2>&1
		timeout 60 scp -O -i "$KEY" $SSHOPT "root@$ip:/tmp/scene.png" "$1/scene-$step.png" > /dev/null 2>&1 ||
		    say "$2 FAIL $step shot"
		if [ "$2" != "$3" ] && [ -s "$out/$3/scene-$step.png" ] && [ -s "$1/scene-$step.png" ]; then
			python3 -I plan/tools/frame-ops/shot-diff.py "$out/$3/scene-$step.png" "$1/scene-$step.png" \
			    "$1/scene-$step-diff.png" 2>&1 | sed "s/^/$2 $step vs $3 /" | tee -a "$summary"
		fi
	done
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

say "start $(date +%H:%M)"
ip=$(timeout 700 $M zedbsd 2> "$out/zedbsd.txt" | tail -1)
[ -n "$ip" ] || { say "FAIL zedBSD did not come up"; exit 1; }
# The kernel to put back: NVME_KERNEL (a copy kept from an earlier run, when the NVMe holds another now) or the NVMe's.
if [ -n "${NVME_KERNEL:-}" ]; then
	cp "$NVME_KERNEL" "$out/nvme-vmunix"
	sum=$(cksum < "$out/nvme-vmunix" | cut -d' ' -f1)
	say "kernel to put back $sum (NVMe now $(z 'cksum < /boot/boot0/vmunix' 2>&1 | cut -d' ' -f1))"
	[ -z "${RESTORE_CKSUM:-}" ] || [ "$sum" = "$RESTORE_CKSUM" ] || { say "FAIL the kernel to put back is $sum, not $RESTORE_CKSUM"; exit 1; }
else
	files_restore_kernel "$out" || exit 1
fi
first=
for spec in "$@"; do
	arm=${spec%%:*}; rest=${spec#*:}
	kernel=${rest%%:*}; rest=${rest#*:}
	build=${rest%%:*}
	bl=
	[ "$rest" != "$build" ] && bl=${rest#*:}
	[ -n "$first" ] || first=$arm
	dir=$out/$arm
	mkdir -p "$dir"
	FILES=$(files_of "$build")
	ip=$(timeout 700 $M zedbsd 2> /dev/null | tail -1)
	[ -n "$ip" ] || { say "$arm FAIL zedBSD did not come up"; break; }
	files_put "$dir" || { say "$arm FAIL files"; break; }
	# The machine's own files (the first arm's kept list) are what files_back checks against.
	[ -s "$out/files-old.txt" ] || cp "$dir/files-old.txt" "$out/files-old.txt"
	timeout 900 $M deploy --kernel "$kernel" > "$dir/deploy.txt" 2>&1 || say "$arm FAIL deploy"
	# The arm's boot line, zedBSD restarted (the new compositor and libraries load).
	if [ "$bl" = bl ]; then
		timeout 900 $M cfg nvrtx.disp.bl 1 > "$dir/cfg.txt" 2>&1 || say "$arm FAIL cfg"
	else
		timeout 900 $M cfg nvrtx.disp.bl > "$dir/cfg.txt" 2>&1 || say "$arm FAIL cfg"
	fi
	ip=$(timeout 700 $M zedbsd 2> /dev/null | tail -1)
	[ -n "$ip" ] || { say "$arm FAIL zedBSD did not come up"; break; }
	say "$arm kernel $(z 'cksum /boot/boot0/vmunix' 2>&1 | cut -d' ' -f1) wayland $(z 'cksum /bin/wayland' 2>&1 | cut -d' ' -f1) cfg bl $(z "grep -c '^nvrtx.disp.bl=' /boot/boot0/zedbsd.cfg" 2>&1) bl lines $(z "dmesg | grep 'block-linear scanout' | tr '\n' ';'" 2>&1) lendable $(z "grep -o 'lendable=[0-9]*' $SESSION | tail -1" 2>&1) $(date +%H:%M)"
	for part in ${PARTS:-bench cycle scenes desk prof}; do
		ip=$(timeout 700 $M zedbsd 2> /dev/null | tail -1)
		[ -n "$ip" ] || { say "$arm FAIL zedBSD did not answer before $part"; break; }
		case $part in
		bench)
			USB_MVIEW=$build/bin/mview timeout 1500 $M bench zedbsd "$dir/bench" "$list" > "$dir/bench.txt" 2>&1
			grep -E "^(big|subs|scene)-[0-9]+ " "$dir/bench.txt" | sed "s/^/$arm /" | tee -a "$summary"
			;;
		cycle) cycle "$dir" "$arm" ;;
		scenes) scenes "$dir" "$arm" "$first" ;;
		desk)
			timeout 900 $M desktop "$dir/desk" > "$dir/desk.txt" 2>&1 || say "$arm FAIL desktop"
			sed "s/^/$arm desk /" "$dir/desk/table.txt" | tee -a "$summary"
			;;
		prof) prof "$dir" "$arm" ;;
		esac
	done
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
