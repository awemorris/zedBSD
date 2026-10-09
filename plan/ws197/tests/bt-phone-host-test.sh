#!/bin/sh
# The host test of bluetoothd's phone link parts (ws197-p002, plan/ws197/phase002/phase.md section 12.1): builds
# userland/base/bluetoothd's RFCOMM, OBEX, SDP server, session, router, link manager, phone pairing and HID host parts with the host's compiler under ASan
# and UBSan and runs plan/ws197/tests/bt-phone-host-test.c and the others below (the bonds of the pairing's test go in a new folder of the
# build).  The last line is "bt-phone-host-test: PASS" or "... FAIL".
# usage: plan/ws197/tests/bt-phone-host-test.sh   (from the repository's top; OUT= to choose the build folder)
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
OUT=${OUT:-build/ws197-phone-host}
mkdir -p "$OUT"
keys=$(mktemp -d "$OUT/pairkeys.XXXXXX")
flags="-std=gnu11 -D_GNU_SOURCE -Wall -Wextra -Werror -Wdeclaration-after-statement -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all -Iinclude -I."
status=0
# Each test program: its name and its sources besides the test.
for test in "bt-phone-host-test userland/base/bluetoothd/rfcomm.c" \
	"bt-phonerec-host-test userland/base/bluetoothd/phonerec.c userland/base/bluetoothd/keys.c userland/base/bluetoothd/hci.c" \
	"bt-outq-host-test userland/base/bluetoothd/outq.c" \
	"bt-phoneio-host-test userland/base/bluetoothd/phoneio.c" \
	"bt-mapxml-host-test userland/base/bluetoothd/mapxml.c" \
	"bt-bmsg-host-test userland/base/bluetoothd/bmsg.c userland/base/bluetoothd/mapxml.c" \
	"bt-obex-host-test userland/base/bluetoothd/obex.c" \
	"bt-sdp-host-test userland/base/bluetoothd/sdps.c userland/base/bluetoothd/sdp.c" \
	"bt-l2cap-move-host-test userland/base/bluetoothd/l2cap.c" \
	"bt-session-host-test userland/base/bluetoothd/session.c userland/base/bluetoothd/hci.c userland/base/bluetoothd/intel.c userland/base/bluetoothd/acl.c -lpthread" \
	"bt-router-host-test userland/base/bluetoothd/router.c userland/base/bluetoothd/linkmgr.c userland/base/bluetoothd/session.c userland/base/bluetoothd/pair.c userland/base/bluetoothd/hci.c userland/base/bluetoothd/intel.c userland/base/bluetoothd/acl.c userland/base/bluetoothd/l2cap.c userland/base/bluetoothd/smp.c userland/base/bluetoothd/crypto.c userland/base/bluetoothd/keys.c -lpthread" \
	"bt-pair-phone-host-test userland/base/bluetoothd/pair.c userland/base/bluetoothd/linkmgr.c userland/base/bluetoothd/session.c userland/base/bluetoothd/hci.c userland/base/bluetoothd/intel.c userland/base/bluetoothd/acl.c userland/base/bluetoothd/l2cap.c userland/base/bluetoothd/smp.c userland/base/bluetoothd/crypto.c userland/base/bluetoothd/keys.c -lpthread" \
	"bt-linkuse-host-test userland/base/bluetoothd/hid.c userland/base/bluetoothd/session.c userland/base/bluetoothd/pair.c userland/base/bluetoothd/router.c userland/base/bluetoothd/linkmgr.c userland/base/bluetoothd/hci.c userland/base/bluetoothd/intel.c userland/base/bluetoothd/acl.c userland/base/bluetoothd/l2cap.c userland/base/bluetoothd/smp.c userland/base/bluetoothd/crypto.c userland/base/bluetoothd/keys.c userland/base/bluetoothd/sdp.c userland/base/bluetoothd/hidp.c userland/base/bluetoothd/hidcache.c userland/base/bluetoothd/hog.c userland/base/bluetoothd/att.c -lpthread" \
	"bt-phone-link-host-test userland/base/bluetoothd/phone.c userland/base/bluetoothd/phonerec.c userland/base/bluetoothd/rfcomm.c userland/base/bluetoothd/obex.c userland/base/bluetoothd/sdps.c userland/base/bluetoothd/snoop.c userland/base/bluetoothd/hid.c userland/base/bluetoothd/session.c userland/base/bluetoothd/pair.c userland/base/bluetoothd/router.c userland/base/bluetoothd/linkmgr.c userland/base/bluetoothd/hci.c userland/base/bluetoothd/intel.c userland/base/bluetoothd/acl.c userland/base/bluetoothd/l2cap.c userland/base/bluetoothd/smp.c userland/base/bluetoothd/crypto.c userland/base/bluetoothd/keys.c userland/base/bluetoothd/sdp.c userland/base/bluetoothd/hidp.c userland/base/bluetoothd/hidcache.c userland/base/bluetoothd/hog.c userland/base/bluetoothd/att.c -lpthread"; do
	set -- $test
	name=$1
	shift
	if ! cc $flags -o "$OUT/$name" "plan/ws197/tests/$name.c" "$@"; then
		echo "$name: build FAILED"
		status=1
		continue
	fi
	if ! timeout 300 "$OUT/$name" "$keys"; then
		status=1
	fi
done
if [ $status -eq 0 ]; then
	echo "bt-phone-host-test: PASS"
else
	echo "bt-phone-host-test: FAIL"
	exit 1
fi
