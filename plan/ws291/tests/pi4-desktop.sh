#!/bin/sh
# ws291-p025: the Raspberry Pi 4's desktop with one kernel, for the V3D driver's regression, run from the top of the
# tree on a board running zedBSD with root's SSH key of the guest harness (plan/tmp/guest/id_ed25519) and the console
# login (no session holds the display):
#   [PI=ADDRESS] KERNEL=VMUNIX PROGRAMS=BUILD sh plan/ws291/tests/pi4-desktop.sh OUT
# KERNEL, when its checksum differs from the board's /boot/boot0/vmunix, is copied there and the board rebooted (the
# board's first kernel is kept once as vmunix.p025old).  PROGRAMS is a build of plan/ws291/tests/config-rpi4-shot.mk:
# its bin/wayland, bin/files, bin/terminal, bin/keiland-shot and dynamic libraries go to /root/p025d and run with
# LD_LIBRARY_PATH.  The compositor runs on its own socket with Files' desktop, a Terminal opens after it, and
# keiland-shot takes the screen (OUT/desk.png).  The compositor's errors and the kernel's new fault lines are counted.
# Running it with two kernels and the same PROGRAMS gives two pictures to compare.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
out=${1:?usage: pi4-desktop.sh OUT}
PI=${PI:-10.0.30.6}
: "${KERNEL:?KERNEL=the kernel}"
: "${PROGRAMS:?PROGRAMS=the build}"
KEY=plan/tmp/guest/id_ed25519
SO="-i $KEY -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR -o ConnectTimeout=8 -o BatchMode=yes"
mkdir -p "$out"
summary=$out/summary.txt
: > "$summary"

say() {
	echo "$*" | tee -a "$summary"
}

b() {
	timeout "${BT:-60}" ssh $SO "root@$PI" "$@"
}

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

say "start $(date +%H:%M:%S)"
want=$(cksum < "$KERNEL" | cut -d' ' -f1)
have=$(b 'cksum < /boot/boot0/vmunix' | cut -d' ' -f1)
if [ "$want" != "$have" ]; then
	b 'test -e /boot/boot0/vmunix.p025old || cp /boot/boot0/vmunix /boot/boot0/vmunix.p025old'
	timeout 300 scp $SO -q "$KERNEL" "root@$PI:/boot/boot0/vmunix.new" &&
	    b 'test "$(cksum < /boot/boot0/vmunix.new | cut -d" " -f1)" = '"$want"' && mv /boot/boot0/vmunix.new /boot/boot0/vmunix && sync' ||
	    { say "FAIL the kernel was not copied"; exit 1; }
	b 'sync; (sleep 1; /sbin/reboot) > /dev/null 2>&1 &'
	sleep 20
	wait_up || { say "FAIL the board did not come back after the reboot"; exit 1; }
	say "rebooted $(date +%H:%M:%S)"
fi
say "board $PI vmunix $(b 'cksum < /boot/boot0/vmunix' | cut -d' ' -f1) cmdline $(b 'cat /boot/boot0/cmdline.txt')"
if b 'dmesg' | grep -q 'held; root starts it'; then
	b 'dmesg' | grep -q 'v3d: V1 ' || BT=120 b 'sysctl hw.gpu.start=1' > /dev/null 2>&1
fi

D=/root/p025d
b "mkdir -p $D/run $D/bin $D/lib && chmod 700 $D/run"
timeout 300 scp $SO -q "$PROGRAMS/bin/wayland" "$PROGRAMS/bin/files" "$PROGRAMS/bin/terminal" "$PROGRAMS/bin/keiland-shot" "root@$PI:$D/bin/" &&
    timeout 300 scp $SO -q "$PROGRAMS"/dynamic/*.so "root@$PI:$D/lib/" || say "FAIL the programs were not copied"
b 'dmesg' > "$out/before.txt" 2>&1
BT=150 b "cd $D && export XDG_RUNTIME_DIR=$D/run LD_LIBRARY_PATH=$D/lib HOME=/root
rm -f run/wayland-0 run/keiland-shot.sock
$D/bin/wayland --socket=$D/run/wayland-0 \"--desktop-client=$D/bin/files --desktop\" > wayland.log 2>&1 < /dev/null &
compositor=\$!
sleep 12
WAYLAND_DISPLAY=wayland-0 $D/bin/terminal > terminal.log 2>&1 < /dev/null &
terminal=\$!
sleep 10
timeout 60 $D/bin/keiland-shot --socket $D/run/keiland-shot.sock $D/desk.png
kill \$terminal; pkill -f $D/bin/files; kill \$compositor; sleep 2" > "$out/run.txt" 2>&1
timeout 120 scp $SO -q "root@$PI:$D/desk.png" "root@$PI:$D/wayland.log" "$out/" || say "FAIL nothing was collected"
b 'dmesg' > "$out/after.txt" 2>&1
grep -F -x -v -f "$out/before.txt" "$out/after.txt" > "$out/kernel.txt"
say "shot $(grep KEILAND-SHOT "$out/run.txt" | tr '\n' ' ')"
say "compositor errors $(grep -cE 'KWL (FAILED|VULKAN_ERROR|MODE_ERROR)' "$out/wayland.log") desktop starts $(grep -c 'KWL DESKTOP start' "$out/wayland.log")"
say "kernel faults $(grep -ciE 'fault|lost|hang|panic|abort|job failed|EIO' "$out/kernel.txt")"
grep -iE 'fault|lost|hang|panic|abort|job failed|EIO' "$out/kernel.txt" | head -4 | sed 's/^/kernel /' | tee -a "$summary"
say "end $(date +%H:%M:%S)"
