#!/bin/sh
# ws333 p028: the TCO watchdog on the 5330, end to end (deploy, boot, run, collect, summary).  One run; the summary is
# LOGS/summary.txt and the last line "5330-p028: PASS" or "5330-p028: FAIL ...".
#   sh plan/ws333/tests/hw5330/5330-p028.sh VMUNIX NAME
# VMUNIX is a kernel of this tree (kbuild.sh) with intel-tco.c.  The 5330 must be on Debian.
#  1. Keeps p4's vmunix and zedbsd.cfg as ~/ws245/vmunix-p028-orig and ~/ws245/zedbsd.cfg-p028-orig (once: a second
#     run leaves the first copies), and puts VMUNIX on p4.
#  2. alive: zedbsd.cfg with watchdog=60.  zedBSD is started once (UEFI BootNext) and must run ALIVE seconds
#     (default 660) without a reset: every 30 s it must answer uname over SSH as zedBSD (a reset would bring up
#     Debian, BootNext being used once).  The kernel's
#     "watchdog:" lines are kept (LOGS/alive-dmesg.txt) and must include "armed".  zedBSD is then rebooted to Debian.
#  3. stop: zedbsd.cfg with watchdog=60 watchdog.stop=180.  zedBSD is started once and answers SSH; 180 s after the
#     arming every CPU stops with its interrupts off, and the watchdog must reset the 5330 back to Debian by itself
#     within 7 minutes of zedBSD answering.  Debian's iTCO_wdt bootstatus (0x20 = the TCO's reset, when the firmware
#     leaves the status bit) is recorded, not judged.
#  4. Puts the original vmunix and zedbsd.cfg back on p4 and checks their sha256.
# The 5330 has no remote power: a run that does not come back is left to a person.  /tmp/i915-hw.lock is held.
# Nothing is removed on either machine.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
VMUNIX=${1:?VMUNIX}
NAME=${2:?NAME}
cd "$(dirname "$0")/../../../.."
LOGS=${LOGS:-build/hw5330/ws333}/$NAME
ALIVE=${ALIVE:-660}
HOST=solaris10-man
ZB=10.0.30.3
ZBU=user
SO='-o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o ConnectTimeout=5 -o LogLevel=ERROR'
NV_BOOT=/dev/disk/by-partuuid/d4dbe962-cc37-4f27-8529-bb7446730178
mkdir -p "$LOGS"
SUMMARY=$LOGS/summary.txt
: > "$SUMMARY"
exec 9> /tmp/i915-hw.lock
flock -w 3600 9 || { echo "5330-p028: FAIL: the lock"; exit 1; }

