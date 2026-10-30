#!/bin/sh
# ws299: one NVMe measurement run on the Latitude 5330, end to end, under /tmp/i915-hw.lock.
#   sh plan/ws299/tests/nvme-bench-5330.sh debian NAME [read] [write]
#   sh plan/ws299/tests/nvme-bench-5330.sh zedbsd NAME VMUNIX|- [read] [write]
# Before each mode Debian trims its file systems (fstrim -av) and the drive rests PRE_REST seconds; a write mode
# starts with p6 (zedBSD's swap) discarded but for its swap header and ends the same way (bench-debian.sh p6fresh).
# p1 to p5 are never discarded.
# debian: runs bench-debian.sh on the 5330's Debian.
# zedbsd: puts VMUNIX on the NVMe's p4 (5330-nvme-write.sh kernel; "-" keeps the kernel there), then for each mode
#   boots zedBSD once from the NVMe (UEFI BootNext of "zedBSD NVMe"), runs bench-zedbsd.sh MODE as root, and reboots
#   to Debian.
# randread (build-randread.sh) is built into RANDREAD_DIR when it is not there, and copied to both systems.
# Output: LOGS (default build/hw5330/ws299) NAME-MODE.txt for each run, then the summary of summary.py.  The last line
# is "nvme-bench: done|FAIL ...".  Nothing is removed on either machine.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
WHERE=${1:?debian|zedbsd}
NAME=${2:?NAME}
shift 2
KERNEL=-
if [ "$WHERE" = zedbsd ]; then
	KERNEL=${1:?VMUNIX or -}
	shift
fi
MODES=${*:-read}
cd "$(dirname "$0")/../../.."
T=plan/ws299/tests
LOGS=${LOGS:-build/hw5330/ws299}
HOST=solaris10-man
ZB=10.0.30.3
SO='-o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o ConnectTimeout=5 -o LogLevel=ERROR'
RANDREAD_DIR=${RANDREAD_DIR:-build/tmp/randread}
ENVS="REST=${REST:-30} RAND_SECONDS=${RAND_SECONDS:-15}"
mkdir -p "$LOGS"
[ -x "$RANDREAD_DIR/cpuloop-zedbsd" ] && [ -x "$RANDREAD_DIR/cpuloop-linux" ] ||
	sh $T/build-randread.sh "$RANDREAD_DIR" > /dev/null || { echo "nvme-bench: FAIL: randread build"; exit 1; }

say() {
	echo "nvme-bench: $*"
}
lin() {
	timeout "${TO:-120}" ssh -o BatchMode=yes -o ConnectTimeout=10 "$HOST" "$1"
}
# The account on zedBSD: user (password user), or kei (password kei) on an image made before the OS name became
# zedBSD; the first one that answers is kept (ZB_USER chooses it).
ZBU=${ZB_USER:-}
zb_account() {
	for name in user kei; do
		if timeout 20 sshpass -p "$name" ssh $SO "$name@$ZB" true 2> /dev/null; then
			ZBU=$name
			return 0
		fi
	done
	return 1
}
zb() {
	[ -n "$ZBU" ] || zb_account || return 255
	timeout "${TO:-60}" sshpass -p "$ZBU" ssh $SO "$ZBU@$ZB" "$1"
}
debian_up() {
	start=$(date +%s)
	until TO=8 lin hostname 2> /dev/null | grep -q chaos; do
		if [ $(($(date +%s) - start)) -gt 300 ]; then
			say "FAIL: Debian did not come back in 5 minutes"
			exit 2
		fi
		sleep 5
	done
}

exec 9> /tmp/i915-hw.lock
flock 9
debian_up
TO=60 timeout 60 scp -q $T/bench-debian.sh "$RANDREAD_DIR/randread-linux" "$RANDREAD_DIR/patwrite-linux" "$RANDREAD_DIR/directio-linux" \
    "$RANDREAD_DIR/cpuloop-linux" plan/tools/hw5330/5330-nvme-write.sh plan/tools/hw5330/ufs-grow.py "$HOST:ws245/" ||
	{ say "FAIL: copy of the helpers"; exit 1; }
lin 'sudo -n sh $HOME/ws245/bench-debian.sh p6save' > /dev/null || { say "FAIL: p6 save"; exit 1; }

