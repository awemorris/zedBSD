#!/bin/sh
# gpu-perf: the measurements of a GPU test machine in one command each, run from the host at the top of the tree.
# The machine (GPU_PERF_MACHINE, machines/NAME.conf, default rdna) runs Debian as its normal system and starts zedBSD
# once with UEFI BootNext; zedBSD gets its address by DHCP on the LAN adapter Debian uses too (found by its MAC).  The
# driver's own parts (its timing marks, its control program) come from the machine's driver (drivers/NAME.conf).
# Every step has a time limit; the long logs stay in OUT, the command prints only the summary.
#
#     measure.sh where                       which system runs: "zedbsd IP", "linux" or "none"
#     measure.sh zedbsd                      zedBSD running (BootNext from Debian when Debian runs); prints its IP
#     measure.sh linux                       Debian running (zedBSD rebooted: BootOrder brings Debian back)
#     measure.sh nicwatch                    puts nicwatch/ on the running zedBSD (reboots itself when its USB LAN
#                                            adapter gets no address; to_zedbsd then boots it again, twice at most)
#     measure.sh timing on|off               the driver's timing marks on the boot line (DRIVER_TIMING_KEY, on: every
#                                            DRIVER_TIMING_VALUE ms) or
#                                            not, zedBSD restarted (via Debian); the marks time every command and
#                                            cost the command-heavy runs several ms a frame, so bench runs go
#                                            without them and profile runs with them
#     measure.sh cfg NAME [VALUE]            the boot line NAME=VALUE in zedbsd.cfg (no VALUE: the line taken out),
#                                            zedBSD restarted (via Debian)
#     measure.sh deploy [--kernel VMUNIX] [DEST=SRC]...
#                                            files copied into the running zedBSD (the kernel to /boot/boot0/vmunix,
#                                            the old one kept as vmunix.old), then zedBSD restarted (via Debian)
#     measure.sh desktop [--keep] OUT        frame-ops on a fresh session (zedBSD restarted first; --keep: the
#                                            running one, whose open windows change the steps): OUT/table.txt
#                                            (printed), session.log, marks, frame-ops.out, shot.png
#     measure.sh mview OUT [MVIEW-OPTION]... mview in the session (its user): its MVIEW lines and the
#                                            compositor's KWL SCANOUT lines of the run (printed), OUT/mview.log
#     measure.sh linux-mview OUT [MVIEW-OPTION]...
#                                            the Linux reference: mview (linux/build-mview.sh, copied to ~/linux-mview)
#                                            on weston's kiosk shell on the DRM backend (seatd, started when not
#                                            running), its MVIEW lines and weston's composition per repaint
#                                            ("plane-only" is the direct scanout) printed, OUT/mview.log
#                                            (LINUX_ENV: variables set for mview, as RADV_DEBUG=nongg)
#     measure.sh profile OUT [MVIEW-OPTION]...
#                                            mview in the session with libvulkan's submission timing
#                                            (VULKAN_ZEDBSD_SUBMIT_TIMING) and the driver's timing marks of the run
#                                            (the boot line must carry DRIVER_TIMING_KEY=N): the MVIEW, VULKAN
#                                            TIMING and vk timing lines (printed), OUT/mview.log, OUT/kernel.log
#     measure.sh shot OUT COPIES             mview's still view of COPIES copies (FIFO, composed: the compositor is
#                                            not active while a game holds the display) captured by
#                                            keiland-shot 6 s in: OUT/shot.png, compared with the Linux reference
#                                            SHOT_REFERENCE/linux-shot-COPIES.png (printed: shot-diff.py and the
#                                            pixels 16 or more levels apart)
#     measure.sh hang OUT                    the hang check: vkhang (a draw that never ends: its app alone loses the
#                                            device, the queue is reset), then vkhang --loops, vkhang --compute --loops
#                                            and a short mview run, and the desktop captured: their lines and the
#                                            kernel's lines of the loss and the reset (printed), OUT/desktop.png
#     measure.sh nggshape OUT                the vertex stage's draw shapes (nggshape of plan/ws291/tests) one by one, each
#                                            waited for 10 s at most: their NGGSHAPE lines (printed), OUT/nggshape.log
#     measure.sh logo OUT                    the console's surface between games (a driver with DRIVER_CTL): its
#                                            watch reads the plane's
#                                            surface every 2 ms while mview takes the display over and gives it back
#                                            (game and window mode, gaps under and over the console's 3 s rest) and
#                                            vkhang loses its device, then while the compositor is ended (zedBSD is
#                                            left without its desktop: restart it after); the first runs' pictures
#                                            and the screen past the console's rest are read; printed: the watch
#                                            lines, the pixels not black, the kernel's console and lease lines
#     measure.sh bench zedbsd|linux OUT [LIST]
#                                            every run of LIST (default bench.list beside this script) on the one
#                                            system, mview copied first (USB_MVIEW, LINUX_MVIEW); OUT/bench.log and
#                                            the table of fps and the frame's stages (printed, OUT/table.txt)
#
# The programs copied in: USB_MVIEW, USB_NGGSHAPE, USB_VKHANG, LINUX_MVIEW (the machine's file gives them).
# The machine's file: ZEDBSD_MAC, ZEDBSD_SUBNET (a /24, as 10.0.30), ZEDBSD_BOOT (the UEFI entry), DEBIAN (user@host),
# PERF_BUILD; the environment overrides each.  KEY (the zedBSD root key), ZEDBSD_ENV (variables set for zedBSD's mview
# in mview, profile and bench, as LINUX_ENV is for Linux's).
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u

