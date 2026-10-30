#!/bin/sh
# ws299: one boot of zedBSD on the Latitude 5330 from the NVMe (the kernel already on p4), a script run there as root,
# and back to Debian, under /tmp/i915-hw.lock.
#   sh plan/ws299/tests/zb-exec-5330.sh NAME SCRIPT [FILE...]
# SCRIPT and the FILEs are copied to zedBSD's /tmp; SCRIPT runs as root with a limit of EXEC_SECONDS (default 1200).
# Output: LOGS (default build/hw5330/ws299) NAME.txt (the script's output) and NAME-dmesg.txt.  The last line is
# "zb-exec: done|FAIL ...".  Nothing is removed on either machine.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
NAME=${1:?NAME}
SCRIPT=${2:?SCRIPT}
shift 2
cd "$(dirname "$0")/../../.."
LOGS=${LOGS:-build/hw5330/ws299}
HOST=solaris10-man
ZB=10.0.30.3
SO='-o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o ConnectTimeout=5 -o LogLevel=ERROR'
mkdir -p "$LOGS"

say() {
	echo "zb-exec: $*"
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
entry=$(lin "sudo -n efibootmgr -v" | grep -i 08ab575f-f718-4eff-a3c9-10d80b1217ea | grep -i 'zedbsd.BOOTX64' | sed -n 's/^Boot\([0-9A-F]\{4\}\).*/\1/p' | head -1)
[ -n "$entry" ] || { say "FAIL: no UEFI entry zedBSD NVMe"; exit 1; }
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
say "zedBSD up after $(($(date +%s) - start + 20)) s"
sleep "${SETTLE:-20}"
TO=120 timeout 120 sshpass -p "$ZBU" scp $SO "$SCRIPT" "$@" "$ZBU@$ZB:/tmp/" || say "copy to zedBSD failed"
TO=$((${EXEC_SECONDS:-1200} + 30)) zb "echo $ZBU | sudo -S sh /tmp/$(basename "$SCRIPT")" > "$LOGS/$NAME.txt" 2>&1
say "script rc=$? ($(tail -n 1 "$LOGS/$NAME.txt"))"
zb 'dmesg' > "$LOGS/$NAME-dmesg.txt" 2>&1
zb 'echo $ZBU | sudo -S reboot' > /dev/null 2>&1
debian_up
say done
