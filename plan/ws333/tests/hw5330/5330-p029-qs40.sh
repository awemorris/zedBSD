#!/bin/sh
# ws333 p029: WS291 p022's qs40 on the 5330, several rounds in one start of zedBSD, end to end (check the NVMe
# against the copy p027 kept, write the image, boot once with the watchdog, run, collect, restore), with
# plan/ws291/tests/5330-mview.sh for its steps (each takes /tmp/i915-hw.lock itself; this script takes it only for its
# own steps between them).  The summary is LOGS/summary.txt, its last line "5330-p029-qs40: done" or
# "5330-p029-qs40: FAIL ...".
#   sh plan/ws333/tests/hw5330/5330-p029-qs40.sh IMG VMUNIX NAME [COPIES...]
# IMG is a disk image of config-5330-qs40.mk; VMUNIX the kernel put on p4 after the image.  COPIES defaults to
# "1 600"; the rounds (ROUNDS, default 3) each run every COPIES once (5330-mview.sh run LOGS/rN COPIES).  The 5330
# must be on Debian.
#  1. The NVMe's zedBSD is kept once in ~/ws245/p029-nvme-BKNAME on Debian (BKNAME default p029; p4's files, p5
#     and p6 whole with gzip, the ESP loader) with its sums (p4's files by cksum but the boot logs under log/, which
#     each start of an image adds, p5 and p6 by sha256, the loader by cksum); a later run finds the copy and checks
#     that the NVMe still matches it.  A start of the NVMe's own zedBSD in between (5330-p027.sh) changes p5: such a
#     run takes a new BKNAME.
#  2. The image to the NVMe (5330-nvme-write.sh image), VMUNIX on p4, watchdog=WATCHDOG (default 600) added to its
#     zedbsd.cfg, zedBSD started once (UEFI BootNext): a zedBSD that stops is reset back to Debian.
#  3. The rounds (mview in the session, 12 s each, hw.gputelemetry samples), then the kernel's log: its i915 diag
#     lines, when the kernel has them, in LOGS/diag.txt.
#  4. Debian again; p4, p5, p6 and the loader written back from the kept copy and compared (but p4's log/); p4's
#     vmunix and zedbsd.cfg from the p027 copies, their sha256 checked.
# Nothing is removed on either machine.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
IMG=${1:?IMG}
VMUNIX=${2:?VMUNIX}
NAME=${3:?NAME}
shift 3
COPIES=${*:-1 600}
ROUNDS=${ROUNDS:-3}
cd "$(dirname "$0")/../../../.."
LOGS=${LOGS:-build/hw5330/ws333}/$NAME
WATCHDOG=${WATCHDOG:-600}
HOST=solaris10-man
NV_BOOT=/dev/disk/by-partuuid/d4dbe962-cc37-4f27-8529-bb7446730178
NV_ROOT=/dev/disk/by-partuuid/24c59452-7bc3-42e7-b532-5653d935c325
NV_SWAP=/dev/disk/by-partuuid/a26f52a1-fed3-4073-b577-1bc61322882f
MV=plan/ws291/tests/5330-mview.sh
mkdir -p "$LOGS"
SUMMARY=$LOGS/summary.txt
: > "$SUMMARY"
say() {
	echo "5330-p029-qs40: $*" | tee -a "$SUMMARY"
}
lin() {
	timeout "${TO:-120}" ssh -n -o BatchMode=yes -o ConnectTimeout=10 "$HOST" "$1"
}
# Runs a command under the hardware lock.
locked() {
	flock -w 3600 /tmp/i915-hw.lock "$@"
}
# Writes the kept NVMe zedBSD back and compares its sums with the kept ones.
nvme_restore() {
	TO=1800 locked timeout 1800 ssh -n -o BatchMode=yes "$HOST" "sudo -n sh -c 'set -e; gzip -dc $BK/p5.gz | dd of=$NV_ROOT bs=4M conv=fsync status=none; gzip -dc $BK/p6.gz | dd of=$NV_SWAP bs=4M conv=fsync status=none; mkdir -p /mnt/zb; while mountpoint -q /mnt/zb; do umount /mnt/zb; done; mount $NV_BOOT /mnt/zb; cp -r --preserve=timestamps $BK/p4/. /mnt/zb/; sync; umount /mnt/zb; cp --preserve=timestamps $BK/BOOTX64.EFI /boot/efi/EFI/zedbsd/BOOTX64.EFI; sync'" > "$LOGS/restore.txt" 2>&1
	sums sums-after.txt
	if cmp -s "$LOGS/sums-now.txt" "$LOGS/sums-after.txt"; then
		say "restore: the NVMe is the kept copy again ($(grep -c . "$LOGS/sums-after.txt") sums)"
	else
		say "restore: differs (p4 may hold files of the image, not removed): $(diff "$LOGS/sums-now.txt" "$LOGS/sums-after.txt" | tr '\n' ' ' | cut -c1-400)"
	fi
}
p4_restore() {
	TO=300 lin "sudo -n sh -c 'mkdir -p /mnt/zb && { while mountpoint -q /mnt/zb; do umount /mnt/zb; done; } && mount $NV_BOOT /mnt/zb && cp $DH/ws245/vmunix-p027-orig /mnt/zb/vmunix && cp $DH/ws245/zedbsd.cfg-p027-orig /mnt/zb/zedbsd.cfg && sync && sha256sum /mnt/zb/vmunix /mnt/zb/zedbsd.cfg && umount /mnt/zb'" > "$LOGS/p4-restore.txt" 2>&1
	say "p4: $(awk '{ printf "%s %s ", substr($1, 1, 16), $2 }' "$LOGS/p4-restore.txt")"
}