top=$(pwd)
. plan/tools/gpu-perf/conf.sh
KEY=${KEY:-$top/plan/tmp/guest/id_ed25519}
SSHOPT="-o BatchMode=yes -o ConnectTimeout=8 -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR"
# The commands to zedBSD share one SSH connection, used again and again (its sshd leaves sessions stuck now and then,
# and enough stuck ones make it refuse every new connection: BUG-346; 2026-10-27 user: one SSH session reused).
mkdir -p "$top/build/tmp"
ZSSHOPT="$SSHOPT -o ControlMaster=auto -o ControlPath=$top/build/tmp/ssh-%h -o ControlPersist=600"
SESSION=/run/user/1000/session.log
# The session's user on the target, as target shell text: the name of uid 1000 (user, or kei in an image made before
# the OS name became zedBSD).
SESSION_USER='$(awk -F: '"'"'$3 == 1000 { print $1; exit }'"'"' /etc/passwd)'

say() {
	echo "measure: $*" >&2
}

# zs IP COMMAND: a command on zedBSD as root, at most ZS_TIMEOUT seconds.
zs() {
	ip=$1
	shift
	timeout "${ZS_TIMEOUT:-60}" ssh -i "$KEY" $ZSSHOPT "root@$ip" "$@"
}

# zcp: scp with the zedBSD key (its SFTP mode); a copy that failed is tried again, twice.
zcp() {
	for try in 1 2 3; do
		timeout 120 scp -i "$KEY" $ZSSHOPT "$@" && return 0
		sleep 5
	done
	return 1
}

# zget IP DIR REMOTE...: each remote file into DIR, read with cat over SSH (an scp from zedBSD sometimes hangs), each
# tried again twice.
zget() {
	gip=$1
	gdir=$2
	shift 2
	for gfile in "$@"; do
		gok=0
		for try in 1 2 3; do
			ZS_TIMEOUT=60 zs "$gip" "cat $gfile" > "$gdir/$(basename "$gfile")" < /dev/null && { gok=1; break; }
			sleep 5
		done
		[ $gok = 1 ] || return 1
	done
	return 0
}

deb() {
	timeout "${ZS_TIMEOUT:-60}" ssh $SSHOPT "$DEBIAN" "$@"
}

# The zedBSD's address from the neighbour table, with a sweep of the subnet when it is not there.
zedbsd_ip() {
	ip=$(ip neigh | awk -v mac="$ZEDBSD_MAC" '$0 ~ mac && $0 !~ /FAILED/ && $1 ~ /^[0-9.]+$/ {print $1; exit}')
	if [ -z "$ip" ]; then
		i=2
		while [ $i -lt 255 ]; do
			ping -c 1 -W 1 "$ZEDBSD_SUBNET.$i" > /dev/null 2>&1 &
			i=$((i + 1))
		done
		wait
		ip=$(ip neigh | awk -v mac="$ZEDBSD_MAC" '$0 ~ mac && $0 !~ /FAILED/ && $1 ~ /^[0-9.]+$/ {print $1; exit}')
	fi
	[ -n "$ip" ] || return 1
	name=$(ZS_TIMEOUT=15 zs "$ip" uname -s 2>/dev/null)
	[ "$name" = zedBSD ] || return 1
	echo "$ip"
}

