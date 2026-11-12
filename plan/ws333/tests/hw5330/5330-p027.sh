#!/bin/sh
# ws333 p027: why the 5330's package never leaves PC0 under zedBSD, end to end (deploy, boot, run, collect, summary),
# under /tmp/i915-hw.lock.  One run; the summary is LOGS/summary.txt and its last line "5330-p027: done" or
# "5330-p027: FAIL ...".
#   sh plan/ws333/tests/hw5330/5330-p027.sh VMUNIX STATIC NAME
# VMUNIX is a kernel of this tree with gt-park.patch (kbuild.sh); STATIC holds this tree's static sysctl and thermal
# (static-tool.sh, thermal-static.sh): the NVMe's userland predates hw.gputelemetry's version 2.  The 5330 must be on
# Debian.  Every zedBSD boot is a one-time UEFI boot with the TCO watchdog on (watchdog=WATCHDOG, default 600): a
# zedBSD that stops is reset back to Debian without a person, and the ACPI timer runs alike in every window.
#  1. Keeps p4's vmunix and zedbsd.cfg once as ~/ws245/*-p027-orig (the vmunix must be ORIG_SHA, the WS333 kernel),
#     puts VMUNIX on p4.
#  2. held: zedBSD as it boots (i915 started, the GT's forcewake domains held for the node's life).  After SETTLE
#     seconds, a 60 s idle window: the package power, the package C-states, CPU 0's and every CPU's, and
#     hw.gputelemetry before and after (the GT's RC6 time over the window, the forcewake domains held).
#  3. parked: hw.gpu.start=1 parks the GT (gt-park.patch: the suspend's worker park, every domain given back, the
#     display left as it is); the same window; hw.gpu.start=1 again wakes it, and the kernel's line says how long the
#     GT was in RC6 while parked.  Then zedBSD reboots to Debian.
#  4. manual: zedBSD with i915.start=manual (i915 attached, never started: the GT as the firmware left it), the same
#     window.  Then Debian.
#  5. p4 gets the original vmunix and zedbsd.cfg back; their sha256 are checked.
# QUIET=1 adds, while the GT is parked, a window with the Wi-Fi radios down and audiod stopped (STATIC/wifi needed).
# PARTS (default "held manual") picks 2-3 (held) and 4 (manual).  Nothing is removed on either machine.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
VMUNIX=${1:?VMUNIX}
STATIC=${2:?STATIC}
NAME=${3:?NAME}
cd "$(dirname "$0")/../../../.."
LOGS=${LOGS:-build/hw5330/ws333}/$NAME
PARTS=${PARTS:-held manual}
WATCHDOG=${WATCHDOG:-600}
SETTLE=${SETTLE:-60}
ORIG_SHA=4c98d617279bf96b
HOST=solaris10-man
ZB=10.0.30.3
ZBU=user
SO='-o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o ConnectTimeout=5 -o LogLevel=ERROR'
NV_BOOT=/dev/disk/by-partuuid/d4dbe962-cc37-4f27-8529-bb7446730178
T=/tmp/p027-thermal
SY=/tmp/p027-sysctl
for f in "$VMUNIX" "$STATIC/sysctl" "$STATIC/thermal"; do
	[ -f "$f" ] || { echo "5330-p027: FAIL: no $f"; exit 1; }
done
mkdir -p "$LOGS"
SUMMARY=$LOGS/summary.txt
: > "$SUMMARY"
exec 9> /tmp/i915-hw.lock
flock -w 3600 9 || { echo "5330-p027: FAIL: the lock"; exit 1; }

