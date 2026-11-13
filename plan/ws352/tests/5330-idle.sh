#!/bin/sh
# ws352: the desktop's idle wakes and power on the Latitude 5330, end to end (deploy, boot, run, collect, summary),
# under /tmp/i915-hw.lock.  One run; the summary is LOGS/summary.txt, its last line "5330-idle: done ..." or
# "5330-idle: FAIL ...".
#   sh plan/ws352/tests/5330-idle.sh VMUNIX STATIC NAME [TREE]
# VMUNIX is the kernel put on p4 (x86_64, CONFIG_KERN_THERMAL=y CONFIG_DRIVER_X86_POWER_CONTROL=y), with or without
# poll-diag.patch (test only: each thread's polls, timeouts and wakes, and wake-reason's sleepers, logged every 10 s).
# STATIC holds the tree's static thermal, sysctl and wifi (plan/ws333/tests/hw5330/thermal-static.sh,
# static-tool.sh).  TREE, when given, is a root tree (build/x86_64/rootfs) whose bin, sbin, lib and usr are laid
# over the NVMe's root (the NVMe's own firmware and /etc stay), so that the tree's userland runs with the NVMe's
# firmware; p5 is then kept whole on Debian first and written back at the end.  The 5330 must be on Debian.
# Every zedBSD boot is a one-time UEFI boot (~/boot-zedbsd.sh) with the TCO watchdog on (watchdog=600).  No S0
# idle and no sleepctl.
#  1. Keeps p4's vmunix and zedbsd.cfg once as ~/ws245/*-ws352-orig (they must be ORIG_SHA and CFG_SHA); with
#     TREE, p5 whole (gzip) as ~/ws245/ws352-p5.gz with its sha256.
#  2. VMUNIX on p4, zedBSD as it boots; with TREE the tree's files are laid over the root (each written beside
#     and renamed over), and zedBSD is started again (Debian in between) so the session starts with them.
#  3. After SETTLE seconds: the inventory (ps, sums of the desktop's programs), then WINDOWS (default "held held2
#     quiet") of 60 s each as WS333 p029 measured them (the package power, the package C-states, every CPU's, the
#     GT's RC6; quiet with the Wi-Fi radios down and audiod stopped), and with the diag kernel the poll and timer
#     reports of the last 10 s after each.
#  4. Debian again; with TREE p5 written back from the copy and its sha256 compared; p4 gets the original vmunix
#     and zedbsd.cfg back and their sha256 are checked.
# Nothing is removed on either machine.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
VMUNIX=${1:?VMUNIX}
STATIC=${2:?STATIC}
NAME=${3:?NAME}
TREE=${4:-}
cd "$(dirname "$0")/../../.."
LOGS=${LOGS:-build/hw5330/ws352}/$NAME
WINDOWS=${WINDOWS:-held held2 quiet}
WATCHDOG=600
SETTLE=${SETTLE:-60}
ORIG_SHA=4c98d617279bf96b
CFG_SHA=1b33a395b0ff4bd7
HOST=solaris10-man
ZB=10.0.30.3
ZBU=user
SO='-o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o ConnectTimeout=5 -o LogLevel=ERROR'
NV_BOOT=/dev/disk/by-partuuid/d4dbe962-cc37-4f27-8529-bb7446730178
NV_ROOT=/dev/disk/by-partuuid/24c59452-7bc3-42e7-b532-5653d935c325
T=/tmp/ws352-thermal
SY=/tmp/ws352-sysctl
for f in "$VMUNIX" "$STATIC/sysctl" "$STATIC/thermal" "$STATIC/wifi"; do
	[ -f "$f" ] || { echo "5330-idle: FAIL: no $f"; exit 1; }
done
if [ -n "$TREE" ]; then
	[ -f "$TREE/bin/wayland" ] && [ -f "$TREE/lib/libc.so" ] || { echo "5330-idle: FAIL: $TREE is not a root tree"; exit 1; }
fi
mkdir -p "$LOGS"
SUMMARY=$LOGS/summary.txt
: > "$SUMMARY"
exec 9> /tmp/i915-hw.lock
flock -w 3600 9 || { echo "5330-idle: FAIL: the lock"; exit 1; }