linux_up() {
	name=$(ZS_TIMEOUT=15 deb uname -s 2>/dev/null)
	[ "$name" = Linux ]
}

where() {
	ip=$(zedbsd_ip) && { echo "zedbsd $ip"; return 0; }
	linux_up && { echo linux; return 0; }
	echo none
}

# wait_for WHAT SECONDS: until the system answers.
wait_for() {
	end=$(($(date +%s) + $2))
	while [ "$(date +%s)" -lt $end ]; do
		if [ "$1" = zedbsd ]; then
			ip=$(zedbsd_ip) && { echo "$ip"; return 0; }
		elif linux_up; then
			return 0
		fi
		sleep 5
	done
	say "$1 did not come up in $2 s"
	return 1
}

to_zedbsd() {
	ip=$(zedbsd_ip) && { echo "$ip"; return 0; }
	linux_up || { say "neither system answers"; return 1; }
	say "Debian: BootNext $ZEDBSD_BOOT, reboot"
	deb "sudo efibootmgr -q -n $ZEDBSD_BOOT && (sleep 1; sudo systemctl reboot) > /dev/null 2>&1 &" || return 1
	sleep 30
	# A zedBSD whose USB LAN adapter got no link reboots by itself into Debian (nicwatch); BootNext again, twice at most.
	tries=0
	until wait_for zedbsd 300 > /dev/null; do
		tries=$((tries + 1))
		[ $tries -le 2 ] && linux_up || return 1
		say "Debian came back (zedBSD had no link): BootNext again"
		deb "sudo efibootmgr -q -n $ZEDBSD_BOOT && (sleep 1; sudo systemctl reboot) > /dev/null 2>&1 &" || return 1
		sleep 30
	done
	ip=$(zedbsd_ip)
	# The session's compositor starts after the network; wait for its log.
	end=$(($(date +%s) + 120))
	while [ "$(date +%s)" -lt $end ]; do
		zs "$ip" "test -S /run/user/1000/wayland-0 && test -f $SESSION" && break
		sleep 3
	done
	echo "$ip"
}

to_linux() {
	linux_up && return 0
	ip=$(zedbsd_ip) || { say "neither system answers"; return 1; }
	say "zedBSD $ip: reboot"
	ZS_TIMEOUT=10 zs "$ip" "(sleep 1; reboot) > /dev/null 2>&1 &"
	sleep 30
	wait_for linux 300
}

