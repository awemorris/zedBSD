#!/bin/sh
# ws291-p021: mview's scene on the Raspberry Pi 5's V3D 7.1, on the board booted from the measurement image
# (rpi5-image.sh, rpi5-image-boot.sh), from the top of the tree:
#   [PI5=ADDRESS] [KERNEL=VMUNIX] [COPIES='1 10 50 74'] [SPIN=12] [MVIEW_ARGS=...] [PROGRAMS=BUILD] sh plan/ws291/tests/rpi5-mview.sh OUT
# The board is found by its Ethernet MAC (98:fe:54:22:65:c3) on 10.0.30.0/24 unless PI5 names it, and must answer
# uname with zedBSD; root is reached with the guest harness's key (plan/tmp/guest/id_ed25519).  The GPU is started
# when it is still held (sysctl hw.gpu.start=1) and its stage lines (QPUs, clock) kept; the compositor runs on its own
# socket with no desktop client, mview spins in the game mode (fullscreen, direct scanout, mailbox, 4 images) once for
# each of COPIES, the compositor's scanout lines and the kernel's GPU lines are kept, and the compositor is ended.
# PROGRAMS names a build of the same config whose bin/wayland, bin/mview and dynamic libraries are copied to
# /root/p021 and run with LD_LIBRARY_PATH (the image's own files are not changed).  KERNEL, when its checksum differs
# from the board's /boot/boot0/vmunix, is copied there (the board's first kernel kept once as vmunix.p021old) and the
# board rebooted; nothing is power-cycled.  Each run's summary keeps the kernel's last cost report and its board line
# (the CPU clock as set and measured, the temperature and the firmware's throttling flags, with v3d.perf=1), so runs
# at different CPU speeds can be told apart.  OUT/summary.txt holds what is printed.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
out=${1:?usage: rpi5-mview.sh OUT}
MAC=98:fe:54:22:65:c3
KEY=plan/tmp/guest/id_ed25519
SO="-i $KEY -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR -o ConnectTimeout=8 -o BatchMode=yes"
mkdir -p "$out"
summary=$out/summary.txt
: > "$summary"

say() {
	echo "$*" | tee -a "$summary"
}

find_board() {
	ip=$(ip neigh | awk -v mac="$MAC" '$0 ~ mac && $0 !~ /FAILED/ && $1 ~ /^[0-9.]+$/ {print $1; exit}')
	if [ -z "$ip" ]; then
		i=2
		while [ $i -lt 255 ]; do
			ping -c 1 -W 1 "10.0.30.$i" > /dev/null 2>&1 &
			i=$((i + 1))
		done
		wait
		ip=$(ip neigh | awk -v mac="$MAC" '$0 ~ mac && $0 !~ /FAILED/ && $1 ~ /^[0-9.]+$/ {print $1; exit}')
	fi
	echo "$ip"
}

b() {
	timeout "${BT:-60}" ssh $SO "root@$PI5" "$@"
}

say "start $(date +%H:%M)"
[ -n "${PI5:-}" ] || PI5=$(find_board)
[ -n "$PI5" ] || { say "FAIL the board's MAC $MAC is not on 10.0.30.0/24"; exit 1; }
os=$(b 'uname -sm' 2>&1)
case $os in
zedBSD*) ;;
*) say "FAIL $PI5 is not zedBSD: $os"; exit 1 ;;
esac
say "board $PI5 $os"

# Waits up to four minutes for the board's SSH after a reboot.
wait_up() {
	n=0
	while [ $n -lt 48 ]; do
		sleep 5
		if b 'uname -s' 2> /dev/null | grep -q zedBSD; then
			return 0
		fi
		n=$((n + 1))
	done
	return 1
}

# The kernel, with a reboot when it changes.
if [ -n "${KERNEL:-}" ]; then
	want=$(cksum < "$KERNEL" | cut -d' ' -f1)
	have=$(b 'cksum < /boot/boot0/vmunix' | cut -d' ' -f1)
	if [ "$want" != "$have" ]; then
		b 'test -e /boot/boot0/vmunix.p021old || cp /boot/boot0/vmunix /boot/boot0/vmunix.p021old'
		timeout 300 scp $SO -q "$KERNEL" "root@$PI5:/boot/boot0/vmunix.new" &&
		    b 'test "$(cksum < /boot/boot0/vmunix.new | cut -d" " -f1)" = '"$want"' && mv /boot/boot0/vmunix.new /boot/boot0/vmunix && sync' ||
		    { say "FAIL the kernel was not copied"; exit 1; }
		b 'sync; (sleep 1; /sbin/reboot) > /dev/null 2>&1 &'
		sleep 20
		wait_up || { say "FAIL the board did not come back after the reboot"; exit 1; }
		say "rebooted $(date +%H:%M:%S)"
	fi
