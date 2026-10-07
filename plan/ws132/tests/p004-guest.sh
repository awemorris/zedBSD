#!/bin/sh
# ws132-p004: volumed on the Venus guest of plan/ws132/tests/config-amd64-p004.mk (started with
#   plan/tools/files/files-guest.sh start IMAGE).  No compositor: the test speaks to volumed with volumectl.
#  1. volumed runs (the boot's service) and lists no volume.
#  2. The host plugs in a FAT stick (16 MiB, label USBSTICK, HELLO.TXT and RUN.SH) on a free xhci port (QEMU chooses: the Venus guest's tablet takes port 4, T1-137): volumed lists
#     "VOLUME id=sd? state=available fs=fat ... label=USBSTICK path=- new=1" and mounts nothing by itself.
#  3. kei (uid 1000) may not mount while root owns the seat's display (RESULT EACCES, zedBSD's 25); with /dev/gpu0 given to
#     kei (as sessiond does), kei mounts it: RESULT 0, /media/USBSTICK, HELLO.TXT reads "hi", its files show kei as
#     their owner and kei can write a file there, the mount is nosuid and noexec (mount's list) and RUN.SH does not
#     run (Permission denied) although its mode says executable.
#  4. While a shell has its current folder there the eject answers EBUSY (zedBSD's 17) with user=sh; afterwards the
#     eject succeeds (RESULT 0), the folder is gone and the volume is available again (new=0).
#  5. Mounted again, the host pulls the stick out: volumed logs REMOVE forced=1, the folder is gone, the list is empty.
# PASS: every "ok" line and the last line p004: PASS.
#   plan/ws132/tests/p004-guest.sh [OUTDIR]     (default build/ws132-p004)
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
cd "$(dirname -- "$0")/../../.."
GUEST_RUNTIME="${GUEST_RUNTIME:-$(pwd)/build/ws071-run}"
export GUEST_RUNTIME
qmp="$GUEST_RUNTIME/qmp.sock"
out=${1:-build/ws132-p004}
mkdir -p "$out"
guest() { timeout 60 python3 plan/tools/guest/guest.py run "$1" 2>&1 | tr -d '\r'; }
send() { timeout 40 python3 plan/ws049/tests/qmp-send.py "$qmp" "$@" >> "$out/qmp.txt" 2>&1; }
status=0
# zedBSD's errno numbers are its own (EACCES 25, EBUSY 17; T1-139): read from the tree's header.
errno_of() { sed -n "s/^#define $1 \([0-9]*\).*/\1/p" include/uapi/errno.h | head -1; }
eacces=$(errno_of EACCES)
ebusy=$(errno_of EBUSY)
expect() {
	if grep -Eq "$2" "$3"; then echo "ok: $1"; else echo "FAIL: $1"; status=1; fi
}
refuse() {
	if grep -Eq "$2" "$3"; then echo "FAIL: $1"; status=1; else echo "ok: $1"; fi
}

# The stick: FAT without partitions, a text and a script.
stick="$out/stick.img"
: > "$stick"   # emptied, not removed (deleting is Q1's step, 2026-10-07)
truncate -s 16M "$stick"
mformat -i "$stick" -T 32768 -h 2 -s 32 -v USBSTICK ::
printf 'hi\n' > "$out/hello.txt"
printf '#!/bin/sh\necho ran\n' > "$out/run.sh"
mcopy -i "$stick" "$out/hello.txt" ::HELLO.TXT
mcopy -i "$stick" "$out/run.sh" ::RUN.SH
: > "$out/qmp.txt"

# 1.
guest 'service start volumed >/dev/null 2>&1; sleep 1; chown root /dev/gpu0; /bin/volumectl list' > "$out/list0.txt"
expect "volumed answers" '^DONE$' "$out/list0.txt"
refuse "no volume yet" '^VOLUME' "$out/list0.txt"