say() {
	echo "5330-p028: $*" | tee -a "$SUMMARY"
}
lin() {
	timeout "${TO:-120}" ssh -n -o BatchMode=yes -o ConnectTimeout=10 "$HOST" "$1"
}
zb() {
	timeout "${TO:-60}" sshpass -p "$ZBU" ssh -n $SO "$ZBU@$ZB" "$1"
}
zr() {
	zb "echo $ZBU | sudo -S sh -c '$1' 2> /dev/null"
}
# Waits for Debian; prints the seconds it took.
debian_up() {
	start=$(date +%s)
	until TO=8 lin hostname 2> /dev/null | grep -q chaos; do
		if [ $(($(date +%s) - start)) -gt "${1:-300}" ]; then
			return 1
		fi
		sleep 5
	done
	echo $(($(date +%s) - start))
}
zedbsd_up() {
	start=$(date +%s)
	until zb 'uname -s' 2> /dev/null | grep -qi zedbsd; do
		if [ $(($(date +%s) - start)) -gt 300 ]; then
			say "FAIL: zedBSD did not come up in 5 minutes"
			exit 2
		fi
		sleep 5
	done
}
# Writes p4's zedbsd.cfg: the original with the given lines added.
config() {
	lin "sudo -n sh -c 'mkdir -p /mnt/zb && { while mountpoint -q /mnt/zb; do umount /mnt/zb; done; } && mount $NV_BOOT /mnt/zb && cp $DH/ws245/zedbsd.cfg-p028-orig /mnt/zb/zedbsd.cfg && for l in $1; do echo \$l >> /mnt/zb/zedbsd.cfg; done && cat /mnt/zb/zedbsd.cfg && sync && umount /mnt/zb'" > "$LOGS/cfg-$2.txt" 2>&1 ||
		{ say "FAIL: zedbsd.cfg ($2)"; exit 1; }
}
to_zedbsd() {
	lin 'sudo -n sh $HOME/ws245/5330-nvme-write.sh check' > /dev/null || { say "FAIL: the NVMe's zedBSD partitions are not as expected"; exit 1; }
	lin "sudo -n efibootmgr -n \$(sudo -n efibootmgr -v | grep -i 08ab575f-f718-4eff-a3c9-10d80b1217ea | grep -i 'zedbsd.BOOTX64' | sed -n 's/^Boot\([0-9A-F]\{4\}\).*/\1/p' | head -1) > /dev/null && sudo -n systemctl reboot" > /dev/null 2>&1
	sleep 20
	zedbsd_up
}
restore() {
	lin "sudo -n sh -c 'mkdir -p /mnt/zb && { while mountpoint -q /mnt/zb; do umount /mnt/zb; done; } && mount $NV_BOOT /mnt/zb && cp $DH/ws245/vmunix-p028-orig /mnt/zb/vmunix && cp $DH/ws245/zedbsd.cfg-p028-orig /mnt/zb/zedbsd.cfg && sync && sha256sum /mnt/zb/vmunix /mnt/zb/zedbsd.cfg $DH/ws245/vmunix-p028-orig $DH/ws245/zedbsd.cfg-p028-orig && umount /mnt/zb'" > "$LOGS/restore.txt" 2>&1
	if [ "$(awk 'NR == 1 || NR == 3 { print $1 }' "$LOGS/restore.txt" | sort -u | wc -l)" = 1 ] &&
		[ "$(awk 'NR == 2 || NR == 4 { print $1 }' "$LOGS/restore.txt" | sort -u | wc -l)" = 1 ]; then
		say "p4 restored: vmunix $(awk 'NR == 1 { print substr($1, 1, 16) }' "$LOGS/restore.txt")"
	else
		say "FAIL: p4 not restored ($LOGS/restore.txt)"
		status=1
	fi
}
status=0

