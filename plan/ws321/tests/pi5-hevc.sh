#!/bin/sh
# WS321 on the Raspberry Pi 5 (H.265 decode with the HEVC decoder as Vulkan Video), run from the top of the tree:
#   PI5=ADDRESS sh plan/ws321/tests/pi5-hevc.sh check                  the board's kernel and boot files (cksums)
#   PI5=ADDRESS KERNEL=VMUNIX sh plan/ws321/tests/pi5-hevc.sh deploy   keeps /boot/boot0/vmunix once as vmunix.p321old,
#       puts KERNEL in its place (checked by cksum) and reboots
#   PI5=ADDRESS BUILD=DIR STREAMS=DIR sh plan/ws321/tests/pi5-hevc.sh files
#       BUILD's bin/vkvideo-probe, bin/media-probe, bin/videoplayer, bin/wayland, bin/files, bin/keiland-shot and
#       dynamic/*.so into /tmp/p321 (a tmpfs) with STREAMS's *.h265 and *.sha256 (plan/ws322/tests/p006/make-hevc.sh)
#   PI5=ADDRESS OUT=DIR [NAMES="a b"] sh plan/ws321/tests/pi5-hevc.sh probe
#       vkvideo-probe --list, then vkvideo-probe --expect on each stream (NAMES, or every one), each with its kernel
#       lines; OUT/probe.txt has one line a stream: name, exit, frames decoded, output, matched, failed
#   PI5=ADDRESS OUT=DIR CLIP=FILE [SECONDS=16] sh plan/ws321/tests/pi5-hevc.sh player
#       Video Player on CLIP (on the board) in a compositor of its own for SECONDS, its FRAMES line, the screen's PNG
#   PI5=ADDRESS sh plan/ws321/tests/pi5-hevc.sh restore               the kept kernel back, a boot, the cksums compared
#       with the ones deploy saw (RESTORED OK); /tmp/p321 goes with the boot
# The board's user is user/user (sudo).  The cksums deploy saw are in build/tmp/p321-board.txt.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
step=${1:?usage: pi5-hevc.sh check|deploy|files|probe|player|restore}
: "${PI5:?PI5=the board's address}"
PW=${PI5_PASSWORD:-user}
STATE=build/tmp/p321-board.txt
SO="-o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR -o ConnectTimeout=8 -o PubkeyAuthentication=no"
H=/tmp/p321

b() {
	timeout "${BT:-60}" sshpass -p "$PW" ssh $SO "user@$PI5" "$@"
}

sb() {
	b "echo $PW | sudo -S sh -c '$1' 2> /dev/null"
}

put() {
	timeout 600 sshpass -p "$PW" scp $SO -q "$@"
}

sums() {
	for f in vmunix cmdline.txt config.txt; do
		printf '%s=%s ' "$f" "$(b "cksum < /boot/boot0/$f" | cut -d' ' -f1)"
	done
	echo
}

wait_down_up() {
	n=0
	while [ $n -lt 24 ]; do
		sleep 5
		b 'uname -s' > /dev/null 2>&1 || break
		n=$((n + 1))
	done
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

reboot_board() {
	sb 'sync; (sleep 1; /sbin/reboot) > /dev/null 2>&1 &' > /dev/null 2>&1
	wait_down_up || { echo "FAIL the board did not come back"; exit 1; }
	echo "up $(b 'uname -v')"
}

case $(b 'uname -s' 2>&1) in
zedBSD*) ;;
*) echo "FAIL $PI5 is not zedBSD"; exit 1 ;;
esac

case $step in
check)
	echo "board $(sums)"
	b 'ls /boot/boot0/ | grep p321old'
	;;
deploy)
	: "${KERNEL:?KERNEL=the kernel}"
	mkdir -p "$(dirname "$STATE")"
	if b 'ls /boot/boot0/ | grep -q p321old'; then
		echo "the kept copy is there already (an earlier deploy): $(sums)"
	else
		echo "original $(sums)" | tee "$STATE"
		sb 'cd /boot/boot0 && cp vmunix vmunix.p321old && sync' || { echo "FAIL the copy"; exit 1; }
	fi
	want=$(cksum < "$KERNEL" | cut -d' ' -f1)
	put "$KERNEL" "user@$PI5:/tmp/vmunix.p321" || { echo "FAIL copy"; exit 1; }
	sb "test \"\$(cksum < /tmp/vmunix.p321 | cut -d\" \" -f1)\" = $want && cp /tmp/vmunix.p321 /boot/boot0/vmunix && sync" ||
	    { echo "FAIL install"; exit 1; }
	echo "deployed $(sums)"
	reboot_board
	b 'dmesg | grep -E "hevc|video" | head -8'
	;;
