#!/bin/sh
# ws291: the desktop's apps opened from App Home on the RTX 2070 (10.0.10.3), each arm a kernel and a tree's programs,
# run from the top of the tree in the machine's lock (flock /tmp/zedbsd-rdna.lock):
#   [RESTORE_CKSUM=N] [APPS='files terminal settings browser'] sh plan/ws291/tests/rtx-apps.sh OUT NAME:KERNEL:BUILD ...
# Each BUILD is a build of plan/ws291/tests/config.mk (rtx-files.sh's 14 programs and libraries).  The NVMe kernel is
# read back (OUT/nvme-vmunix, checked against RESTORE_CKSUM); then for each arm its files and KERNEL put in (zedBSD
# restarted) and each app of APPS opened from App Home (super, its name typed, enter), its process looked for 8 s
# later (/bin/APP among the processes), the screen captured by keiland-shot (OUT/NAME/app-APP.png, compared with the
# first arm's by shot-diff.py) and the app ended (its processes sent SIGTERM).  The kernel's lines of a lost device,
# a fault or a failed run are printed for each arm.  At the end the files and the NVMe kernel are put back, checked
# by cksum, and Debian again.  OUT/summary.txt holds what is printed.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
out=${1:?usage: rtx-apps.sh OUT NAME:KERNEL:BUILD ...}
shift
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

# apps DIR ARM FIRST: each app opened, looked for, captured and ended.
apps() {
	for app in ${APPS:-files terminal settings browser}; do
		r=$(ZT=120 z "[ -S /run/aat-input.sock ] || { aat-input start --width 1920 --height 1080 > /dev/null 2>&1; sleep 3; }
aat-input move-to 1900 1060 > /dev/null
aat-input key super > /dev/null; aat-input sleep 800 > /dev/null; aat-input type $app > /dev/null
aat-input sleep 500 > /dev/null; aat-input key enter > /dev/null; sleep 8
n=\$(ps -A -o args | grep -cE '^/bin/$app( |\$)')
rm -f /tmp/app.png
env XDG_RUNTIME_DIR=/run/user/1000 keiland-shot /tmp/app.png > /dev/null 2>&1 < /dev/null
for p in \$(ps -A -o pid,args | awk '\$2 == \"/bin/$app\" { print \$1 }'); do kill \$p; done
sleep 2
echo processes=\$n left=\$(ps -A -o args | grep -cE '^/bin/$app( |\$)') compositor=\$(ps -A -o args | grep -cE '^/bin/wayland')" 2>&1 | tail -1)
		say "$2 $app $r"
		timeout 60 scp -O -i "$KEY" $SSHOPT "root@$ip:/tmp/app.png" "$1/app-$app.png" > /dev/null 2>&1 ||
		    say "$2 FAIL $app shot"
		if [ "$2" != "$3" ] && [ -s "$out/$3/app-$app.png" ] && [ -s "$1/app-$app.png" ]; then
			python3 -I plan/tools/frame-ops/shot-diff.py "$out/$3/app-$app.png" "$1/app-$app.png" \
			    "$1/app-$app-diff.png" 2>&1 | sed "s/^/$2 $app vs $3 /" | tee -a "$summary"
		fi
	done
}

say "start $(date +%H:%M)"
ip=$(timeout 700 $M zedbsd 2> "$out/zedbsd.txt" | tail -1)
[ -n "$ip" ] || { say "FAIL zedBSD did not come up"; exit 1; }
files_restore_kernel "$out" || exit 1
first=
for spec in "$@"; do
	arm=${spec%%:*}; rest=${spec#*:}
	kernel=${rest%%:*}; build=${rest#*:}
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
	ip=$(timeout 700 $M zedbsd 2> /dev/null | tail -1)
	[ -n "$ip" ] || { say "$arm FAIL zedBSD did not come up"; break; }
	say "$arm kernel $(z 'cksum /boot/boot0/vmunix' 2>&1 | cut -d' ' -f1) libvulkan $(z 'cksum /lib/libvulkan.so' 2>&1 | cut -d' ' -f1) wayland $(z 'cksum /bin/wayland' 2>&1 | cut -d' ' -f1) $(date +%H:%M)"
	# The session's desktop up before the first app.
	sleep 20
	apps "$dir" "$arm" "$first"
	z "dmesg | grep -cE 'lost|fault|executor fail|renderer faulted|refused'; dmesg | grep -E 'lost|fault|executor fail|renderer faulted|refused' | tail -6" 2>&1 |
	    sed "s/^/$arm kernel /" | tee -a "$summary"
done
FILES=$(files_of "${build:-}")
ip=$(timeout 700 $M zedbsd 2> /dev/null | tail -1)
if [ -n "$ip" ]; then
	files_back "$out"
else
	say "FAIL nothing put back: zedBSD did not answer"
fi
timeout 400 $M linux > "$out/linux.txt" 2>&1 || say "FAIL back to Debian"
say "debian $(timeout 60 ssh $SSHOPT awe@10.0.10.3 "uname -sr; lspci -k -d 10de: | grep -c 'in use: vfio-pci'" 2>&1 | tr '\n' ' ') (RTX functions on vfio-pci)"
say "end $(date +%H:%M)"
