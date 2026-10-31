#!/bin/sh
# ws291-p025: the Raspberry Pi 4's desktop frames (frame-ops, as the RTX and RDNA runs) with one kernel, run from the top
# of the tree on a board running zedBSD with root's SSH key of the guest harness (plan/tmp/guest/id_ed25519):
#   [PI=ADDRESS] KERNEL=VMUNIX AAT=AAT-INPUT [RUNS=1] [CONSOLE=1] sh plan/ws291/tests/pi4-desk.sh OUT
# KERNEL is a build of plan/ws291/tests/config-rpi4-desk.mk (the test input injection), AAT its bin/aat-input.  The
# board gets KERNEL as /boot/boot0/vmunix, AAT as /bin/aat-input, /etc/keiland/session with the compositor's frame
# log (plan/ws291/tests/session.sh) and the graphical login (cmdline.txt login=graphical, the board's autologin
# account), each first file kept once as NAME.p025old; then, RUNS times, the board is rebooted, the autologin
# session waited for and plan/tools/frame-ops/frame-ops.sh run in it (OUT/deskN: session.log, marks, the table of
# plan/tools/frame-ops/frame-times.py and every frame).  At the end the console login and the board's own session file
# are put back (they take effect at the next boot; KERNEL and /bin/aat-input stay).  CONSOLE=1 keeps the console
# login and runs the compositor as root on its own socket with the session's options (--glass, the wallpaper,
# --log-frames) and its default desktop client instead of the graphical session, whose compositor waits for the
# display's images forever at 1920x1080 (seen 2026-10-31 with the main kernel and this one).  OUT/summary.txt holds what is printed.
# LIBS='NAME=PATH ...' puts shared libraries in /lib first, BINS='NAME=PATH ...' programs in /bin (see below).
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
out=${1:?usage: pi4-desk.sh OUT}
PI=${PI:-10.0.30.6}
: "${KERNEL:?KERNEL=the kernel}"
: "${AAT:?AAT=aat-input}"
KEY=plan/tmp/guest/id_ed25519
SO="-i $KEY -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR -o ConnectTimeout=8 -o BatchMode=yes"
SESSION=/run/user/1000/session.log
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
login=graphical
[ "${CONSOLE:-0}" = 1 ] && login=console
b 'cd /boot/boot0 && for f in vmunix cmdline.txt; do test -e $f.p025old || cp $f $f.p025old; done; test -e /etc/keiland/session.p025old || cp /etc/keiland/session /etc/keiland/session.p025old'
want=$(cksum < "$KERNEL" | cut -d' ' -f1)
timeout 300 scp $SO -q "$KERNEL" "root@$PI:/boot/boot0/vmunix.new" &&
    timeout 120 scp $SO -q "$AAT" "root@$PI:/bin/aat-input" &&
    timeout 120 scp $SO -q plan/ws291/tests/session.sh "root@$PI:/etc/keiland/session" &&
    b 'test "$(cksum < /boot/boot0/vmunix.new | cut -d" " -f1)" = '"$want"' && mv /boot/boot0/vmunix.new /boot/boot0/vmunix && chmod 755 /bin/aat-input && chmod 644 /etc/keiland/session && echo "login='"$login"' v3d.perf=1" > /boot/boot0/cmdline.txt && sync' ||
    { say "FAIL the files were not put in"; exit 1; }