say() {
	echo "5330-idle: $*" | tee -a "$SUMMARY"
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
zput() {
	TO=120 timeout 120 sshpass -p "$ZBU" scp $SO -q "$1" "$ZBU@$ZB:$2"
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
	lin "sudo -n sh -c 'mkdir -p /mnt/zb && { while mountpoint -q /mnt/zb; do umount /mnt/zb; done; } && mount $NV_BOOT /mnt/zb && cp $DH/ws245/vmunix-ws352-orig /mnt/zb/vmunix && cp $DH/ws245/zedbsd.cfg-ws352-orig /mnt/zb/zedbsd.cfg && sync && sha256sum /mnt/zb/vmunix /mnt/zb/zedbsd.cfg && umount /mnt/zb'" > "$LOGS/restore.txt" 2>&1
	v=$(awk 'NR == 1 { print substr($1, 1, 16) }' "$LOGS/restore.txt")
	c=$(awk 'NR == 2 { print substr($1, 1, 16) }' "$LOGS/restore.txt")
	if [ "$v" = "$ORIG_SHA" ] && [ "$c" = "$CFG_SHA" ]; then
		say "p4 restored: vmunix $v zedbsd.cfg $c"
	else
		say "FAIL: p4 not restored: vmunix $v zedbsd.cfg $c ($LOGS/restore.txt)"
	fi
}
# p5 written back from the copy kept at the start (Debian running), and its sha256 compared.
p5_restore() {
	[ -n "$TREE" ] || return 0
	[ "${P5_KEPT:-0}" = 1 ] || return 0
	TO=1800 lin "sudo -n sh -c 'gzip -dc $DH/ws245/ws352-p5.gz | dd of=$NV_ROOT bs=4M conv=fsync status=none && sync && sha256sum < $NV_ROOT'" > "$LOGS/p5-restore.txt" 2>&1
	now_sha=$(awk 'NR == 1 { print substr($1, 1, 16) }' "$LOGS/p5-restore.txt")
	if [ -n "$now_sha" ] && [ "$now_sha" = "$P5_SHA" ]; then
		say "p5 restored: $now_sha"
		P5_KEPT=0
	else
		say "FAIL: p5 not restored: $now_sha, kept $P5_SHA ($LOGS/p5-restore.txt)"
	fi
}
# Gives up: the originals back if zedBSD answers, Debian back if it can be, p4 restored, the reason last.
fail() {
	say "FAIL: $*"
	if ! TO=8 lin hostname 2> /dev/null | grep -q chaos; then
		zr 'sync; reboot' > /dev/null 2>&1
		sleep 20
		debian_up 600 > /dev/null || { say "FAIL: Debian did not come back; the watchdog (${WATCHDOG} s) should reset zedBSD, else a person"; exit 2; }
	fi
	p5_restore
	restore
	say "FAIL: $*"
	exit 1
}
install() {
	TO=120 timeout 120 scp -q "$1" "$HOST:ws245/vmunix-ws352-$NAME" || fail "copy of $1"
	lin "sudo -n sh \$HOME/ws245/5330-nvme-write.sh kernel \$HOME/ws245/vmunix-ws352-$NAME" > /dev/null || fail "kernel install"
	lin "sudo -n sh -c 'mkdir -p /mnt/zb && { while mountpoint -q /mnt/zb; do umount /mnt/zb; done; } && mount $NV_BOOT /mnt/zb && cp $DH/ws245/zedbsd.cfg-ws352-orig /mnt/zb/zedbsd.cfg && echo watchdog=$WATCHDOG >> /mnt/zb/zedbsd.cfg && cat /mnt/zb/zedbsd.cfg && sha256sum /mnt/zb/vmunix && sync && umount /mnt/zb'" > "$LOGS/install.txt" 2>&1 ||
		fail "zedbsd.cfg"
	say "kernel $(sha256sum "$1" | cut -c1-16) on p4 ($(awk '$2 == "/mnt/zb/vmunix" { print substr($1, 1, 16) }' "$LOGS/install.txt"))"
}
to_zedbsd() {
	lin 'sudo -n sh $HOME/ws245/5330-nvme-write.sh check' > /dev/null || fail "the NVMe's zedBSD partitions are not as expected"
	lin '$HOME/boot-zedbsd.sh' > "$LOGS/boot-$1.txt" 2>&1
	sleep 20
	zedbsd_up || fail "zedBSD did not answer in 5 minutes"
	for f in thermal:$T sysctl:$SY wifi:/tmp/ws352-wifi; do
		zput "$STATIC/${f%%:*}" "${f#*:}" || fail "copy of ${f%%:*}"
	done
	zb "chmod 755 $T $SY /tmp/ws352-wifi"
	zb 'dmesg' > "$LOGS/$1-boot.txt" 2>&1
	say "$1: $(grep -h 'watchdog: armed' "$LOGS/$1-boot.txt" | head -1)"
}
to_debian() {
	zr 'sync; reboot' > /dev/null 2>&1
	sleep 20
	took=$(debian_up 600) || { say "FAIL: Debian did not come back in 10 minutes"; exit 2; }
	say "Debian up after $took s ($(TO=15 lin 'uname -r'))"
}
th() {
	TO=${TO:-60} zr "$T $1" > "$LOGS/$2" 2>&1
}
gt() {
	TO=20 zb "$SY hw.gputelemetry" > "$LOGS/$1" 2>&1
}
rc6_share() {
	awk '
	$1 == "hw.gputelemetry:" && $3 == "driver=i915" {
		for (i = 2; i <= NF; i++) { split($i, kv, "="); f[kv[1]] = kv[2] }
		n++; t[n] = f["time_ns"]; s[n] = f["sleep_ns"]; v[n] = f["valid"]; h[n] = f["awake_holds"]
	}
	END {
		if (n < 2) { print "no i915 telemetry"; exit }
		if (t[2] > t[1]) printf "RC6 %.1f %% of %.1f s (valid %s/%s), forcewake domains held %s\n", (s[2] - s[1]) * 100 / (t[2] - t[1]), (t[2] - t[1]) / 1e9, v[1], v[2], h[2]
		else print "no time between the readings"
	}' "$LOGS/$1" "$LOGS/$2"
}
# One idle window of 60 s: the package power, the package's residency, CPU 0's, every CPU's, the GT's RC6.
window() {
	gt "$1-gt0.txt"
	TO=90 th "power -i 60" "$1-power.txt" &
	TO=90 th "idle package 0 -i 60" "$1-idle-package.txt" &
	TO=90 th "idle cpu 0 -i 60" "$1-idle-cpu0.txt" &
	TO=100 zr "n=\$($SY hw.ncpu | sed \"s/.*: *//\"); i=0; while [ \$i -lt \$n ]; do $T idle cpu \$i | sed \"s/^/\$i /\"; i=\$((i + 1)); done; sleep 60; i=0; while [ \$i -lt \$n ]; do $T idle cpu \$i | sed \"s/^/\$i /\"; i=\$((i + 1)); done" > "$LOGS/$1-idle-cpus.txt" 2>&1 &
	wait
	gt "$1-gt1.txt"
	say "$1 power: $(grep -h '^power' "$LOGS/$1-power.txt" | sed 's/^power [0-9]* //' | grep package | tr '\n' ';')"
	say "$1 package: $(grep -h '^state' "$LOGS/$1-idle-package.txt" | sed 's/residency_ns=[0-9]* //' | tr '\n' ';')"
	say "$1 cpus (core C6 % of 60 s): $(awk '$2 == "state" && $3 == "C6" { split($4, kv, "="); k = $1; if (k in a) printf "%s=%.1f ", $1, (kv[2] - a[k]) / 6e8; else a[k] = kv[2] }' "$LOGS/$1-idle-cpus.txt")"
	say "$1 gt: $(rc6_share "$1-gt0.txt" "$1-gt1.txt")"
}
# The diag kernel's last 10 s reports in a dmesg: the poll timeouts by thread, the near sleepers by process.
diag() {
	zb 'dmesg' > "$LOGS/$1.txt" 2>&1
	if grep -q 'polldiag:' "$LOGS/$1.txt"; then
		awk '/polldiag: notifies/ { block++ } { b[NR] = block; l[NR] = $0 } END { for (i = 1; i <= NR; i++) if (b[i] == block && l[i] ~ /polldiag:/) print l[i] }' "$LOGS/$1.txt" > "$LOGS/$1-poll.txt"
		say "$1 poll ($(grep -h 'notifies' "$LOGS/$1-poll.txt" | sed 's/.*polldiag: //'), 10 s, timeouts>0 by tid): $(awk '{ for (i = 1; i <= NF; i++) if ($i == "polldiag:") break } $(i + 1) == "pid" && $(i + 8) > 0 { printf "%s/%s t=%s c=%s w=%s; ", $(i + 2), $(i + 4), $(i + 8), $(i + 6), $(i + 16) }' "$LOGS/$1-poll.txt")"
		say "$1 timer interrupts (cpu=/s): $(awk '{ for (i = 1; i <= NF; i++) if ($i == "tickdiag:") break } i <= NF && $(i + 2) == "cpu" { t = $(i + 1); if (t != last) { last = t; line = "" } line = line sprintf("%s=%d ", $(i + 3), $(i + 5) / 10); out = line } END { print out }' "$LOGS/$1.txt")"
	fi
}

# 1. The original kernel and configuration, kept once.
TO=15 lin 'uname -sr' | grep -q Linux || { say "FAIL: the 5330 is not on Debian"; exit 1; }
say "$NAME start $(date '+%F %T') on $(TO=15 lin 'uname -sr')"
DH=$(TO=15 lin 'echo $HOME')
case $DH in /home/*) ;; *) say "FAIL: the Debian home ($DH)"; exit 1 ;; esac
TO=60 timeout 60 scp -q plan/tools/hw5330/5330-nvme-write.sh "$HOST:ws245/" || { say "FAIL: copy of the writer"; exit 1; }
lin "sudo -n sh -c 'mkdir -p /mnt/zb && { while mountpoint -q /mnt/zb; do umount /mnt/zb; done; } && mount $NV_BOOT /mnt/zb && { [ -f $DH/ws245/vmunix-ws352-orig ] || cp /mnt/zb/vmunix $DH/ws245/vmunix-ws352-orig; } && { [ -f $DH/ws245/zedbsd.cfg-ws352-orig ] || cp /mnt/zb/zedbsd.cfg $DH/ws245/zedbsd.cfg-ws352-orig; } && chown $(id -un) $DH/ws245/*-ws352-orig && sync && umount /mnt/zb'" > /dev/null 2>&1 ||
	{ say "FAIL: keeping the original"; exit 1; }
orig=$(TO=15 lin 'sha256sum ws245/vmunix-ws352-orig' | cut -c1-16)
cfg=$(TO=15 lin 'sha256sum ws245/zedbsd.cfg-ws352-orig' | cut -c1-16)
[ "$orig" = "$ORIG_SHA" ] && [ "$cfg" = "$CFG_SHA" ] || { say "FAIL: p4 is vmunix $orig zedbsd.cfg $cfg, not $ORIG_SHA $CFG_SHA"; exit 1; }
say "original kept: vmunix $orig zedbsd.cfg $cfg"
P5_KEPT=0
if [ -n "$TREE" ]; then
	TO=1800 lin "sudo -n sh -c 'while mountpoint -q /mnt/zb; do umount /mnt/zb; done; dd if=$NV_ROOT bs=4M status=none | gzip -1 > $DH/ws245/ws352-p5.gz && sync && sha256sum < $NV_ROOT && gzip -dc $DH/ws245/ws352-p5.gz | sha256sum'" > "$LOGS/p5-keep.txt" 2>&1 ||
		{ say "FAIL: keeping p5 ($LOGS/p5-keep.txt)"; exit 1; }
	P5_SHA=$(awk 'NR == 1 { print substr($1, 1, 16) }' "$LOGS/p5-keep.txt")
	gz_sha=$(awk 'NR == 2 { print substr($1, 1, 16) }' "$LOGS/p5-keep.txt")
	[ -n "$P5_SHA" ] && [ "$P5_SHA" = "$gz_sha" ] || { say "FAIL: the copy of p5 ($P5_SHA, $gz_sha)"; exit 1; }
	P5_KEPT=1
	say "p5 kept: $P5_SHA ($(TO=15 lin "du -h $DH/ws245/ws352-p5.gz" | cut -f1))"
fi

# 2. The kernel, zedBSD, and the overlay with a second start.
install "$VMUNIX"
to_zedbsd first
if [ -n "$TREE" ]; then
	# The helper on zedBSD: each file written beside its place and renamed over it (a running program keeps its
	# old file), each link made again; then the files' sums.
	cat > "$LOGS/lay.sh" <<'EOS'
set -u
cd /tmp/ws352-tree || exit 1
find bin sbin lib usr -type d | while read -r d; do mkdir -p "/$d"; done
find bin sbin lib usr -type f | while read -r f; do
	# A set-user or set-group program (sudo, su, passwd) keeps the NVMe's: its owner and mode come from the image.
	if [ -u "/$f" ] || [ -g "/$f" ]; then echo "kept /$f"; continue; fi
	cp "$f" "/$f.ws352-new" || exit 1
	if [ -x "$f" ]; then chmod 755 "/$f.ws352-new"; else chmod 644 "/$f.ws352-new"; fi
	mv "/$f.ws352-new" "/$f" || exit 1
done
sh /tmp/ws352-tree-links.sh
sync
echo laid $(find bin sbin lib usr -type f | wc -l) files and $(grep -c . /tmp/ws352-tree-links.sh) links
EOS
	# The tree's directories and files go in the tar; its links are made again by commands.
	(cd "$TREE" && find bin sbin lib usr ! -type l) | sort > "$LOGS/tree-list.txt"
	(cd "$TREE" && find bin sbin lib usr -type l | sort | while read -r f; do echo "ln -sf $(readlink "$f") /$f"; done) > "$LOGS/tree-links.sh"
	(cd "$TREE" && tar --format=ustar --no-recursion -cf - -T "$OLDPWD/$LOGS/tree-list.txt") > "$LOGS/tree.tar" || fail "tree tar"
	zput "$LOGS/tree-links.sh" /tmp/ws352-tree-links.sh || fail "copy of the links"
	TO=600 timeout 600 sshpass -p "$ZBU" scp $SO -q "$LOGS/tree.tar" "$ZBU@$ZB:/tmp/ws352-tree.tar" || fail "copy of the tree"
	zput "$LOGS/lay.sh" /tmp/ws352-lay.sh || fail "copy of the lay helper"
	TO=300 zb 'mkdir -p /tmp/ws352-tree && cd /tmp/ws352-tree && pax -r < /tmp/ws352-tree.tar' > "$LOGS/tree-unpack.txt" 2>&1 ||
		fail "tree unpack ($LOGS/tree-unpack.txt)"
	TO=900 zr "sh /tmp/ws352-lay.sh" > "$LOGS/lay.txt" 2>&1 || fail "lay ($LOGS/lay.txt)"
	say "tree: $(tail -n 1 "$LOGS/lay.txt"), $(grep -c '^kept' "$LOGS/lay.txt") set-id programs kept"
	to_debian
	to_zedbsd second
fi

# 3. The inventory, the windows, the diag.
th "strategy balanced" strategy.txt
sleep "$SETTLE"
zb 'ps -ef; ps -A -o pid,comm,args' > "$LOGS/ps.txt" 2>&1
zb 'uname -a; for f in /bin/wayland /bin/files /bin/settings /usr/libexec/keiland-ime /lib/libc.so /lib/ld.so /lib/libkeiland.so /lib/libvulkan.so; do [ -e $f ] && cksum $f; done' > "$LOGS/inventory.txt" 2>&1
mkdir -p "$LOGS/bin"
for f in /bin/wayland /bin/files /bin/settings /usr/libexec/keiland-ime; do
	TO=120 timeout 120 sshpass -p "$ZBU" scp $SO -q "$ZBU@$ZB:$f" "$LOGS/bin/" 2> /dev/null
done
say "desktop: $(awk '$6 ~ /^\// { printf "%s:%s ", $2, $6 }' "$LOGS/ps.txt")"
diag diag-idle
for w in $WINDOWS; do
	if [ "$w" = quiet ]; then
		TO=30 zr "/tmp/ws352-wifi wlan0 down; ifconfig wlan0 down; service stop audiod" > "$LOGS/quiet.txt" 2>&1
		sleep 10
	fi
	window "$w"
	diag "diag-$w"
done
zb 'dmesg' > "$LOGS/end.txt" 2>&1
to_debian

# 4. p5 and p4 as they were.
p5_restore
restore
say "done $(date '+%F %T')"
sync