part() {
	case " $PARTS " in
	*" $1 "*) return 0 ;;
	esac
	return 1
}
say() {
	echo "5330-p027: $*" | tee -a "$SUMMARY"
}
lin() {
	timeout "${TO:-120}" ssh -n -o BatchMode=yes -o ConnectTimeout=10 "$HOST" "$1"
}
zb() {
	timeout "${TO:-60}" sshpass -p "$ZBU" ssh -n $SO "$ZBU@$ZB" "$1"
}
# As root on zedBSD.
zr() {
	zb "echo $ZBU | sudo -S sh -c '$1' 2> /dev/null"
}
debian_up() {
	start=$(date +%s)
	until TO=8 lin hostname 2> /dev/null | grep -q chaos; do
		if [ $(($(date +%s) - start)) -gt "${1:-420}" ]; then
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
			return 1
		fi
		sleep 5
	done
	say "zedBSD up after $(($(date +%s) - start + 20)) s"
}
restore() {
	lin "sudo -n sh -c 'mkdir -p /mnt/zb && { while mountpoint -q /mnt/zb; do umount /mnt/zb; done; } && mount $NV_BOOT /mnt/zb && cp $DH/ws245/vmunix-p027-orig /mnt/zb/vmunix && cp $DH/ws245/zedbsd.cfg-p027-orig /mnt/zb/zedbsd.cfg && sync && sha256sum /mnt/zb/vmunix /mnt/zb/zedbsd.cfg $DH/ws245/vmunix-p027-orig $DH/ws245/zedbsd.cfg-p027-orig && umount /mnt/zb'" > "$LOGS/restore.txt" 2>&1
	if [ "$(awk 'NR == 1 || NR == 3 { print $1 }' "$LOGS/restore.txt" | sort -u | wc -l)" = 1 ] &&
		[ "$(awk 'NR == 2 || NR == 4 { print $1 }' "$LOGS/restore.txt" | sort -u | wc -l)" = 1 ]; then
		say "p4 restored: vmunix $(awk 'NR == 1 { print substr($1, 1, 16) }' "$LOGS/restore.txt") zedbsd.cfg $(awk 'NR == 2 { print substr($1, 1, 16) }' "$LOGS/restore.txt")"
	else
		say "FAIL: p4 not restored ($LOGS/restore.txt)"
	fi
}
# Gives up: Debian back if it can be, p4 restored, the reason last.
fail() {
	say "FAIL: $*"
	if ! TO=8 lin hostname 2> /dev/null | grep -q chaos; then
		zr 'reboot' > /dev/null 2>&1
		sleep 20
		debian_up 600 > /dev/null || { say "FAIL: Debian did not come back; the watchdog (${WATCHDOG} s) should reset zedBSD, else a person"; exit 2; }
	fi
	restore
	say "FAIL: $*"
	exit 1
}
# Writes p4's zedbsd.cfg: the original with the given lines added.
config() {
	lin "sudo -n sh -c 'mkdir -p /mnt/zb && { while mountpoint -q /mnt/zb; do umount /mnt/zb; done; } && mount $NV_BOOT /mnt/zb && cp $DH/ws245/zedbsd.cfg-p027-orig /mnt/zb/zedbsd.cfg && for l in $1; do echo \$l >> /mnt/zb/zedbsd.cfg; done && cat /mnt/zb/zedbsd.cfg && sync && umount /mnt/zb'" > "$LOGS/cfg-$2.txt" 2>&1 ||
		fail "zedbsd.cfg ($2)"
}
to_zedbsd() {
	lin 'sudo -n sh $HOME/ws245/5330-nvme-write.sh check' > /dev/null || fail "the NVMe's zedBSD partitions are not as expected"
	lin "sudo -n efibootmgr -n \$(sudo -n efibootmgr -v | grep -i 08ab575f-f718-4eff-a3c9-10d80b1217ea | grep -i 'zedbsd.BOOTX64' | sed -n 's/^Boot\([0-9A-F]\{4\}\).*/\1/p' | head -1) > /dev/null && sudo -n systemctl reboot" > /dev/null 2>&1
	sleep 20
	zedbsd_up || fail "zedBSD did not answer in 5 minutes"
	TO=60 timeout 60 sshpass -p "$ZBU" scp $SO -q "$STATIC/thermal" "$ZBU@$ZB:$T" || fail "copy of thermal"
	TO=60 timeout 60 sshpass -p "$ZBU" scp $SO -q "$STATIC/sysctl" "$ZBU@$ZB:$SY" || fail "copy of sysctl"
	zb "chmod 755 $T $SY"
}
to_debian() {
	zr 'reboot' > /dev/null 2>&1
	sleep 20
	took=$(debian_up 600) || { say "FAIL: Debian did not come back in 10 minutes"; exit 2; }
	say "Debian up after $took s ($(TO=15 lin 'uname -r'))"
}
# Runs a thermal command as root on zedBSD into LOGS/FILE.
th() {
	TO=${TO:-60} zr "$T $1" > "$LOGS/$2" 2>&1
}
# hw.gputelemetry into LOGS/FILE.
gt() {
	TO=20 zb "$SY hw.gputelemetry" > "$LOGS/$1" 2>&1
}
# The GT's RC6 share between two telemetry readings (FILE1, FILE2), and the forcewake domains held at the second.
rc6_share() {
	awk '
	$1 == "hw.gputelemetry:" && $3 == "driver=i915" {
		for (i = 2; i <= NF; i++) { split($i, kv, "="); f[kv[1]] = kv[2] }
		n++; t[n] = f["time_ns"]; s[n] = f["sleep_ns"]; v[n] = f["valid"]; h[n] = f["awake_holds"]; w[n] = f["awake_wakes"]
	}
	END {
		if (n < 2) { print "no i915 telemetry"; exit }
		if (t[2] > t[1]) printf "RC6 %.1f %% of %.1f s (valid %s/%s), forcewake domains held %s, wakes %s->%s\n", (s[2] - s[1]) * 100 / (t[2] - t[1]), (t[2] - t[1]) / 1e9, v[1], v[2], h[2], w[1], w[2]
		else print "no time between the readings"
	}' "$LOGS/$1" "$LOGS/$2"
}
# One idle window of 60 s on zedBSD: the package power, the package's residency, CPU 0's, every CPU's, the GT's RC6.
window() {
	gt "$1-gt0.txt"
	TO=90 th "power -i 60" "$1-power.txt" &
	TO=90 th "idle package 0 -i 60" "$1-idle-package.txt" &
	TO=90 th "idle cpu 0 -i 60" "$1-idle-cpu0.txt" &
	TO=100 zr "n=\$(sysctl hw.ncpu | sed \"s/.*: *//\"); i=0; while [ \$i -lt \$n ]; do $T idle cpu \$i | sed \"s/^/\$i /\"; i=\$((i + 1)); done; sleep 60; i=0; while [ \$i -lt \$n ]; do $T idle cpu \$i | sed \"s/^/\$i /\"; i=\$((i + 1)); done" > "$LOGS/$1-idle-cpus.txt" 2>&1 &
	wait
	gt "$1-gt1.txt"
	say "$1 power: $(grep -h '^power' "$LOGS/$1-power.txt" | sed 's/^power [0-9]* //' | tr '\n' ';')"
	say "$1 package: $(grep -h '^state' "$LOGS/$1-idle-package.txt" | sed 's/residency_ns=[0-9]* //' | tr '\n' ';')"
	say "$1 cpu0: $(grep -h '^state' "$LOGS/$1-idle-cpu0.txt" | sed 's/residency_ns=[0-9]* //' | tr '\n' ';')"
	say "$1 cpus (core state % of 60 s): $(awk '$2 == "state" { split($4, kv, "="); k = $1 " " $3; if (k in a) printf "%s:%s=%.1f ", $1, $3, (kv[2] - a[k]) / 6e8; else a[k] = kv[2] }' "$LOGS/$1-idle-cpus.txt")"
	say "$1 gt: $(rc6_share "$1-gt0.txt" "$1-gt1.txt")"
}