# LIBS='NAME=PATH ...': each PATH becomes the board's /lib/NAME (put beside and renamed over it, the first kept once as
# /lib/NAME.p025old); they stay after the run.
for pair in ${LIBS:-}; do
	name=${pair%%=*}
	path=${pair#*=}
	timeout 120 scp $SO -q "$path" "root@$PI:/lib/$name.new" &&
	    b "test \"\$(cksum < /lib/$name.new | cut -d' ' -f1)\" = $(cksum < "$path" | cut -d' ' -f1) && { test -e /lib/$name.p025old || cp /lib/$name /lib/$name.p025old; } && chmod 755 /lib/$name.new && mv /lib/$name.new /lib/$name && sync" ||
	    { say "FAIL /lib/$name was not put in"; exit 1; }
	say "lib $name $(b "cksum < /lib/$name" | cut -d' ' -f1)"
done

# BINS='NAME=PATH ...': the same for the board's /bin/NAME (the compositor, the Terminal, Files of a tree's build).
for pair in ${BINS:-}; do
	name=${pair%%=*}
	path=${pair#*=}
	timeout 120 scp $SO -q "$path" "root@$PI:/bin/$name.new" &&
	    b "test \"\$(cksum < /bin/$name.new | cut -d' ' -f1)\" = $(cksum < "$path" | cut -d' ' -f1) && { test -e /bin/$name.p025old || cp /bin/$name /bin/$name.p025old; } && chmod 755 /bin/$name.new && mv /bin/$name.new /bin/$name && sync" ||
	    { say "FAIL /bin/$name was not put in"; exit 1; }
	say "bin $name $(b "cksum < /bin/$name" | cut -d' ' -f1)"
done

i=1
while [ $i -le "${RUNS:-1}" ]; do
	d=$out/desk$i
	mkdir -p "$d"
	b 'sync; (sleep 1; /sbin/reboot) > /dev/null 2>&1 &'
	sleep 20
	wait_up || { say "FAIL the board did not come back after the reboot"; break; }
	# CONSOLE=1: the compositor as root, its log in place of the session's.
	if [ "${CONSOLE:-0}" = 1 ]; then
		SESSION=/root/p025desk/session.log
		BT=120 b 'sysctl hw.gpu.start=1' > /dev/null 2>&1
		b "D=/root/p025desk; mkdir -p \$D/run; chmod 700 \$D/run; cd \$D; XDG_RUNTIME_DIR=\$D/run HOME=/root nohup /bin/wayland --glass --log-frames --wallpaper=/usr/share/keiland/wallpaper.png --socket=\$D/run/wayland-0 > \$D/session.log 2>&1 < /dev/null &" > /dev/null 2>&1
	fi

	# The session: its log with frame lines.
	n=0
	while [ $n -lt 30 ] && ! b "grep -q 'KWL LAT draw' $SESSION" 2> /dev/null; do
		sleep 3
		n=$((n + 1))
	done
	say "desk$i board vmunix $(b 'cksum < /boot/boot0/vmunix' | cut -d' ' -f1) session $(b "wc -l < $SESSION" 2>&1)"
	sleep 10
	timeout 120 scp $SO -q plan/tools/frame-ops/frame-ops.sh "root@$PI:/tmp/frame-ops.sh"
	BT=400 b "sh /tmp/frame-ops.sh $SESSION /tmp/marks 1920 1080 > /tmp/frame-ops.out 2>&1 < /dev/null" > /dev/null 2>&1
	timeout 120 scp $SO -q "root@$PI:$SESSION" "root@$PI:/tmp/marks" "root@$PI:/tmp/frame-ops.out" "$d/" || say "desk$i FAIL nothing collected"
	b 'dmesg' > "$d/dmesg.txt" 2>&1
	python3 -I plan/tools/frame-ops/frame-times.py "$d/session.log" "$d/marks" > "$d/table.txt" 2>&1
	python3 -I plan/tools/frame-ops/frame-times.py "$d/session.log" "$d/marks" --frames > "$d/frames.txt" 2>&1
	sed "s/^/desk$i /" "$d/table.txt" | tee -a "$summary"
	say "desk$i kernel faults $(grep -ciE 'fault|lost|hang|panic|job failed|EIO' "$d/dmesg.txt")"
	i=$((i + 1))
done

# The console login and the board's session file again, from the next boot.
b 'cp /etc/keiland/session.p025old /etc/keiland/session && cp /boot/boot0/cmdline.txt.p025old /boot/boot0/cmdline.txt && echo "login=console v3d.perf=1" > /boot/boot0/cmdline.txt && sync'
say "end $(date +%H:%M:%S) cmdline $(b 'cat /boot/boot0/cmdline.txt')"
