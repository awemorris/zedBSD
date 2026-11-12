#!/bin/sh
# ws333 p027: WS291 p022's qs40 x 600 on the 5330 with an image of this tree, end to end (check the backup, write the
# image, boot with the watchdog, run, collect, restore), with plan/ws291/tests/5330-mview.sh for its steps (each takes
# /tmp/i915-hw.lock itself; this script takes it only for its own steps between them).  The summary is
# LOGS/summary.txt, its last line "5330-p027-qs40: done" or "5330-p027-qs40: FAIL ...".
#   sh plan/ws333/tests/hw5330/5330-p027-qs40.sh IMG VMUNIX NAME [COPIES...]
# IMG is a disk image of config-5330-qs40.mk; VMUNIX the kernel put on p4 after the image (the image's own when the
# same).  COPIES defaults to "600 600".  The 5330 must be on Debian.
#  1. The NVMe's p5, p6 and the ESP loader must still be what ~/p1-p022/orig (5330-mview.sh backup, 2026-10-31)
#     holds, so 5330-mview.sh restore brings them back; p4's vmunix and zedbsd.cfg are the WS333 ones kept by
#     5330-p027.sh (~/ws245/*-p027-orig).
#  2. The image to the NVMe (5330-nvme-write.sh image), VMUNIX on p4, watchdog=WATCHDOG (default 600) added to its
#     zedbsd.cfg, zedBSD started once (UEFI BootNext): a zedBSD that stops is reset back to Debian.
#  3. 5330-mview.sh run LOGS/run COPIES (mview in the session, 12 s each, direct scanout, hw.gputelemetry samples).
#  4. Debian again; 5330-mview.sh restore (p4, p5, p6 and the loader from the backup, their sums compared); p4's
#     vmunix and zedbsd.cfg from the p027 copies, their sha256 checked.
# Nothing is removed on either machine.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
IMG=${1:?IMG}
VMUNIX=${2:?VMUNIX}
NAME=${3:?NAME}
shift 3
COPIES=${*:-600 600}
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
	echo "5330-p027-qs40: $*" | tee -a "$SUMMARY"
}
lin() {
	timeout "${TO:-120}" ssh -n -o BatchMode=yes -o ConnectTimeout=10 "$HOST" "$1"
}
# Runs a command under the hardware lock.
locked() {
	flock -w 3600 /tmp/i915-hw.lock "$@"
}
p4_restore() {
	TO=300 lin "sudo -n sh -c 'mkdir -p /mnt/zb && { while mountpoint -q /mnt/zb; do umount /mnt/zb; done; } && mount $NV_BOOT /mnt/zb && cp $DH/ws245/vmunix-p027-orig /mnt/zb/vmunix && cp $DH/ws245/zedbsd.cfg-p027-orig /mnt/zb/zedbsd.cfg && sync && sha256sum /mnt/zb/vmunix /mnt/zb/zedbsd.cfg && umount /mnt/zb'" > "$LOGS/p4-restore.txt" 2>&1
	say "p4: $(awk '{ printf "%s %s ", substr($1, 1, 16), $2 }' "$LOGS/p4-restore.txt")"
}

TO=15 lin 'uname -sr' | grep -q Linux || { say "FAIL: the 5330 is not on Debian"; exit 1; }
say "$NAME start $(date '+%F %T') on $(TO=15 lin 'uname -sr')"
DH=$(TO=15 lin 'echo $HOME')
TO=15 lin "test -f ws245/vmunix-p027-orig && test -f ws245/zedbsd.cfg-p027-orig && test -f p1-p022/orig/sums.txt" ||
	{ say "FAIL: the p027 copies or the p022 backup are missing"; exit 1; }

