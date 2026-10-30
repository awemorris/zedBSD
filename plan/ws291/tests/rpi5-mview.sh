#!/bin/sh
# ws291-p021: mview's scene on the Raspberry Pi 5's V3D 7.1, on the board booted from the measurement image
# (rpi5-image.sh, rpi5-image-boot.sh), from the top of the tree:
#   [PI5=ADDRESS] [COPIES='1 10 50 74'] [SPIN=12] [MVIEW_ARGS=...] [PROGRAMS=BUILD] sh plan/ws291/tests/rpi5-mview.sh OUT
# The board is found by its Ethernet MAC (98:fe:54:22:65:c3) on 10.0.30.0/24 unless PI5 names it, and must answer
# uname with zedBSD; root is reached with the guest harness's key (plan/tmp/guest/id_ed25519).  The GPU is started
# when it is still held (sysctl hw.gpu.start=1) and its stage lines (QPUs, clock) kept; the compositor runs on its own
# socket with no desktop client, mview spins in the game mode (fullscreen, direct scanout, mailbox, 4 images) once for
# each of COPIES, the compositor's scanout lines and the kernel's GPU lines are kept, and the compositor is ended.
# PROGRAMS names a build of the same config whose bin/wayland, bin/mview and dynamic libraries are copied to
# /root/p021 and run with LD_LIBRARY_PATH (the image's own files are not changed).  Nothing is power-cycled or rebooted.
# OUT/summary.txt holds what is printed.
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
	head -6 "$out/c$copies.kernel" | sed "s/^/c$copies kernel /" | tee -a "$summary"
done
b "grep -E 'KWL (SCANOUT|MODE [a-z]|FAILED|VULKAN_ERROR|DIRECT|OUTPUT)' $D/wayland.log | sort | uniq -c | head -12" > "$out/scanout.txt" 2>&1
sed 's/^/compositor /' "$out/scanout.txt" | tee -a "$summary"
b "kill \$(cat $D/wayland.pid); sleep 1; dmesg | grep -iE 'v3d|hvs|fault|lost|hang' | tail -8" > "$out/kernel.txt" 2>&1
sed 's/^/kernel /' "$out/kernel.txt" | tee -a "$summary"
say "end $(date +%H:%M)"
