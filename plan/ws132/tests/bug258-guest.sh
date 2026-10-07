#!/bin/sh
# BUG-258: a hot-plugged USB stick with a partition table, and a card reader whose card is not in LUN 0, become
# disks and volumes.  Runs on the Venus guest of plan/ws132/tests/config-amd64-p004.mk (started with
#   plan/tools/files/files-guest.sh start IMAGE), like p004-guest.sh.  No compositor: volumectl speaks to volumed.
#  1. The host plugs in a 64 MiB stick with an MBR and one FAT32 partition (label PARTSTICK, HELLO.TXT) through
#     usb-storage: the partition appears as /dev/sdX1 (the control worker read the table after the boot), volumed
#     lists "VOLUME id=sdX1 ... fs=fat ... label=PARTSTICK", kei mounts it and HELLO.TXT reads "hi".  Pulled out.
#  2. The host plugs in a usb-bot "card reader" whose LUN 0 is an empty CD drive (not a disk) and whose LUN 1 holds
#     a FAT card without partitions (label CARDLUN1): the kernel logs "2 LUNs; probing each for a medium" and
#     "LUN 1 of 2 has a medium", and volumed lists the volume CARDLUN1.  Pulled out.
# PASS: every "ok" line and the last line bug258: PASS.
#   plan/ws132/tests/bug258-guest.sh [OUTDIR]     (default build/ws132-bug258, a fresh run directory each time)
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
cd "$(dirname -- "$0")/../../.."
. plan/tools/fresh-out.sh
GUEST_RUNTIME="${GUEST_RUNTIME:-$(pwd)/build/ws071-run}"
export GUEST_RUNTIME
qmp="$GUEST_RUNTIME/qmp.sock"
fresh_out "${1:-build/ws132-bug258}"
out=$fresh_dir
guest() { timeout 60 python3 plan/tools/guest/guest.py run "$1" 2>&1 | tr -d '\r'; }
send() { timeout 40 python3 plan/ws049/tests/qmp-send.py "$qmp" "$@" >> "$out/qmp.txt" 2>&1; }
status=0
expect() {
	if grep -Eq "$2" "$3"; then echo "ok: $1"; else echo "FAIL: $1"; status=1; fi
}
refuse() {
	if grep -Eq "$2" "$3"; then echo "FAIL: $1"; status=1; else echo "ok: $1"; fi
}
: > "$out/qmp.txt"
printf 'hi\n' > "$out/hello.txt"

# The stick: an MBR with one FAT32 partition from 1 MiB.
stick="$out/stick.img"
truncate -s 64M "$stick"
printf 'label: dos\nstart=2048, type=c\n' | /sbin/sfdisk -q "$stick"
mformat -i "$stick@@1M" -F -c 1 -v PARTSTICK ::
mcopy -i "$stick@@1M" "$out/hello.txt" ::HELLO.TXT

# The card in the reader's LUN 1: FAT without partitions.
card="$out/card.img"
truncate -s 16M "$card"
mformat -i "$card" -T 32768 -h 2 -s 32 -v CARDLUN1 ::
mcopy -i "$card" "$out/hello.txt" ::HELLO.TXT

# volumed runs; nothing is listed yet.
guest 'service start volumed >/dev/null 2>&1; sleep 1; chown root /dev/gpu0; /bin/volumectl list' > "$out/list0.txt"
expect "volumed answers" '^DONE$' "$out/list0.txt"
refuse "no volume yet" '^VOLUME' "$out/list0.txt"

# 1.
send blockdev-add "{\"driver\":\"raw\",\"node-name\":\"stick0\",\"file\":{\"driver\":\"file\",\"filename\":\"$(realpath "$stick")\"}}"
send device_add '{"driver":"usb-storage","bus":"xhci.0","drive":"stick0","id":"stick"}'
sleep 5
guest 'ls /dev | grep "^sd"; /bin/volumectl list' > "$out/list1.txt"
expect "the partition is a disk" '^sd[a-z]1$' "$out/list1.txt"
expect "the partition is a volume" '^VOLUME id=sd[a-z]1 state=available fs=fat .*label=PARTSTICK path=- new=1$' "$out/list1.txt"
id=$(sed -n 's/^VOLUME id=\([a-z0-9]*\) .*label=PARTSTICK.*/\1/p' "$out/list1.txt" | head -1)
id=${id:-sdb1}
guest "chown kei /dev/gpu0; su kei -c '/bin/volumectl mount $id'; cat /media/PARTSTICK/HELLO.TXT; su kei -c '/bin/volumectl eject $id'; chown root /dev/gpu0" > "$out/mount1.txt"
expect "kei mounts the partition" '^RESULT 1 0$' "$out/mount1.txt"
expect "HELLO.TXT reads hi" '^hi$' "$out/mount1.txt"
send device_del '{"id":"stick"}'
sleep 4
send blockdev-del '{"node-name":"stick0"}'

# 2.
send blockdev-add "{\"driver\":\"raw\",\"node-name\":\"card0\",\"file\":{\"driver\":\"file\",\"filename\":\"$(realpath "$card")\"}}"
send device_add '{"driver":"usb-bot","bus":"xhci.0","id":"reader"}'
send device_add '{"driver":"scsi-cd","bus":"reader.0","scsi-id":0,"lun":0,"id":"reader-cd"}'
send device_add '{"driver":"scsi-hd","bus":"reader.0","scsi-id":0,"lun":1,"drive":"card0","removable":true,"id":"reader-card"}'
send qom-set '{"path":"reader","property":"attached","value":true}'
sleep 5
guest 'dmesg | grep "usb-storage" | tail -6; /bin/volumectl list' > "$out/list2.txt"
expect "the reader's LUNs are probed" 'usb-storage: 2 LUNs; probing each for a medium' "$out/list2.txt"
expect "the card is found in LUN 1" 'usb-storage: LUN 1 of 2 has a medium' "$out/list2.txt"
expect "the card is a volume" '^VOLUME id=sd[a-z] state=available fs=fat .*label=CARDLUN1 path=- new=1$' "$out/list2.txt"
send device_del '{"id":"reader"}'
sleep 4
send blockdev-del '{"node-name":"card0"}'
guest '/bin/volumectl list' > "$out/list3.txt"
refuse "the list is empty again" '^VOLUME' "$out/list3.txt"

[ $status = 0 ] && echo "bug258: PASS" || echo "bug258: FAIL"
exit $status