# 1. p5, p6 and the loader as the backup holds them.
TO=1200 locked sh -c "timeout 1200 ssh -n -o BatchMode=yes $HOST \"sudo -n sh -c 'cksum /boot/efi/EFI/zedbsd/BOOTX64.EFI; for p in $NV_ROOT $NV_SWAP; do echo \\\$(sha256sum < \\\$p | cut -c1-16) \\\$(readlink -f \\\$p); done'\"" > "$LOGS/sums-now.txt" 2>&1
TO=15 lin 'cat p1-p022/orig/sums.txt' > "$LOGS/sums-orig.txt"
if ! grep -F -x -f "$LOGS/sums-now.txt" "$LOGS/sums-orig.txt" | grep -c . | grep -qx 3; then
	say "FAIL: p5, p6 or the loader differ from the p022 backup (now: $(tr '\n' ' ' < "$LOGS/sums-now.txt"))"
	exit 1
fi
say "p5, p6 and the loader are the p022 backup's: $(tr '\n' ' ' < "$LOGS/sums-now.txt")"

# 2. The image, the kernel, the watchdog, zedBSD once.
TO=1200 timeout 1200 scp -q "$IMG" "$HOST:p1-p022/image.img" || { say "FAIL: copy of the image"; exit 1; }
TO=60 timeout 60 scp -q plan/tools/hw5330/5330-nvme-write.sh plan/tools/hw5330/ufs-grow.py "$HOST:ws245/" || exit 1
TO=120 timeout 120 scp -q "$VMUNIX" "$HOST:ws245/vmunix-p027-$NAME" || { say "FAIL: copy of the kernel"; exit 1; }
TO=1500 locked sh -c "timeout 1500 ssh -n -o BatchMode=yes $HOST 'sudo -n sh \$HOME/ws245/5330-nvme-write.sh image \$HOME/p1-p022/image.img && sudo -n sh \$HOME/ws245/5330-nvme-write.sh kernel \$HOME/ws245/vmunix-p027-$NAME'" > "$LOGS/write.txt" 2>&1 ||
	{ say "FAIL: the NVMe write ($LOGS/write.txt)"; TO=1800 sh "$MV" restore >> "$LOGS/restore.txt" 2>&1; p4_restore; exit 1; }
TO=120 lin "sudo -n sh -c 'mkdir -p /mnt/zb && { while mountpoint -q /mnt/zb; do umount /mnt/zb; done; } && mount $NV_BOOT /mnt/zb && echo watchdog=$WATCHDOG >> /mnt/zb/zedbsd.cfg && cat /mnt/zb/zedbsd.cfg && sync && umount /mnt/zb'" > "$LOGS/cfg.txt" 2>&1
say "image $(sha256sum "$IMG" | cut -c1-16) and kernel $(sha256sum "$VMUNIX" | cut -c1-16) on the NVMe; $(grep -c watchdog "$LOGS/cfg.txt") watchdog line"

# 3. The run (5330-mview.sh starts zedBSD once by BootNext and waits for the session).
# shellcheck disable=SC2086
timeout 2400 sh "$MV" run "$LOGS/run" $COPIES > "$LOGS/run.txt" 2>&1
sed 's/^/run: /' "$LOGS/run/summary.txt" 2> /dev/null | tee -a "$SUMMARY" > /dev/null
T=60 timeout 60 ssh -i plan/tmp/guest/id_ed25519 -o BatchMode=yes -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR root@10.0.30.3 \
	'uname -v; dmesg | grep -E "watchdog: armed|gt pm:|DC_STATE|rps: (started|fixed)"' > "$LOGS/zedbsd.txt" 2>&1 < /dev/null
say "zedBSD: $(tr '\n' ';' < "$LOGS/zedbsd.txt")"

# 4. Debian, the backup back, p4 as WS333 had it.
timeout 600 sh "$MV" linux > "$LOGS/linux.txt" 2>&1
TO=15 lin 'uname -sr' | grep -q Linux || { say "FAIL: Debian did not come back; the watchdog should reset zedBSD within ${WATCHDOG} s"; exit 2; }
timeout 1800 sh "$MV" restore > "$LOGS/restore.txt" 2>&1
say "restore: $(grep 'restore:' "$LOGS/restore.txt" | tr '\n' ' ')"
locked true
p4_restore
say "done $(date '+%F %T')"