# 2.
send blockdev-add "{\"driver\":\"raw\",\"node-name\":\"stick0\",\"file\":{\"driver\":\"file\",\"filename\":\"$(realpath "$stick")\"}}"
send device_add '{"driver":"usb-storage","bus":"xhci.0","drive":"stick0","id":"stick"}'
sleep 4
guest '/bin/volumectl list; ls /media' > "$out/list1.txt"
expect "the stick is listed, available and new" '^VOLUME id=sd[a-z] state=available fs=fat size=16777216 label=USBSTICK path=- new=1$' "$out/list1.txt"
refuse "nothing is mounted by itself" 'USBSTICK$' "$out/list1.txt"
id=$(sed -n 's/^VOLUME id=\([a-z0-9]*\) .*label=USBSTICK.*/\1/p' "$out/list1.txt" | head -1)
id=${id:-sda}

# 3.
guest "su kei -c '/bin/volumectl mount $id'" > "$out/mount-refused.txt"
expect "kei may not mount without the seat (EACCES=$eacces)" "^RESULT 1 $eacces\$" "$out/mount-refused.txt"
guest "chown kei /dev/gpu0; su kei -c '/bin/volumectl mount $id'; ls -ln /media/USBSTICK; cat /media/USBSTICK/HELLO.TXT; su kei -c 'echo w > /media/USBSTICK/KEI.TXT' && echo wrote; mount | grep USBSTICK; /media/USBSTICK/RUN.SH; echo run=\$?" > "$out/mount.txt"
expect "kei mounts it" '^RESULT 1 0$' "$out/mount.txt"
expect "the volume is mounted under /media" 'state=mounted .*path=/media/USBSTICK new=0' "$out/mount.txt"
expect "HELLO.TXT reads hi" '^hi$' "$out/mount.txt"
expect "its files show kei (1000) as their owner" ' 1000 +1000 .*[Hh][Ee][Ll][Ll][Oo]\.[Tt][Xx][Tt]$' "$out/mount.txt"
expect "kei can write there" '^wrote$' "$out/mount.txt"
expect "the mount is nosuid and noexec" 'USBSTICK.*nosuid.*noexec|USBSTICK.*noexec.*nosuid' "$out/mount.txt"
refuse "RUN.SH does not run" '^ran$' "$out/mount.txt"
expect "the exec is refused" '^run=(126|1)$' "$out/mount.txt"

# 4.
guest "(cd /media/USBSTICK && sleep 6) & sleep 1; su kei -c '/bin/volumectl eject $id'; wait; su kei -c '/bin/volumectl eject $id'; ls /media" > "$out/eject.txt"
expect "a busy eject answers EBUSY with the program (EBUSY=$ebusy)" "^RESULT 1 $ebusy user=(sh|sleep)" "$out/eject.txt"
expect "the eject succeeds afterwards" '^RESULT 1 0$' "$out/eject.txt"
expect "the volume is available again" 'state=available .*path=- new=0' "$out/eject.txt"
refuse "the folder is gone" '^USBSTICK$' "$out/eject.txt"

# 5.
guest "su kei -c '/bin/volumectl mount $id' >/dev/null; ls /media" > "$out/remount.txt"
expect "mounted again" '^USBSTICK$' "$out/remount.txt"
send device_del '{"id":"stick"}'
sleep 4
send blockdev-del '{"node-name":"stick0"}'
guest '/bin/volumectl list; ls /media; echo media-end; dmesg | grep "unmount: /media" | tail -2' > "$out/pulled.txt"
refuse "the list is empty" '^VOLUME' "$out/pulled.txt"
refuse "the folder is gone after the pull" '^USBSTICK$' "$out/pulled.txt"
guest 'chown root /dev/gpu0; tail -20 /var/log/messages 2>/dev/null | grep VOLUMED' > "$out/volumed-log.txt"

[ $status = 0 ] && echo "p004: PASS" || echo "p004: FAIL"
exit $status