files)
	: "${BUILD:?BUILD=the build}"
	: "${STREAMS:?STREAMS=the streams}"
	b "mkdir -p $H/bin $H/lib $H/streams $H/run && chmod 700 $H/run"
	put "$BUILD/bin/vkvideo-probe" "$BUILD/bin/media-probe" "$BUILD/bin/videoplayer" "$BUILD/bin/wayland" \
	    "$BUILD/bin/files" "$BUILD/bin/keiland-shot" "user@$PI5:$H/bin/" &&
	    put "$BUILD"/dynamic/*.so "user@$PI5:$H/lib/" &&
	    put "$STREAMS"/*.h265 "$STREAMS"/*.sha256 "user@$PI5:$H/streams/" || { echo "FAIL files"; exit 1; }
	b "chmod 755 $H/bin/*; ls $H/streams | wc -l; df -k /tmp | tail -1"
	;;
probe)
	: "${OUT:?OUT=the output directory}"
	mkdir -p "$OUT"
	names=${NAMES:-$(b "ls $H/streams" | sed -n 's/\.h265$//p' | tr '\n' ' ')}
	cat > "$OUT/probe-board.sh" <<BOARD
H=$H
export LD_LIBRARY_PATH=\$H/lib
timeout 60 \$H/bin/vkvideo-probe --list > \$H/list.txt 2>&1
echo "list exit=\$? \$(grep -ciE 'h265|265' \$H/list.txt) h265 lines"
for n in $names; do
	dmesg > \$H/k.before
	timeout 600 \$H/bin/vkvideo-probe --h265 --expect=\$H/streams/\$n.sha256 --time \$H/streams/\$n.h265 > \$H/v-\$n.txt 2>&1
	e=\$?
	dmesg > \$H/k.after
	last=\$(grep -E 'frames decoded' \$H/v-\$n.txt | tail -1)
	echo "stream \$n exit=\$e | \$last | mismatch \$(grep -c MISMATCH \$H/v-\$n.txt) | status \$(grep -c ' status ' \$H/v-\$n.txt)"
	grep -F -x -v -f \$H/k.before \$H/k.after | grep -iE 'video|hevc|fault|tfu|lost|panic' | head -3 | sed "s/^/kernel \$n /"
done
BOARD
	put "$OUT/probe-board.sh" "user@$PI5:$H/probe.sh" || { echo "FAIL script"; exit 1; }
	BT=7000 sb "sh $H/probe.sh" > "$OUT/probe.txt" 2>&1
	put "user@$PI5:$H/list.txt" "$OUT/" > /dev/null 2>&1
	b "cat $H/v-*.txt" > "$OUT/probe-outputs.txt" 2>&1
	b 'dmesg' > "$OUT/dmesg.txt" 2>&1
	grep -c '^stream' "$OUT/probe.txt"
	;;
player)
	: "${OUT:?OUT=the output directory}"
	: "${CLIP:?CLIP=the clip on the board}"
	mkdir -p "$OUT"
	cat > "$OUT/player-board.sh" <<BOARD
H=$H
export LD_LIBRARY_PATH=\$H/lib XDG_RUNTIME_DIR=\$H/run
rm -f \$H/run/wayland-p321 \$H/player.png
\$H/bin/wayland --socket=\$H/run/wayland-p321 "--desktop-client=\$H/bin/files --desktop" > \$H/wayland.log 2>&1 < /dev/null &
wpid=\$!
sleep 15
WAYLAND_DISPLAY=wayland-p321 \$H/bin/videoplayer --timeout-s=${SECONDS:-16} $CLIP > \$H/player.log 2>&1 < /dev/null &
ppid=\$!
sleep 8
timeout 60 \$H/bin/keiland-shot --socket \$H/run/keiland-shot.sock \$H/player.png > \$H/shot.txt 2>&1
echo "shot exit=\$? \$(tr '\n' ' ' < \$H/shot.txt)"
top -b -n 1 2> /dev/null | head -12 | sed 's/^/top /'
wait \$ppid
echo "player exit=\$?"
grep -E 'VIDEOPLAYER (PRESENT|FRAMES|FAILED|DONE)|GPU holds|read back|Vulkan Video|H.265|h265' \$H/player.log | tail -10
kill \$wpid 2> /dev/null; sleep 3; kill -9 \$wpid 2> /dev/null; pkill -f "\$H/bin/files" 2> /dev/null
echo "compositor \$(grep -cE 'KWL (FAILED|VULKAN_ERROR)' \$H/wayland.log) errors"
dmesg | grep -iE 'video|hevc' | tail -6
BOARD
	put "$OUT/player-board.sh" "user@$PI5:$H/player.sh" || { echo "FAIL script"; exit 1; }
	BT=300 sb "sh $H/player.sh" > "$OUT/player.txt" 2>&1
	put "user@$PI5:$H/player.png" "user@$PI5:$H/player.log" "user@$PI5:$H/wayland.log" "$OUT/" > /dev/null 2>&1
	cat "$OUT/player.txt"
	;;
restore)
	sb 'cd /boot/boot0 && test -f vmunix.p321old && mv vmunix.p321old vmunix && sync' || { echo "FAIL restore"; exit 1; }
	reboot_board
	now=$(sums)
	was=$(sed -n 's/^original //p' "$STATE")
	echo "now $now"
	echo "was $was"
	if [ "$now" = "$was" ]; then echo "RESTORED OK"; else echo "FAIL restore mismatch"; fi
	b 'ls /boot/boot0/ | grep p321old; ls /tmp | grep p321'
	;;
esac
sync
