#!/bin/sh
# The hang check's S5 and S2 (plan/ws250/tests/rtx-hang-check.sh) on the RTX 2070 machine (GPU_PERF_MACHINE=rtx,
# 10.0.10.3), for each kernel given, in one command: the kernel deployed and zedBSD up, then
#   T   vkhang --loops three times, each timed from the host (ssh included): how long a client takes from its start to
#       its first draw's end, so kernels that set a client up faster or slower show
#   S5  vkhang killed KILL seconds after it shows (0.05, 0.5 as the hang check does, 1.5): whether it ended, the
#       control vkhang --loops's time, the recovery's lines (whether the draw that never ends had begun)
#   S2  vkhang while mview runs, as the hang check does: lost, mview's frames, the control
# A kernel given as VMUNIX@NAME=VALUE runs with the boot line NAME=VALUE (taken out again after its runs).  The last
# kernel given stays in place (the NVMe's own kernel last puts the machine back).  OUT/summary.txt holds the summaries.
#   usage: plan/tools/rtx-video/rtx-s5-check.sh OUT VMUNIX[@NAME=VALUE]...   (from the top of the tree)
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
OUT=${1:?usage: rtx-s5-check.sh OUT VMUNIX...}
shift
export GPU_PERF_MACHINE=${GPU_PERF_MACHINE:-rtx}
MEASURE="sh plan/tools/gpu-perf/measure.sh"
top=$(pwd)
. plan/tools/gpu-perf/conf.sh
KEY=${KEY:-$top/plan/tmp/guest/id_ed25519}
SSHOPT="-o BatchMode=yes -o ConnectTimeout=8 -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR"
mkdir -p "$OUT" "$top/build/tmp"
ZSSHOPT="$SSHOPT -o ControlMaster=auto -o ControlPath=$top/build/tmp/ssh-%h -o ControlPersist=600"
SUMMARY=$OUT/summary.txt
: > "$SUMMARY"
MVIEW_RUN="--spin=12 --content=game --present=mailbox --copies=5000"
AS_USER='su $(awk -F: '"'"'$3 == 1000 { print $1; exit }'"'"' /etc/passwd) -c'
ENVS="env XDG_RUNTIME_DIR=/run/user/1000 WAYLAND_DISPLAY=wayland-0"
KLINES="channel [0-9]+|lost|stopped|stuck|isolat|fault|session channel|shares the work channel|executor fail|vram clear|rm: "

say() {
	echo "$(date +%H:%M:%S) $*" | tee -a "$SUMMARY"
}

# zs IP COMMAND: a command on zedBSD as root, at most ZS_TIMEOUT seconds.
zs() {
	zip=$1
	shift
	timeout "${ZS_TIMEOUT:-60}" ssh -i "$KEY" $ZSSHOPT "root@$zip" "$@"
}

# run NAME LIMIT: the script on stdin run on zedBSD as root (at most LIMIT seconds), its output in DIR/NAME.log.
run() {
	rname=$1
	rlimit=$2
	ZS_TIMEOUT=$rlimit zs "$IP" "cat > /tmp/ws243-$rname.sh && timeout $rlimit sh /tmp/ws243-$rname.sh" > "$DIR/$rname.log" 2>&1
}

# spun FILE: target shell text that waits (30 s at most) until the mview writing FILE has counted its first second of frames.
spun() {
	printf 'w=0; while ! grep -q "MVIEW FPS" %s 2> /dev/null && [ $w -lt 150 ]; do sleep 0.2; w=$((w + 1)); done\n' "$1"
}