deploy() {
	ip=$(to_zedbsd) || return 1
	if [ "${1:-}" = --kernel ]; then
		zcp "$2" "root@$ip:/tmp/vmunix.new" || return 1
		zs "$ip" "cp /boot/boot0/vmunix /boot/boot0/vmunix.old && cp /tmp/vmunix.new /boot/boot0/vmunix && sync" || return 1
		say "kernel $(sha256sum "$2" | cut -c1-16) put in"
		shift 2
	fi
	for pair in "$@"; do
		dest=${pair%%=*}
		src=${pair#*=}
		zcp "$src" "root@$ip:/tmp/deploy.file" || return 1
		zs "$ip" "cp /tmp/deploy.file '$dest' && sync" || return 1
		say "$dest put in"
	done
	to_linux || return 1
	to_zedbsd
}

timing() {
	ip=$(to_zedbsd) || return 1
	if [ "$1" = on ]; then
		zs "$ip" "sed -i -e '/^#*$DRIVER_TIMING_KEY=/d' /boot/boot0/zedbsd.cfg && echo $DRIVER_TIMING_KEY=$DRIVER_TIMING_VALUE >> /boot/boot0/zedbsd.cfg && sync" || return 1
	else
		zs "$ip" "sed -i -e '/^$DRIVER_TIMING_KEY=/d' /boot/boot0/zedbsd.cfg && sync" || return 1
	fi
	zs "$ip" "grep -c '^$DRIVER_TIMING_KEY=' /boot/boot0/zedbsd.cfg" | sed 's/^/measure: timing lines /' >&2
	to_linux || return 1
	to_zedbsd
}

cfg() {
	ip=$(to_zedbsd) || return 1
	zs "$ip" "sed -i -e '/^$1=/d' /boot/boot0/zedbsd.cfg && sync" || return 1
	if [ -n "${2:-}" ]; then
		zs "$ip" "echo '$1=$2' >> /boot/boot0/zedbsd.cfg && sync" || return 1
	fi
	zs "$ip" "grep -v '^#' /boot/boot0/zedbsd.cfg" | sed 's/^/measure: cfg /' >&2
	to_linux || return 1
	to_zedbsd
}

desktop() {
	keep=0
	if [ "$1" = --keep ]; then
		keep=1
		shift
	fi
	out=$1
	mkdir -p "$out"
	if [ $keep = 0 ]; then
		to_linux || return 1
	fi
	ip=$(to_zedbsd) || return 1
	zcp plan/tools/frame-ops/frame-ops.sh "root@$ip:/tmp/frame-ops.sh" || return 1
	# The input daemon frame-ops starts keeps no descriptor of the SSH session.
	ZS_TIMEOUT=400 zs "$ip" "sh /tmp/frame-ops.sh $SESSION /tmp/marks 1920 1080 > /tmp/frame-ops.out 2>&1 < /dev/null; keiland-shot /tmp/shot.png > /dev/null 2>&1 < /dev/null"
	zget "$ip" "$out" "$SESSION" /tmp/marks /tmp/frame-ops.out /tmp/shot.png || return 1
	python3 -I plan/tools/frame-ops/frame-times.py "$out/session.log" "$out/marks" > "$out/table.txt" 2>&1
	uname=$(zs "$ip" uname -v)
	echo "zedBSD $ip $uname"
	cat "$out/table.txt"
}

# The lines a zedBSD run starts with: a pointer move through the test input (started when it is not), so the
# desktop's idle time starts over and the screen stays on and unlocked (it goes off after 15 minutes without input).
WAKE="[ -S /run/aat-input.sock ] || { aat-input start --width 1920 --height 1080 > /dev/null 2>&1; sleep 3; }
aat-input move-to 960 600 > /dev/null 2>&1
aat-input move-to 962 602 > /dev/null 2>&1"

# zrun IP: the script on standard input run on zedBSD as root, detached from the SSH session (a session that ran
# mview in the foreground was sometimes closed by the remote side), its end polled for at most 600 seconds.
zrun() {
	ip=$1
	zs "$ip" "cat > /tmp/zrun.sh && rm -f /tmp/zrun.done && { (sh /tmp/zrun.sh; echo \$? > /tmp/zrun.done) > /dev/null 2>&1 < /dev/null & }" || return 1
	end=$(($(date +%s) + 600))
	while [ "$(date +%s)" -lt $end ]; do
		sleep 3
		ZS_TIMEOUT=15 zs "$ip" "test -f /tmp/zrun.done" < /dev/null 2> /dev/null && return 0
	done
	say "the run did not end in 600 s"
	return 1
}

mview() {
	out=$1
	shift
	mkdir -p "$out"
	ip=$(to_zedbsd) || return 1
	# The run's own lines of the compositor's log follow the line count taken before it.
	zrun "$ip" <<EOS || return 1
$WAKE
rm -f /tmp/mview.log /tmp/mview-scanout.log
n=\$(wc -l < $SESSION)
su $SESSION_USER -c 'env XDG_RUNTIME_DIR=/run/user/1000 WAYLAND_DISPLAY=wayland-0 ${ZEDBSD_ENV:-} /bin/mview $*' > /tmp/mview.log 2>&1 < /dev/null
tail -n +\$n $SESSION | grep -E 'KWL SCANOUT' > /tmp/mview-scanout.log
EOS
	zget "$ip" "$out" /tmp/mview.log /tmp/mview-scanout.log || return 1
	grep -vE 'MVIEW (FRAME|INPUT)' "$out/mview.log"
	head -5 "$out/mview-scanout.log"
}

profile() {
	out=$1
	shift
	mkdir -p "$out"
	ip=$(to_zedbsd) || return 1
	zrun "$ip" <<EOS || return 1
$WAKE
rm -f /tmp/mview.log /tmp/kernel.log
n=\$(dmesg | wc -l)
su $SESSION_USER -c 'env XDG_RUNTIME_DIR=/run/user/1000 WAYLAND_DISPLAY=wayland-0 VULKAN_ZEDBSD_SUBMIT_TIMING=1 ${ZEDBSD_ENV:-} /bin/mview $*' > /tmp/mview.log 2>&1 < /dev/null
dmesg | tail -n +\$n | grep 'vk timing' > /tmp/kernel.log
EOS
	zget "$ip" "$out" /tmp/mview.log /tmp/kernel.log || return 1
	grep -E 'MVIEW (SPIN|STAGES|FAILED)|VULKAN TIMING' "$out/mview.log" | tail -6
	grep -E 'frames|gpu busy|op ' "$out/kernel.log" | tail -9
}

shot() {
	out=$1
	copies=$2
	mkdir -p "$out"
	ip=$(to_zedbsd) || return 1
	zrun "$ip" <<EOS || return 1
$WAKE
rm -f /tmp/shot.png /tmp/mview-shot.log
su $SESSION_USER -c 'env XDG_RUNTIME_DIR=/run/user/1000 WAYLAND_DISPLAY=wayland-0 /bin/mview --token=shot --frames=900 --content=none --present=fifo --copies=$copies' > /tmp/mview-shot.log 2>&1 < /dev/null &
sleep 6
env XDG_RUNTIME_DIR=/run/user/1000 keiland-shot /tmp/shot.png > /dev/null 2>&1 < /dev/null
wait
EOS
	zget "$ip" "$out" /tmp/shot.png /tmp/mview-shot.log || return 1
	grep -E 'MVIEW (DONE|FAILED)' "$out/mview-shot.log"
	python3 -I plan/tools/frame-ops/shot-diff.py "$SHOT_REFERENCE/linux-shot-$copies.png" "$out/shot.png"
	python3 -I -c 'import sys, numpy
from PIL import Image
a = numpy.asarray(Image.open(sys.argv[1]).convert("RGB"), dtype=numpy.int16)
b = numpy.asarray(Image.open(sys.argv[2]).convert("RGB"), dtype=numpy.int16)
print("over15", int((numpy.abs(a - b).max(axis=2) >= 16).sum()))' "$SHOT_REFERENCE/linux-shot-$copies.png" "$out/shot.png"
}

hang() {
	out=$1
	mkdir -p "$out"
	ip=$(to_zedbsd) || return 1
	zcp "$USB_VKHANG" "root@$ip:/tmp/vkhang" || return 1
	zrun "$ip" <<EOS || return 1
$WAKE
rm -f /tmp/hang.log /tmp/desktop.png
n=\$(dmesg | wc -l)
chmod 755 /tmp/vkhang
for args in '' '--loops' '--compute --loops'; do
	su $SESSION_USER -c "env XDG_RUNTIME_DIR=/run/user/1000 WAYLAND_DISPLAY=wayland-0 /tmp/vkhang \$args" >> /tmp/hang.log 2>&1 < /dev/null
done
su $SESSION_USER -c 'env XDG_RUNTIME_DIR=/run/user/1000 WAYLAND_DISPLAY=wayland-0 /bin/mview --token=after-hang --spin=2 --content=game --present=mailbox --copies=1' >> /tmp/hang.log 2>&1 < /dev/null
sleep 2
keiland-shot /tmp/desktop.png > /dev/null 2>&1 < /dev/null
dmesg | tail -n +\$n | grep -E 'lost|reset|fence|isolat|fault' >> /tmp/hang.log
EOS
	zget "$ip" "$out" /tmp/hang.log /tmp/desktop.png || return 1
	grep -E 'VKHANG|MVIEW (SPIN|FAILED)|lost|reset|fence|isolat|fault' "$out/hang.log"
}

nggshape() {
	out=$1
	mkdir -p "$out"
	ip=$(to_zedbsd) || return 1
	zcp "$USB_NGGSHAPE" "root@$ip:/tmp/nggshape" || return 1
	zrun "$ip" <<EOS || return 1
$WAKE
rm -f /tmp/nggshape.log
chmod 755 /tmp/nggshape
while read -r args; do
	su $SESSION_USER -c "env XDG_RUNTIME_DIR=/run/user/1000 /tmp/nggshape \$args" >> /tmp/nggshape.log 2>&1 < /dev/null
done <<'LIST'
--indexed --triangles=16 --repeat=1000
--triangles=50000 --indexed --depth
--triangles=50000 --indexed --cull --allback
--triangles=50000 --cull --allback --tail
--triangles=50000 --tiny
--triangles=50000 --outside
--triangles=50000 --cross --depth
--triangles=50000 --indexed --shuffle
--triangles=50000 --cluster=4 --depth
--triangles=50000 --mview
--triangles=50000 --mview --cull
--triangles=50000 --heavy
--triangles=50000 --degenerate=3
--triangles=50000 --mview --texture
LIST
EOS
	zget "$ip" "$out" /tmp/nggshape.log || return 1
	grep NGGSHAPE "$out/nggshape.log"
}

linux_mview() {
	out=$1
	shift
	mkdir -p "$out"
	to_linux || return 1
	# weston on the display, its DRM backend's log telling where each repaint put the window.
	deb "test -S /run/user/1000/wayland-1 && pgrep -x weston > /dev/null || (export XDG_RUNTIME_DIR=/run/user/1000 LIBSEAT_BACKEND=seatd; nohup weston --backend=drm --shell=kiosk --continue-without-input --socket=wayland-1 --log=/tmp/weston.log --logger-scopes=log,drm-backend > /tmp/weston.out 2>&1 < /dev/null & sleep 5)" || return 1
	ZS_TIMEOUT=600 deb "export XDG_RUNTIME_DIR=/run/user/1000 WAYLAND_DISPLAY=wayland-1; n=\$(wc -l < /tmp/weston.log); ${LINUX_ENV:-} ~/linux-mview/bin/mview --model=\$HOME/linux-mview/share/mview/qs40 $* > /tmp/mview.log 2>&1 < /dev/null; tail -n +\$n /tmp/weston.log | grep -E 'Using (plane-only|mixed|renderer)' | sed -E 's/^\[[0-9:.]+\][[:space:]]*//' | sort | uniq -c > /tmp/mview-planes.log"
	timeout 60 scp $SSHOPT "$DEBIAN:/tmp/mview.log" "$DEBIAN:/tmp/mview-planes.log" "$out/" || return 1
	grep -vE 'MVIEW (FRAME|INPUT)' "$out/mview.log"
	cat "$out/mview-planes.log"
}

bench() {
	system=$1
	bout=$2
	list=${3:-$(dirname "$0")/bench.list}
	mkdir -p "$bout"
	: > "$bout/bench.log"
	if [ "$system" = zedbsd ]; then
		ip=$(to_zedbsd) || return 1
		zcp "$USB_MVIEW" "root@$ip:/tmp/mview" || return 1
		zs "$ip" "cp /tmp/mview /bin/mview" || return 1
	else
		to_linux || return 1
		timeout 120 scp -q -r $SSHOPT "$LINUX_MVIEW/." "$DEBIAN:linux-mview/" || return 1
	fi
	grep -v '^#' "$list" | while IFS='|' read -r blabel boptions; do
		[ -n "$blabel" ] || continue
		echo "RUN $blabel $boptions" >> "$bout/bench.log"
		# A run whose summary did not come back (the SSH session to zedBSD drops now and then) is tried again, twice.
		tries=0
		while [ $tries -lt 3 ]; do
			if [ "$system" = zedbsd ]; then
				mview "$bout/$blabel" --token="$blabel" --spin=8 --content=game --present=mailbox --images=${BENCH_IMAGES:-4} $boptions > "$bout/$blabel.txt" 2>&1 < /dev/null
			else
				linux_mview "$bout/$blabel" --token="$blabel" --spin=8 --content=game --present=mailbox --images=${BENCH_IMAGES:-4} $boptions > "$bout/$blabel.txt" 2>&1 < /dev/null
			fi
			grep -q "MVIEW SPIN run=$blabel " "$bout/$blabel.txt" && break
			tries=$((tries + 1))
			sleep 20
		done
		cat "$bout/$blabel.txt" >> "$bout/bench.log"
	done
	python3 -I "$(dirname "$0")/mview-table.py" "$bout/bench.log" > "$bout/table.txt"
	cat "$bout/table.txt"
}

logo() {
	out=$1
	mkdir -p "$out"
	[ -n "$DRIVER_CTL" ] || { say "logo: the driver $GPU_PERF_DRIVER has no control program"; return 1; }
	ip=$(to_zedbsd) || return 1
	zcp "$USB_VKHANG" "root@$ip:/tmp/vkhang" || return 1
	zrun "$ip" <<EOS || return 1
$WAKE
rm -f /tmp/watch.log /tmp/logo.log /tmp/logo-kernel.log /tmp/rest.ppm /tmp/after.ppm
n=\$(dmesg | wc -l)
chmod 755 /tmp/vkhang
$DRIVER_CTL watch 75 > /tmp/watch.log 2>&1 &
w=\$!
sleep 1
for content in game game none game; do
	su $SESSION_USER -c "env XDG_RUNTIME_DIR=/run/user/1000 WAYLAND_DISPLAY=wayland-0 /bin/mview --token=logo --spin=2 --content=\$content --present=mailbox --copies=1" >> /tmp/logo.log 2>&1 < /dev/null
	sleep 1
done
su $SESSION_USER -c "env XDG_RUNTIME_DIR=/run/user/1000 WAYLAND_DISPLAY=wayland-0 /tmp/vkhang" >> /tmp/logo.log 2>&1 < /dev/null
sleep 1
su $SESSION_USER -c "env XDG_RUNTIME_DIR=/run/user/1000 WAYLAND_DISPLAY=wayland-0 /bin/mview --token=logo --spin=2 --content=game --present=mailbox --copies=1" >> /tmp/logo.log 2>&1 < /dev/null
sleep 5
su $SESSION_USER -c "env XDG_RUNTIME_DIR=/run/user/1000 WAYLAND_DISPLAY=wayland-0 /bin/mview --token=logo --spin=2 --content=game --present=mailbox --copies=1" >> /tmp/logo.log 2>&1 < /dev/null
$DRIVER_CTL display | grep -E '^address' >> /tmp/logo.log
wait \$w
$DRIVER_CTL watch 10 > /tmp/watch2.log 2>&1 &
w=\$!
sleep 1
kill \$(ps -ef | awk '\$0 ~ /bin.wayland --session/ && \$0 !~ /awk/ {print \$2}')
wait \$w
$DRIVER_CTL display | grep -E '^(display|address)' >> /tmp/logo.log
$DRIVER_CTL screen /tmp/after.ppm >> /tmp/logo.log 2>&1
dmesg | tail -n +\$n | grep -E 'console|display lease|lost|reset' > /tmp/logo-kernel.log
EOS
	zget "$ip" "$out" /tmp/watch.log /tmp/watch2.log /tmp/logo.log /tmp/logo-kernel.log /tmp/after.ppm || return 1
	cat "$out/watch.log"
	sed 's/^/compositor gone: /' "$out/watch2.log"
	grep -E 'MVIEW (SPIN|FAILED)|VKHANG|^address|^display|^screen' "$out/logo.log" | cut -c1-160
	for ppm in after; do
		python3 -I -c 'import sys
from PIL import Image
im = Image.open(sys.argv[1]).convert("RGB")
print(sys.argv[2], "pixels", im.size[0] * im.size[1], "not black", sum(1 for p in im.getdata() if p != (0, 0, 0)))' "$out/$ppm.ppm" "$ppm"
	done
	grep -cE 'display lease .* claimed' "$out/logo-kernel.log" | sed 's/^/leases claimed: /'
	grep -E 'console' "$out/logo-kernel.log" | sed 's/^.*\] //' | sort | uniq -c
}

# Puts nicwatch (nicwatch/) on the running zedBSD: the script, its service and its line in /etc/rc.conf.
nicwatch() {
	ip=$(to_zedbsd) || return 1
	zcp "$(dirname "$0")/nicwatch/nicwatch.sh" "root@$ip:/etc/nicwatch.sh" || return 1
	zcp "$(dirname "$0")/nicwatch/service" "root@$ip:/etc/service.d/nicwatch" || return 1
	zs "$ip" "chmod 755 /etc/nicwatch.sh; grep -q '^  nicwatch:' /etc/rc.conf || printf '  # Reboots when ue0 gets no address (plan/tools/gpu-perf/nicwatch).\\n  nicwatch:\\n    enabled: true\\n    optional: true\\n' >> /etc/rc.conf; sync; tail -4 /etc/rc.conf" || return 1
	say "nicwatch put in (from the next boot)"
}

command=${1:-}
[ $# -gt 0 ] && shift
case $command in
where) where ;;
nicwatch) nicwatch ;;
zedbsd) to_zedbsd ;;
linux) to_linux && echo linux ;;
deploy) deploy "$@" ;;
timing) timing "$@" ;;
cfg) cfg "$@" ;;
desktop) desktop "$@" ;;
mview) mview "$@" ;;
profile) profile "$@" ;;
shot) shot "$@" ;;
hang) hang "$@" ;;
linux-mview) linux_mview "$@" ;;
bench) bench "$@" ;;
nggshape) nggshape "$@" ;;
logo) logo "$@" ;;
*) sed -n '2,55p' "$0"; exit 2 ;;
esac
