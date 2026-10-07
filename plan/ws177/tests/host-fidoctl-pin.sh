#!/bin/sh
# ws177-p006: builds and runs the host test of fidoctl's PIN reading (host-fidoctl-pin.c takes in
# userland/base/fidoctl/main.c) with the libpasskey sources fidoctl's host test links, under ASan and UBSan, in a new
# directory under build/tmp (nothing is removed here; Q1's plan/tools/q1-clean.sh removes old runs).
#   sh plan/ws177/tests/host-fidoctl-pin.sh
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -eu
repo=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)
. "$repo/plan/tools/fresh-out.sh"
fresh_out "$repo/build/tmp/ws177-fidoctl-pin"
work=$fresh_dir
LIB=$repo/userland/base/libpasskey
cd "$repo"
cc -std=c11 -D_DEFAULT_SOURCE -Wall -Wextra -Werror -Wno-unused-function -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all -I. \
	-o "$work/pin" plan/ws177/tests/host-fidoctl-pin.c $LIB/cbor.c $LIB/crypto-openssl.c $LIB/ctap2.c $LIB/descriptor.c \
	$LIB/hid.c $LIB/os-posix.c $LIB/os-linux.c $LIB/pin.c $LIB/transport-nfc.c $LIB/verify.c -lcrypto
timeout 20 "$work/pin" < /dev/null