number=0
for KERNEL in "$@"; do
	number=$((number + 1))
	DIR=$OUT/k$number
	mkdir -p "$DIR"
	VMUNIX=${KERNEL%%@*}
	LINE=
	[ "$VMUNIX" = "$KERNEL" ] || LINE=${KERNEL#*@}

	# The kernel in place, the boot line when one is given, and zedBSD up.
	$MEASURE deploy --kernel "$VMUNIX" > "$DIR/deploy.log" 2>&1 || { say "FAIL deploy $VMUNIX"; exit 1; }
	if [ -n "$LINE" ]; then
		$MEASURE cfg "${LINE%%=*}" "${LINE#*=}" > "$DIR/cfg.log" 2>&1 || { say "FAIL cfg $LINE"; exit 1; }
	fi
	IP=$($MEASURE zedbsd 2> "$DIR/zedbsd.log") || { say "FAIL zedbsd not up"; exit 1; }
	say "k$number zedbsd $IP kernel $(zs "$IP" "cksum < /boot/boot0/vmunix" < /dev/null | cut -d' ' -f1) (wanted $(cksum < "$VMUNIX" | cut -d' ' -f1)) | line ${LINE:-none}: $(zs "$IP" "grep -cx '${LINE:-none-given}' /boot/boot0/zedbsd.cfg" < /dev/null)"
	timeout 120 scp -i "$KEY" $ZSSHOPT "$USB_VKHANG" "$USB_MVIEW" "root@$IP:/tmp/" > /dev/null 2>&1 || { say "FAIL copy vkhang mview"; exit 1; }
	zs "$IP" "chmod 755 /tmp/vkhang /tmp/mview" < /dev/null

	# T: a client's start to its first draw's end, three times.
	for i in 1 2 3; do
		start=$(date +%s%N)
		line=$(zs "$IP" "$AS_USER \"$ENVS /tmp/vkhang --loops\" 2>&1" < /dev/null | grep VKHANG | tail -1)
		end=$(date +%s%N)
		say "k$number T$i $(( (end - start) / 1000000 )) ms | $line"
	done

	# S5: closed KILL seconds in while its draw never ends.
	for kill in 0.05 0.5 1.5; do
		run "s5-$kill" 60 <<EOS
n=\$(dmesg | wc -l)
hangs() { ps -A -o pid,state,comm | awk '\$2 != "Z" && \$3 ~ /vkhang$/ { print \$1 }'; }
$AS_USER "$ENVS /tmp/vkhang" > /tmp/s5.log 2>&1 &
w=0; while [ -z "\$(hangs)" ] && [ \$w -lt 50 ]; do sleep 0.1; w=\$((w + 1)); done
sleep $kill
echo "S5KILL \$(hangs | tr '\\n' ' ')"
kill -9 \$(hangs)
w=0; while [ -n "\$(hangs)" ] && [ \$w -lt 100 ]; do sleep 0.2; w=\$((w + 1)); done
echo "S5ENDED after=\$((w * 2))00ms still=\$(hangs | wc -l)"
wait
$AS_USER "$ENVS /tmp/vkhang --loops" 2>&1
dmesg | tail -n +\$n | grep -E '$KLINES'
EOS
		say "k$number S5 kill $kill s | $(grep S5ENDED "$DIR/s5-$kill.log") | control $(grep -E 'VKHANG' "$DIR/s5-$kill.log" | tail -1) | $(grep -c ' lost:' "$DIR/s5-$kill.log") lost, $(grep -oE 'stop answered [0-9]+ after [0-9]+ ms' "$DIR/s5-$kill.log" | head -1)"
	done

	# S2: vkhang while mview runs.
	run s2 120 <<EOS
n=\$(dmesg | wc -l)
$AS_USER "$ENVS /tmp/mview --token=s2 $MVIEW_RUN" > /tmp/s2-mview.log 2>&1 &
$(spun /tmp/s2-mview.log)
sleep 1
$AS_USER "$ENVS /tmp/vkhang" 2>&1
wait
grep -E 'MVIEW (SPIN|STAGES|FAILED|FPS|HITCH )' /tmp/s2-mview.log
$AS_USER "$ENVS /tmp/vkhang --loops" 2>&1
dmesg | tail -n +\$n | grep -E '$KLINES'
EOS
	say "k$number S2 $(grep -E 'VKHANG' "$DIR/s2.log" | head -1) | $(grep -E 'MVIEW SPIN|MVIEW FAILED' "$DIR/s2.log" | tail -1) | control $(grep -E 'VKHANG' "$DIR/s2.log" | tail -1) | $(grep -oE 'stop answered [0-9]+ after [0-9]+ ms' "$DIR/s2.log" | head -1) | $(grep -c 'vram clear' "$DIR/s2.log") vram clear lines"

	# The boot line taken out again.
	if [ -n "$LINE" ]; then
		$MEASURE cfg "${LINE%%=*}" > "$DIR/cfg-back.log" 2>&1 || say "FAIL cfg back ${LINE%%=*}"
	fi
done
say "done: $OUT"
cat "$SUMMARY"