fi
say "kernel vmunix $(b 'cksum < /boot/boot0/vmunix' | cut -d' ' -f1) $(b 'uname -v' 2>&1) cmdline $(b 'cat /boot/boot0/cmdline.txt' 2>&1)"

# The GPU, started once per boot.
if b 'dmesg' | grep -q 'held; root starts it'; then
	if ! b 'dmesg' | grep -q 'V1 ok'; then
		BT=120 b 'sysctl hw.gpu.start=1' > "$out/start.txt" 2>&1
		say "started: $(tr '\n' ' ' < "$out/start.txt")"
	fi
fi
b 'dmesg' | grep -E 'v3d: V[0-9]|display: (P|R)[0-9]' > "$out/stages.txt"
grep -E 'V1 ok|V2 version|clock' "$out/stages.txt" | head -6 | sed 's/^/gpu /' | tee -a "$summary"
say "nodes $(b 'ls /dev/gpu*' 2>&1 | tr '\n' ' ')"

# The programs: the image's own, or PROGRAMS's copied to /root/p021.
D=/root/p021
ENV="XDG_RUNTIME_DIR=$D/run"
WAYLAND=/bin/wayland
MVIEW=/bin/mview
b "mkdir -p $D/run $D/bin $D/lib && chmod 700 $D/run"
if [ -n "${PROGRAMS:-}" ]; then
	timeout 300 scp $SO -q "$PROGRAMS/bin/wayland" "$PROGRAMS/bin/mview" "root@$PI5:$D/bin/" &&
	    timeout 300 scp $SO -q "$PROGRAMS"/dynamic/*.so "root@$PI5:$D/lib/" || say "FAIL the programs were not copied"
	ENV="$ENV LD_LIBRARY_PATH=$D/lib"
	WAYLAND=$D/bin/wayland
	MVIEW=$D/bin/mview
	say "programs $(b "cksum $WAYLAND $MVIEW $D/lib/libvulkan.so" 2>&1 | awk '{print $1}' | tr '\n' ' ')"
fi

# The compositor on its own socket (a stale socket of an earlier run removed first).
b "rm -f $D/run/wayland-0; env $ENV nohup $WAYLAND --socket=$D/run/wayland-0 --desktop-client=none > $D/wayland.log 2>&1 < /dev/null & echo \$! > $D/wayland.pid; sleep 8; test -S $D/run/wayland-0 && grep -q 'step=socket' $D/wayland.log && echo compositor up || { echo compositor down; tail -5 $D/wayland.log; }" > "$out/compositor.txt" 2>&1
say "$(tr '\n' ' ' < "$out/compositor.txt")"

for copies in ${COPIES:-1 10 50 74}; do
	b 'dmesg' > "$out/c$copies.before" 2>&1
	BT=$((${SPIN:-12} + 120)) b "env $ENV $MVIEW --display=$D/run/wayland-0 --token=c$copies --spin=${SPIN:-12} --copies=$copies --content=game --present=mailbox --images=4 ${MVIEW_ARGS:-}" > "$out/c$copies.txt" 2>&1
	say "c$copies $(grep -E 'MVIEW (SPIN|STAGES|FAILED|HITCHES)' "$out/c$copies.txt" | sed 's/frames=[0-9]* seconds=[0-9.]* //' | tr '\n' ' ')"
	b 'dmesg' > "$out/c$copies.after" 2>&1
	grep -F -x -v -f "$out/c$copies.before" "$out/c$copies.after" > "$out/c$copies.kernel"
	grep -vE 'v3d: (perf|gpu|board)|display: perf|flip report' "$out/c$copies.kernel" | head -6 | sed "s/^/c$copies kernel /" | tee -a "$summary"
	grep -E 'v3d: perf' "$out/c$copies.kernel" | tail -1 | sed "s/^/c$copies perf /" | tee -a "$summary"
	grep -E 'v3d: board' "$out/c$copies.kernel" | tail -1 | sed "s/^/c$copies /" | tee -a "$summary"
	say "c$copies board-range $(grep -E 'v3d: board' "$out/c$copies.kernel" | sed -n 's/.*arm=[0-9]*\/\([0-9]*\) .*temp=\([0-9.]*\) throttled=\(0x[0-9a-f]*\).*/\1 \2 \3/p' | sort -u | tr '\n' ',')"
done
b "grep -E 'KWL (SCANOUT|MODE [a-z]|FAILED|VULKAN_ERROR|DIRECT|OUTPUT)' $D/wayland.log | sort | uniq -c | head -12" > "$out/scanout.txt" 2>&1
sed 's/^/compositor /' "$out/scanout.txt" | tee -a "$summary"
b "kill \$(cat $D/wayland.pid); sleep 1; dmesg | grep -iE 'v3d|hvs|fault|lost|hang' | tail -8" > "$out/kernel.txt" 2>&1
sed 's/^/kernel /' "$out/kernel.txt" | tee -a "$summary"
say "end $(date +%H:%M)"
