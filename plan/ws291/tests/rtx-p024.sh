#!/bin/sh
# ws291-p024: kernels side by side on the RTX 2070 (10.0.10.3) with one tree's programs, each mview run,
# with TIMING=1 libvulkan's submission timing (VULKAN_ZEDBSD_SUBMIT_TIMING=1, about 2 us a part: subs-N is slower
# with it), run from the top of the tree in the machine's lock:
#   FILES='DEST=SRC ...' [RESTORE_CKSUM=N] [ROUNDS=1] [SPIN=8] [LIST=plan/ws291/tests/rtx-ab.list] [PERF=0|1]
#       [TIMING=0|1] [DMESG=REGEX] [SHOT=1] sh plan/ws291/tests/rtx-p024.sh OUT NAME:KERNEL ...
# The NVMe kernel is read back (checked against RESTORE_CKSUM), FILES put in (rtx-files.sh); then ROUNDS times, for
# each arm, its KERNEL put in (zedBSD restarted; with PERF=1 the driver's counts are on, nvrtx.vk.perf=1) and each
# LIST line (LABEL|OPTIONS) run as a game-mode mview of SPIN seconds: "NAME LABEL fps= stages", the VULKAN TIMING
# parts with TIMING=1, and the driver's perf lines when on (and the kernel lines DMESG matches, in OUT/NAME-ROUND/);
# with SHOT=1 then the three still views of 100 copies (one draw, a draw each, a submit each) captured by keiland-shot
# and compared with the first arm's first round (shot-diff.py).  At the end the counts are
# turned off, the files and the NVMe kernel put back, checked by cksum, and Debian again.
# OUT/summary.txt holds what is printed.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
out=${1:?usage: rtx-p024.sh OUT NAME:KERNEL ...}
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

# shots DIR ARM FIRST: the three still views of 100 copies, each compared with FIRST's.
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
		timeout 60 scp -O -i "$KEY" $SSHOPT "root@$ip:/tmp/shot.png" "$1/$view.png" > /dev/null 2>&1 || say "$2 FAIL $view shot"
		if [ "$1" != "$3" ] && [ -s "$3/$view.png" ] && [ -s "$1/$view.png" ]; then
			python3 -I plan/tools/frame-ops/shot-diff.py "$3/$view.png" "$1/$view.png" 2>&1 | sed "s/^/$2 $view vs first /" | tee -a "$summary"
		fi
	done
	[ -s "$1/one.png" ] && [ -s "$1/subs.png" ] &&
	    python3 -I plan/tools/frame-ops/shot-diff.py "$1/one.png" "$1/subs.png" 2>&1 | sed "s/^/$2 one-vs-subs /" | tee -a "$summary"
}

say "start $(date +%H:%M)"
ip=$(timeout 700 $M zedbsd 2> "$out/zedbsd.txt" | tail -1)
[ -n "$ip" ] || { say "FAIL zedBSD did not come up"; exit 1; }
files_restore_kernel "$out" || exit 1
failed=0
files_put "$out" || failed=1
perf_on=0
round=0
first=
while [ $failed = 0 ] && [ $round -lt "${ROUNDS:-1}" ]; do
	round=$((round + 1))
	for arm in "$@"; do
		[ $failed = 0 ] || break
		name=${arm%%:*}
		kernel=${arm#*:}
		dir=$out/$name-$round
		mkdir -p "$dir"
		timeout 700 $M deploy --kernel "$kernel" > "$dir/deploy.txt" 2>&1 || say "FAIL deploy $name"
		ip=$($M zedbsd 2> /dev/null | tail -1)
		[ -n "$ip" ] || { say "FAIL zedBSD did not come up ($name)"; failed=1; break; }
		if [ "${PERF:-0}" = 1 ] && [ $perf_on = 0 ]; then
			timeout 700 $M timing on > "$dir/timing-on.txt" 2>&1 || say "FAIL timing on"
			perf_on=1
			ip=$($M zedbsd 2> /dev/null | tail -1)
			[ -n "$ip" ] || { say "FAIL zedBSD did not come up ($name)"; failed=1; break; }
		fi
		say "$name round $round kernel $(cksum < "$kernel" | cut -d' ' -f1) on zedBSD $(z 'cksum /boot/boot0/vmunix' 2>&1 | cut -d' ' -f1)"
		envtiming=
		[ "${TIMING:-0}" = 1 ] && envtiming=VULKAN_ZEDBSD_SUBMIT_TIMING=1
		while IFS='|' read -r label opts; do
			case $label in ''|'#'*) continue ;; esac
			ZT=$((${SPIN:-8} + 60)) z 'U=$(awk -F: "\$3 == 1000 { print \$1; exit }" /etc/passwd)
n=$(dmesg | wc -l)
su $U -c "env XDG_RUNTIME_DIR=/run/user/1000 WAYLAND_DISPLAY=wayland-0 '"$envtiming"' timeout '$((${SPIN:-8} + 40))' /bin/mview --token=r --spin='"${SPIN:-8}"' --content=game --present=mailbox --images=4 '"$opts"'" 2>&1 |
    grep -E "MVIEW (SPIN|STAGES|FAILED)|VULKAN TIMING" | tail -6
dmesg | tail -n +$n | grep -E "perf: gpu|perf [0-9]|perf: frames|fault|lost" | tail -6
dmesg | tail -n +$n | grep -E "'"${DMESG:-^$}"'" | tail -40 > /tmp/p024-dmesg.txt' < /dev/null > "$dir/$label.txt" 2>&1
			[ -n "${DMESG:-}" ] && timeout 60 scp -O -i "$KEY" $SSHOPT "root@$ip:/tmp/p024-dmesg.txt" "$dir/$label-dmesg.txt" > /dev/null 2>&1
			fps=$(grep -o 'fps=[0-9.]*' "$dir/$label.txt" | head -1)
			stages=$(grep 'MVIEW STAGES' "$dir/$label.txt" | sed 's/.*frame=/frame=/' | head -1)
			timing=$(grep 'VULKAN TIMING' "$dir/$label.txt" | tail -1 | grep -oE ' (submit|execute|context_hold|private_fence|prepare|reserve|finish|reap|command_ran|command_deferred)=[0-9/]*' | tr '\n' ' ')
			say "$name $label $fps $stages"
			[ -n "$timing" ] && say "$name $label timing $timing"
			grep -E 'perf' "$dir/$label.txt" | sed "s/^/$name $label /" | tee -a "$summary"
		done < "$list"
		[ -n "$first" ] || first=$dir
		[ "${SHOT:-0}" = 1 ] && shots "$dir" "$name $round" "$first"
	done
done
if [ $perf_on = 1 ]; then
	timeout 700 $M timing off > "$out/timing-off.txt" 2>&1 || say "FAIL timing off"
fi
ip=$(timeout 700 $M zedbsd 2> /dev/null | tail -1)
if [ -n "$ip" ]; then
	files_back "$out"
else
	say "FAIL nothing put back: zedBSD did not answer"
fi
timeout 400 $M linux > "$out/linux.txt" 2>&1 || say "FAIL back to Debian"
say "debian $(timeout 60 ssh $SSHOPT awe@10.0.10.3 "uname -sr; lspci -k -d 10de: | grep -c 'in use: vfio-pci'" 2>&1 | tr '\n' ' ') (RTX functions on vfio-pci)"
say "end $(date +%H:%M)"
