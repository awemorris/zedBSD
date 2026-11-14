#!/bin/sh
# The Raspberry Pi 5's desktop measured with plan/tools/frame-ops (fps, work, period, lag per step, and each key's way
# to the screen), and the board put back.  Run from the top of the tree.
#   PI5=ADDRESS KERNEL=VMUNIX INPUT=AAT-INPUT sh plan/ws353/tests/pi5-frame-ops.sh deploy
#       the board's vmunix kept as /boot/boot0/vmunix.uat and /etc/keiland/session copied to $KEEP (outside build/),
#       KERNEL (a build of plan/tools/aat/config-rpi5-aat.mk) put as /boot/boot0/vmunix, the session script given
#       --log-frames and TERMINAL_PERF=1, and a reboot
#   PI5=ADDRESS INPUT=AAT-INPUT sh plan/ws353/tests/pi5-frame-ops.sh run
#       the output's size read from the session log (KWL READY), frame-ops.sh run as root with aat-input from
#       /tmp/frame-ops, the session log and the marks brought to $OUT, frame-times.py and key-latency.py on them
#       ($OUT/summary.txt)
#   PI5=ADDRESS sh plan/ws353/tests/pi5-frame-ops.sh restore
#       vmunix.uat and the kept session script back, a reboot, the cksums compared with the ones deploy saw
#       (RESTORED OK) and the desktop's KWL READY looked for
# KEEP is keep/ by default, OUT build/q2087/frame-ops.  The board's user is user/user (sudo).
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
step=${1:?usage: pi5-frame-ops.sh deploy|run|restore}
: "${PI5:?PI5=the board's address}"
PW=${PI5_PASSWORD:-user}
KEEP=${KEEP:-keep}
OUT=${OUT:-build/q2087/frame-ops}
SO="-o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR -o ConnectTimeout=8 -o PubkeyAuthentication=no"
SLOG=/run/user/1000/session.log

b() {
	timeout "${BT:-60}" sshpass -p "$PW" ssh $SO "user@$PI5" "$@"
}

sb() {
	b "echo $PW | sudo -S sh -c '$1' 2> /dev/null"
}

put() {
	timeout 300 sshpass -p "$PW" scp $SO -q "$1" "user@$PI5:$2"
}

get() {
	timeout 300 sshpass -p "$PW" scp $SO -q "user@$PI5:$1" "$2"
}

sums() {
	for f in /boot/boot0/vmunix /boot/boot0/cmdline.txt /boot/boot0/config.txt /etc/keiland/session; do
		printf '%s=%s ' "$(basename "$f")" "$(b "cksum < $f" | cut -d' ' -f1)"
	done
	echo
}

board_up() {
	BT=15 b 'uname -s' 2> /dev/null | grep -q zedBSD
}

reboot_board() {
	sb 'sync; (sleep 1; /sbin/reboot) > /dev/null 2>&1 &' > /dev/null 2>&1
	n=0
	while [ $n -lt 24 ]; do
		sleep 5
		board_up || break
		n=$((n + 1))
	done
	n=0
	while [ $n -lt 48 ]; do
		sleep 5
		if board_up; then
			echo "up $(b 'uname -v')"
			return 0
		fi
		n=$((n + 1))
	done
	echo "FAIL the board did not come back"
	exit 1
}

# The desktop's READY line in the session log, within 120 s.
ready() {
	n=0
	while [ $n -lt 24 ]; do
		line=$(b "grep 'KWL READY' $SLOG 2> /dev/null | tail -1")
		if [ -n "$line" ]; then
			echo "$line"
			return 0
		fi
		sleep 5
		n=$((n + 1))
	done
	return 1
}

board_up || { echo "FAIL $PI5 is not zedBSD"; exit 1; }
mkdir -p "$KEEP" "$OUT"