# 1. The original kernel and configuration, kept once; the kernel under test on p4.
TO=15 lin 'uname -sr' | grep -q Linux || { say "FAIL: the 5330 is not on Debian"; exit 1; }
# The Debian user's home, written out: root's shell under sudo would read its own.
DH=$(TO=15 lin 'echo $HOME')
case $DH in /home/*) ;; *) say "FAIL: the Debian home ($DH)"; exit 1 ;; esac
TO=60 timeout 60 scp -q plan/tools/hw5330/5330-nvme-write.sh "$HOST:ws245/" || { say "FAIL: copy of the writer"; exit 1; }
lin "sudo -n sh -c 'mkdir -p /mnt/zb && { while mountpoint -q /mnt/zb; do umount /mnt/zb; done; } && mount $NV_BOOT /mnt/zb && { [ -f $DH/ws245/vmunix-p028-orig ] || cp /mnt/zb/vmunix $DH/ws245/vmunix-p028-orig; } && { [ -f $DH/ws245/zedbsd.cfg-p028-orig ] || cp /mnt/zb/zedbsd.cfg $DH/ws245/zedbsd.cfg-p028-orig; } && chown $(id -un) $DH/ws245/*-p028-orig && sync && umount /mnt/zb'" > /dev/null 2>&1 ||
	{ say "FAIL: keeping the original"; exit 1; }
say "original kept: $(TO=15 lin 'sha256sum ws245/vmunix-p028-orig' | cut -c1-16)"
TO=120 timeout 120 scp -q "$VMUNIX" "$HOST:ws245/vmunix-p028-$NAME" || { say "FAIL: copy"; exit 1; }
lin "sudo -n sh \$HOME/ws245/5330-nvme-write.sh kernel \$HOME/ws245/vmunix-p028-$NAME" > /dev/null || { say "FAIL: kernel install"; exit 1; }
say "kernel $(sha256sum "$VMUNIX" | cut -c1-16) on p4"

# 2. alive: ALIVE seconds without a reset.
config "watchdog=60" alive
to_zedbsd
zb 'dmesg' > "$LOGS/alive-dmesg-full.txt" 2>&1
grep 'watchdog' "$LOGS/alive-dmesg-full.txt" > "$LOGS/alive-dmesg.txt"
grep -q 'watchdog: armed' "$LOGS/alive-dmesg.txt" || { say "alive: FAIL: no armed line"; status=1; }
say "alive: $(grep 'watchdog: armed' "$LOGS/alive-dmesg.txt" | head -1)"
start=$(date +%s)
last=0
while [ $(($(date +%s) - start)) -lt "$ALIVE" ]; do
	up=$(zb 'uname -s' 2> /dev/null | tr '\n' ' ')
	echo "$(($(date +%s) - start)) $up" >> "$LOGS/alive-uptime.txt"
	if ! echo "$up" | grep -qi zedbsd; then
		say "alive: FAIL: zedBSD stopped answering after $(($(date +%s) - start)) s"
		status=1
		break
	fi
	sleep 30
done
[ "$status" = 0 ] && say "alive: answered for $(($(date +%s) - start)) s ($(tail -1 "$LOGS/alive-uptime.txt"))"
zr 'reboot' > /dev/null 2>&1
sleep 20
took=$(debian_up 300) || { say "FAIL: Debian did not come back after the alive run"; exit 2; }
say "Debian back after the alive run ($took s)"

# 3. stop: the watchdog brings the 5330 back; not tried when the alive run did not arm it.
if [ "$status" != 0 ]; then
	restore
	say "FAIL (the alive run failed; the stop was not tried)"
	exit 1
fi
config "watchdog=60 watchdog.stop=180" stop
to_zedbsd
zb 'dmesg' 2> /dev/null | grep 'watchdog' > "$LOGS/stop-dmesg.txt"
say "stop: zedBSD up; $(grep -c . "$LOGS/stop-dmesg.txt") watchdog lines"
up_at=$(date +%s)
gone=
while [ $(($(date +%s) - up_at)) -lt 420 ]; do
	if ! zb 'uname -s' > /dev/null 2>&1; then
		gone=$(($(date +%s) - up_at))
		break
	fi
	sleep 5
done
if [ -z "$gone" ]; then
	say "stop: FAIL: zedBSD kept answering for 7 minutes; rebooted to Debian"
	status=1
	zr 'reboot' > /dev/null 2>&1
	sleep 20
fi
say "stop: zedBSD stopped answering after ${gone:-?} s"
took=$(debian_up 420) || { say "stop: FAIL: Debian did not come back in 7 minutes; the 5330 needs a person"; exit 2; }
say "stop: Debian back $(($(date +%s) - up_at)) s after zedBSD answered"
TO=30 lin 'cat /sys/class/watchdog/watchdog0/bootstatus; uptime; sudo -n dmesg | grep -i tco' > "$LOGS/stop-debian.txt" 2>&1
bootstatus=$(head -1 "$LOGS/stop-debian.txt")
say "stop: Debian's iTCO bootstatus $bootstatus (recorded only)"

# 4. p4 as it was.
restore
if [ "$status" = 0 ]; then
	say "PASS"
else
	say "FAIL (see $SUMMARY)"
fi
exit "$status"