TO=15 lin 'uname -sr' | grep -q Linux || { say "FAIL: the 5330 is not on Debian"; exit 1; }
say "$NAME start $(date '+%F %T') on $(TO=15 lin 'uname -sr')"
DH=$(TO=15 lin 'echo $HOME')
TO=15 lin "test -f ws245/vmunix-p027-orig && test -f ws245/zedbsd.cfg-p027-orig" ||
	{ say "FAIL: the p027 copies of p4 are missing"; exit 1; }
BK=$DH/ws245/p029-nvme-${BKNAME:-p029}
# The sums of the NVMe zedBSD as it is now, into LOGS/FILE.
sums() {
	TO=1800 locked timeout 1800 ssh -n -o BatchMode=yes "$HOST" "sudo -n sh -c 'mkdir -p /mnt/zb && { while mountpoint -q /mnt/zb; do umount /mnt/zb; done; } && mount -o ro $NV_BOOT /mnt/zb && (cd /mnt/zb && find . -type f ! -path ./log/\\* | sort | xargs cksum); umount /mnt/zb; cksum /boot/efi/EFI/zedbsd/BOOTX64.EFI; for p in $NV_ROOT $NV_SWAP; do echo \$(sha256sum < \$p | cut -c1-16) \$(readlink -f \$p); done'" > "$LOGS/$1" 2>&1
}

# 1. The NVMe zedBSD kept once, or checked against the copy kept before (but p4's boot logs).
sums sums-now.txt
if TO=15 lin "test -f $BK/sums.txt"; then
	TO=15 lin "cat $BK/sums.txt" > "$LOGS/sums-kept.txt"
	if ! cmp -s "$LOGS/sums-kept.txt" "$LOGS/sums-now.txt"; then
		say "FAIL: the NVMe differs from the copy kept in $BK ($(diff "$LOGS/sums-kept.txt" "$LOGS/sums-now.txt" | tr '\n' ' ' | cut -c1-300))"
		exit 1
	fi
	say "the NVMe matches the copy kept in $BK (but p4's log/)"
else
	TO=1800 locked timeout 1800 ssh -n -o BatchMode=yes "$HOST" "set -e; mkdir -p $BK/p4; sudo -n sh -c 'mount -o ro $NV_BOOT /mnt/zb && cp -r --preserve=timestamps /mnt/zb/. $BK/p4/ && umount /mnt/zb; cp --preserve=timestamps /boot/efi/EFI/zedbsd/BOOTX64.EFI $BK/BOOTX64.EFI; dd if=$NV_ROOT bs=4M status=none | gzip -1 > $BK/p5.gz; dd if=$NV_SWAP bs=4M status=none | gzip -1 > $BK/p6.gz; sync'" > "$LOGS/backup.txt" 2>&1 ||
		{ say "FAIL: the backup ($LOGS/backup.txt)"; exit 1; }
	TO=60 timeout 60 scp -q "$LOGS/sums-now.txt" "$HOST:$BK/sums.txt" || exit 1
	say "the NVMe kept in $BK: $(TO=15 lin "du -sh $BK/p5.gz $BK/p6.gz" | tr '\n' ' ')"