case $step in
deploy)
	: "${KERNEL:?KERNEL=the kernel}"
	if b 'test -f /boot/boot0/vmunix.uat'; then
		echo "FAIL /boot/boot0/vmunix.uat is there already (an earlier deploy): restore first"
		exit 1
	fi
	echo "original $(sums)" | tee "$KEEP/pi5-board.txt"
	get /etc/keiland/session "$KEEP/session" || { echo "FAIL session copy"; exit 1; }
	sb 'cp -p /boot/boot0/vmunix /boot/boot0/vmunix.uat && sync' || { echo "FAIL vmunix.uat"; exit 1; }
	want=$(cksum < "$KERNEL" | cut -d' ' -f1)
	put "$KERNEL" /tmp/vmunix.fo || { echo "FAIL copy"; exit 1; }
	sb "test \"\$(cksum < /tmp/vmunix.fo | cut -d\" \" -f1)\" = $want && cp /tmp/vmunix.fo /boot/boot0/vmunix && sync" ||
	    { echo "FAIL install"; exit 1; }
	# The session script with the frames' lines and the Terminal's times.
	sed -e 's|^exec /bin/wayland --session|TERMINAL_PERF=1; export TERMINAL_PERF\nexec /bin/wayland --log-frames --session|' \
	    "$KEEP/session" > "$OUT/session.fo"
	grep -q -- '--log-frames' "$OUT/session.fo" || { echo "FAIL session edit"; exit 1; }
	put "$OUT/session.fo" /tmp/session.fo &&
	    sb 'cp /tmp/session.fo /etc/keiland/session.new && chmod 755 /etc/keiland/session.new &&
	    mv /etc/keiland/session.new /etc/keiland/session && sync' || { echo "FAIL session install"; exit 1; }
	echo "deployed $(sums)"
	reboot_board
	;;
run)
	: "${INPUT:?INPUT=aat-input}"
	line=$(ready) || { echo "FAIL no KWL READY"; exit 1; }
	w=$(echo "$line" | sed -n 's/.* width=\([0-9]*\).*/\1/p')
	h=$(echo "$line" | sed -n 's/.* height=\([0-9]*\).*/\1/p')
	echo "mode $(b "grep 'KWL DISPLAY device' $SLOG | tail -1")"
	echo "ready ${w}x$h"
	b 'grep -c "KWL LAT draw" '"$SLOG" | sed 's/^/frames-so-far /'
	b 'mkdir -p /tmp/frame-ops'
	put "$INPUT" /tmp/frame-ops/aat-input && put plan/tools/frame-ops/frame-ops.sh /tmp/frame-ops/frame-ops.sh ||
	    { echo "FAIL files"; exit 1; }
	b 'chmod 755 /tmp/frame-ops/aat-input /tmp/frame-ops/frame-ops.sh'
	sleep 10
	BT=300 sb "PATH=/tmp/frame-ops:\$PATH sh /tmp/frame-ops/frame-ops.sh $SLOG /tmp/frame-ops/marks $w $h > /tmp/frame-ops/run.txt 2>&1"
	sb "chmod a+r /tmp/frame-ops/run.txt /tmp/frame-ops/marks; cp $SLOG /tmp/frame-ops/session.log; chmod a+r /tmp/frame-ops/session.log"
	get /tmp/frame-ops/run.txt "$OUT/run.txt" && get /tmp/frame-ops/marks "$OUT/marks" &&
	    get /tmp/frame-ops/session.log "$OUT/session.log" || { echo "FAIL collect"; exit 1; }
	sync
	{
		echo "== mode"
		grep 'KWL DISPLAY device' "$OUT/session.log" | tail -1
		echo "ready ${w}x$h"
		echo "== frame-ops"
		cat "$OUT/run.txt"
		echo "== marks"
		cat "$OUT/marks"
		echo "== frame-times"
		python3 -I plan/tools/frame-ops/frame-times.py "$OUT/session.log" "$OUT/marks"
		echo "== key-latency"
		python3 -I plan/tools/frame-ops/key-latency.py "$OUT/session.log" "$OUT/marks"
	} > "$OUT/summary.txt" 2>&1
	echo "summary $OUT/summary.txt"
	;;
restore)
	b 'test -f /boot/boot0/vmunix.uat' || { echo "FAIL no vmunix.uat"; exit 1; }
	put "$KEEP/session" /tmp/session.uat || { echo "FAIL copy"; exit 1; }
	sb 'cp /tmp/session.uat /etc/keiland/session.new && chmod 755 /etc/keiland/session.new &&
	    mv /etc/keiland/session.new /etc/keiland/session && mv /boot/boot0/vmunix.uat /boot/boot0/vmunix && sync' ||
	    { echo "FAIL restore"; exit 1; }
	reboot_board
	now=$(sums)
	was=$(sed -n 's/^original //p' "$KEEP/pi5-board.txt")
	echo "now $now"
	echo "was $was"
	if [ "$now" = "$was" ]; then echo "RESTORED OK"; else echo "FAIL restore mismatch"; fi
	ready || echo "FAIL no KWL READY"
	echo "frame lines: $(b "grep -c 'KWL LAT draw' $SLOG")"
	b 'ls /boot/boot0/; test -e /dev/input-inject && echo input-inject-present || echo no-input-inject'
	;;
esac
sync