# Trims Debian's file systems and lets the drive rest before a mode.
settle() {
	TO=600 lin 'sudo -n fstrim -av' | sed 's/^/nvme-bench: fstrim: /'
	sleep "${PRE_REST:-60}"
}
p6fresh() {
	lin 'sudo -n sh $HOME/ws245/bench-debian.sh p6fresh' > /dev/null || { say "FAIL: p6 fresh"; exit 1; }
}

if [ "$WHERE" = debian ]; then
	for mode in $MODES; do
		settle
		TO=1800 lin "sudo -n env $ENVS sh \$HOME/ws245/bench-debian.sh $mode" > "$LOGS/$NAME-$mode.txt" 2>&1
		say "debian $mode rc=$? ($(tail -1 "$LOGS/$NAME-$mode.txt"))"
	done
	python3 -I $T/summary.py "$LOGS" "$NAME"
	say done
	exit 0
fi

# zedBSD: the kernel and the UEFI entry.
lin 'sudo -n sh $HOME/ws245/5330-nvme-write.sh check' > /dev/null || { say "FAIL: the NVMe's zedBSD partitions are not as expected"; exit 1; }
if [ "$KERNEL" != - ]; then
	TO=120 timeout 120 scp -q "$KERNEL" "$HOST:ws245/vmunix-$NAME" || { say "FAIL: copy"; exit 1; }
	lin "sudo -n sh \$HOME/ws245/5330-nvme-write.sh kernel \$HOME/ws245/vmunix-$NAME" > /dev/null || { say "FAIL: kernel install"; exit 1; }
	say "kernel installed ($(sha256sum "$KERNEL" | cut -c1-16))"
fi
entry=$(lin "sudo -n efibootmgr -v" | grep -i 08ab575f-f718-4eff-a3c9-10d80b1217ea | grep -i 'zedbsd.BOOTX64' | sed -n 's/^Boot\([0-9A-F]\{4\}\).*/\1/p' | head -1)
[ -n "$entry" ] || { say "FAIL: no UEFI entry zedBSD NVMe"; exit 1; }

for mode in $MODES; do
	[ "$mode" = write ] && p6fresh
	run=$mode
	envrun=$ENVS
	case "$mode" in
	p7rawwrite | p7fswrite | p7direct)
		lin 'sudo -n sh $HOME/ws245/bench-debian.sh p7fresh' > /dev/null || { say "FAIL: p7 fresh"; exit 1; } ;;
	p7ufswrite)
		# The root's kind of UFS on p7 (P7_IMAGE, an image on the 5330), written to without mkfs.
		TO=900 lin "sudo -n env P7_IMAGE=${P7_IMAGE:-} sh \$HOME/ws245/bench-debian.sh p7ufs" > /dev/null ||
			{ say "FAIL: p7 ufs"; exit 1; }
		run="p7fswrite"
		envrun="$ENVS MKFS=0" ;;
	esac
	settle
	lin "sudo -n efibootmgr -n $entry > /dev/null && sudo -n systemctl reboot" > /dev/null 2>&1
	sleep 20
	start=$(date +%s)
	until zb 'uname -s' 2> /dev/null | grep -qi zedbsd; do
		if [ $(($(date +%s) - start)) -gt 300 ]; then
			say "FAIL: zedBSD did not answer in 5 minutes (a power cycle may be needed)"
			exit 2
		fi
		sleep 5
	done
	say "zedBSD up after $(($(date +%s) - start + 20)) s ($mode)"
	sleep "${SETTLE:-30}"
	TO=60 timeout 60 sshpass -p "$ZBU" scp $SO $T/bench-zedbsd.sh "$RANDREAD_DIR/randread-zedbsd" "$RANDREAD_DIR/patwrite-zedbsd" "$RANDREAD_DIR/directio-zedbsd" \
	    "$RANDREAD_DIR/cpuloop-zedbsd" "$ZBU@$ZB:/tmp/" ||
		say "copy to zedBSD failed"
	TO=1500 zb "echo $ZBU | sudo -S env $envrun sh /tmp/bench-zedbsd.sh $run" > "$LOGS/$NAME-$mode.txt" 2>&1
	say "zedbsd $mode rc=$? ($(tail -1 "$LOGS/$NAME-$mode.txt"))"
	zb 'dmesg' > "$LOGS/$NAME-$mode-dmesg.txt" 2>&1
	zb 'echo $ZBU | sudo -S reboot' > /dev/null 2>&1
	debian_up
	[ "$mode" = write ] && p6fresh
done
python3 -I $T/summary.py "$LOGS" "$NAME"
say done