# 1. The original kernel and configuration, kept once; the kernel under test on p4.
TO=15 lin 'uname -sr' | grep -q Linux || { say "FAIL: the 5330 is not on Debian"; exit 1; }
say "$NAME start $(date '+%F %T') on $(TO=15 lin 'uname -sr')"
DH=$(TO=15 lin 'echo $HOME')
case $DH in /home/*) ;; *) say "FAIL: the Debian home ($DH)"; exit 1 ;; esac
TO=60 timeout 60 scp -q plan/tools/hw5330/5330-nvme-write.sh "$HOST:ws245/" || { say "FAIL: copy of the writer"; exit 1; }
lin "sudo -n sh -c 'mkdir -p /mnt/zb && { while mountpoint -q /mnt/zb; do umount /mnt/zb; done; } && mount $NV_BOOT /mnt/zb && { [ -f $DH/ws245/vmunix-p027-orig ] || cp /mnt/zb/vmunix $DH/ws245/vmunix-p027-orig; } && { [ -f $DH/ws245/zedbsd.cfg-p027-orig ] || cp /mnt/zb/zedbsd.cfg $DH/ws245/zedbsd.cfg-p027-orig; } && chown $(id -un) $DH/ws245/*-p027-orig && sync && umount /mnt/zb'" > /dev/null 2>&1 ||
	{ say "FAIL: keeping the original"; exit 1; }
orig=$(TO=15 lin 'sha256sum ws245/vmunix-p027-orig' | cut -c1-16)
[ "$orig" = "$ORIG_SHA" ] || { say "FAIL: p4's vmunix is $orig, not $ORIG_SHA"; exit 1; }
say "original kept: vmunix $orig zedbsd.cfg $(TO=15 lin 'sha256sum ws245/zedbsd.cfg-p027-orig' | cut -c1-16)"
TO=120 timeout 120 scp -q "$VMUNIX" "$HOST:ws245/vmunix-p027-$NAME" || { say "FAIL: copy"; exit 1; }
lin "sudo -n sh \$HOME/ws245/5330-nvme-write.sh kernel \$HOME/ws245/vmunix-p027-$NAME" > /dev/null || { say "FAIL: kernel install"; exit 1; }
say "kernel $(sha256sum "$VMUNIX" | cut -c1-16) on p4"

# 2-3. i915 started: the domains held, then given back.
if part held; then
	config "watchdog=$WATCHDOG" held
	to_zedbsd
	zb 'dmesg' > "$LOGS/held-boot.txt" 2>&1
	say "held: $(grep -h 'watchdog: armed' "$LOGS/held-boot.txt" | head -1)"
	say "held: $(grep -h 'gt pm:' "$LOGS/held-boot.txt" | head -1)"
	th "strategy balanced" held-strategy.txt
	sleep "$SETTLE"
	window held
	TO=30 zr "$SY hw.gpu.start=1" > "$LOGS/park.txt" 2>&1
	sleep 10
	window parked
	# QUIET=1: still parked, the Wi-Fi radios down and audiod (the sound card's stream) stopped, the same window.
	if [ "${QUIET:-0}" = 1 ]; then
		TO=60 timeout 60 sshpass -p "$ZBU" scp $SO -q "$STATIC/wifi" "$ZBU@$ZB:/tmp/p027-wifi" && zb 'chmod 755 /tmp/p027-wifi'
		TO=30 zr "/tmp/p027-wifi wlan0 down; ifconfig wlan0 down; service stop audiod" > "$LOGS/quiet.txt" 2>&1
		sleep 10
		zb 'dmesg | tail -20' > "$LOGS/quiet-dmesg.txt" 2>&1
		say "quiet: $(tr '\n' ' ' < "$LOGS/quiet.txt" | cut -c1-200)"
		window quiet
	fi
	TO=30 zr "$SY hw.gpu.start=1" > "$LOGS/unpark.txt" 2>&1
	sleep 3
	zb 'dmesg' > "$LOGS/held-end.txt" 2>&1
	say "park: $(grep -h 'i915: park:\|test park\|i915: unpark' "$LOGS/held-end.txt" | tr '\n' ';')"
	to_debian
fi

# 4. i915 attached but never started.
if part manual; then
	config "watchdog=$WATCHDOG i915.start=manual" manual
	to_zedbsd
	zb 'dmesg' > "$LOGS/manual-boot.txt" 2>&1
	say "manual: $(grep -h 'i915.start=manual' "$LOGS/manual-boot.txt" | head -1)"
	th "strategy balanced" manual-strategy.txt
	sleep "$SETTLE"
	window manual
	to_debian
fi

# 5. p4 as it was.
restore
say "done $(date '+%F %T')"