fi
say "sums: $(grep -c . "$LOGS/sums-now.txt") lines, $(tail -3 "$LOGS/sums-now.txt" | tr '\n' ' ')"

# 2. The image, the kernel, the watchdog, zedBSD once.
TO=1200 timeout 1200 scp -q "$IMG" "$HOST:p1-p022/image.img" || { say "FAIL: copy of the image"; exit 1; }
TO=60 timeout 60 scp -q plan/tools/hw5330/5330-nvme-write.sh plan/tools/hw5330/ufs-grow.py "$HOST:ws245/" || exit 1
TO=120 timeout 120 scp -q "$VMUNIX" "$HOST:ws245/vmunix-p029-$NAME" || { say "FAIL: copy of the kernel"; exit 1; }
TO=1500 locked sh -c "timeout 1500 ssh -n -o BatchMode=yes $HOST 'sudo -n sh \$HOME/ws245/5330-nvme-write.sh image \$HOME/p1-p022/image.img && sudo -n sh \$HOME/ws245/5330-nvme-write.sh kernel \$HOME/ws245/vmunix-p029-$NAME'" > "$LOGS/write.txt" 2>&1 ||
	{ say "FAIL: the NVMe write ($LOGS/write.txt)"; nvme_restore; p4_restore; exit 1; }
TO=120 lin "sudo -n sh -c 'mkdir -p /mnt/zb && { while mountpoint -q /mnt/zb; do umount /mnt/zb; done; } && mount $NV_BOOT /mnt/zb && echo watchdog=$WATCHDOG >> /mnt/zb/zedbsd.cfg && cat /mnt/zb/zedbsd.cfg && sync && umount /mnt/zb'" > "$LOGS/cfg.txt" 2>&1
say "image $(sha256sum "$IMG" | cut -c1-16) and kernel $(sha256sum "$VMUNIX" | cut -c1-16) on the NVMe; $(grep -c watchdog "$LOGS/cfg.txt") watchdog line"

# 3. The rounds (5330-mview.sh starts zedBSD once by BootNext and waits for the session).
r=1
while [ "$r" -le "$ROUNDS" ]; do
	# shellcheck disable=SC2086
	timeout 2400 sh "$MV" run "$LOGS/r$r" $COPIES > "$LOGS/run-r$r.txt" 2>&1
	sed "s/^/r$r: /" "$LOGS/r$r/summary.txt" 2> /dev/null | grep -E 'SPIN|FAILED' | tee -a "$SUMMARY" > /dev/null
	r=$((r + 1))
done
T=60 timeout 60 ssh -i plan/tmp/guest/id_ed25519 -o BatchMode=yes -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR root@10.0.30.3 \
	'uname -v; dmesg | grep -E "watchdog: armed|gt pm:|DC_STATE|rps: (started|fixed)"' > "$LOGS/zedbsd.txt" 2>&1 < /dev/null
say "zedBSD: $(tr '\n' ';' < "$LOGS/zedbsd.txt")"
for what in dmesg 'cat /run/user/1000/session.log'; do
	T=60 timeout 60 ssh -i plan/tmp/guest/id_ed25519 -o BatchMode=yes -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR root@10.0.30.3 \
		"$what" > "$LOGS/after-$(echo "$what" | tr -c 'a-z\n' '_' | cut -c1-12).txt" 2>&1 < /dev/null
done
say "kernel: $(grep -h -i -E 'forcewake domain asleep|did not wake|did not complete|hang|reset|fault|lost|i915.*fail' "$LOGS"/after-dmesg*.txt | head -8 | tr '\n' ';')"
grep -h 'diag:' "$LOGS"/after-dmesg*.txt > "$LOGS/diag.txt" 2> /dev/null
say "diag: $(grep -c . "$LOGS/diag.txt") lines, $(grep -c 'slow item' "$LOGS/diag.txt") slow items, $(grep -c 'stream of' "$LOGS/diag.txt") slow streams"

# 4. Debian, the backup back, p4 as WS333 had it.
timeout 600 sh "$MV" linux > "$LOGS/linux.txt" 2>&1
TO=15 lin 'uname -sr' | grep -q Linux || { say "FAIL: Debian did not come back; the watchdog should reset zedBSD within ${WATCHDOG} s"; exit 2; }
nvme_restore
p4_restore
say "done $(date '+%F %T')"
