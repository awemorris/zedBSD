#!/bin/sh
# The host tests of bluetoothd (ws143-p003, ws143-p004): builds userland/base/bluetoothd's parts with the host's compiler
# (under ASan and UBSan) and runs plan/ws143/tests/bt-daemon-host-test.c (p003: the parsers, the Intel load's plan, a
# scripted controller's sessions, a fixed-seed fuzz) and bt-pair-host-test.c (p004: the cryptography against FIPS-197,
# RFC 4493 and the Core's sample data, ACL, L2CAP, SMP against a scripted responder, the bonds, a fuzz).  The synthetic
# firmware files and the bonds go in new folders of the build.
# usage: plan/ws143/tests/bt-daemon-host-test.sh   (from the repository's top; OUT= to choose the build folder)
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -eu
OUT=${OUT:-build/ws143-bt-daemon-host}
mkdir -p "$OUT"
firmware=$(mktemp -d "$OUT/firmware.XXXXXX")
bonds=$(mktemp -d "$OUT/bonds.XXXXXX")
flags="-std=gnu11 -D_GNU_SOURCE -Wall -Wextra -Werror -Wdeclaration-after-statement -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all -Iinclude -I."
cc $flags -o "$OUT/bt-daemon-host-test" plan/ws143/tests/bt-daemon-host-test.c \
	userland/base/bluetoothd/hci.c userland/base/bluetoothd/intel.c userland/base/bluetoothd/session.c \
	userland/base/bluetoothd/acl.c -lpthread
timeout 120 "$OUT/bt-daemon-host-test" "$firmware"
cc $flags -o "$OUT/bt-pair-host-test" plan/ws143/tests/bt-pair-host-test.c userland/base/bluetoothd/crypto.c \
	userland/base/bluetoothd/acl.c userland/base/bluetoothd/l2cap.c userland/base/bluetoothd/smp.c \
	userland/base/bluetoothd/keys.c userland/base/bluetoothd/hci.c
timeout 120 "$OUT/bt-pair-host-test" "$bonds"
links=$(mktemp -d "$OUT/links.XXXXXX")
cc $flags -o "$OUT/bt-link-host-test" plan/ws143/tests/bt-link-host-test.c userland/base/bluetoothd/session.c \
	userland/base/bluetoothd/pair.c userland/base/bluetoothd/hci.c userland/base/bluetoothd/intel.c \
	userland/base/bluetoothd/acl.c userland/base/bluetoothd/l2cap.c userland/base/bluetoothd/smp.c \
	userland/base/bluetoothd/crypto.c userland/base/bluetoothd/keys.c userland/base/bluetoothd/router.c -lpthread
timeout 120 "$OUT/bt-link-host-test" "$links"
hid=$(mktemp -d "$OUT/hid.XXXXXX")
cc $flags -o "$OUT/bt-hid-host-test" plan/ws143/tests/bt-hid-host-test.c userland/base/bluetoothd/sdp.c \
	userland/base/bluetoothd/hidp.c userland/base/bluetoothd/att.c userland/base/bluetoothd/hidcache.c \
	userland/base/bluetoothd/snoop.c userland/base/bluetoothd/keys.c userland/base/bluetoothd/hci.c
timeout 120 "$OUT/bt-hid-host-test" "$hid"
hidhost=$(mktemp -d "$OUT/hidhost.XXXXXX")
cc $flags -o "$OUT/bt-hidhost-host-test" plan/ws143/tests/bt-hidhost-host-test.c userland/base/bluetoothd/hid.c \
	userland/base/bluetoothd/session.c userland/base/bluetoothd/pair.c userland/base/bluetoothd/router.c \
	userland/base/bluetoothd/hci.c userland/base/bluetoothd/intel.c userland/base/bluetoothd/acl.c \
	userland/base/bluetoothd/l2cap.c userland/base/bluetoothd/smp.c userland/base/bluetoothd/crypto.c \
	userland/base/bluetoothd/keys.c userland/base/bluetoothd/sdp.c userland/base/bluetoothd/hidp.c \
	userland/base/bluetoothd/hidcache.c userland/base/bluetoothd/hog.c userland/base/bluetoothd/att.c -lpthread
timeout 300 "$OUT/bt-hidhost-host-test" "$hidhost"
cc $flags -o "$OUT/bt-hog-host-test" plan/ws143/tests/bt-hog-host-test.c userland/base/bluetoothd/hog.c \
	userland/base/bluetoothd/att.c
timeout 60 "$OUT/bt-hog-host-test"
