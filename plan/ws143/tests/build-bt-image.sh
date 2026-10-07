#!/bin/sh
# ws143: builds the Bluetooth test image: plan/ws143/tests/config-amd64-bt.mk (the Files image with the test kernel's
# loopback controller, bluetoothd, bt, bt-probe and runas) and the test's accounts: /etc/passwd and /etc/group of the
# base with btuser (uid 1001), a user that is neither root nor in wheel nor the seat's user (ws143-p004 review S7), so the
# permission checks of bt-daemon-p003.sh and bt-pair-p004.sh have someone who must be refused.
#   plan/ws143/tests/build-bt-image.sh BUILD
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -eu
cd "$(dirname -- "$0")/../../.."
FILES_CONFIG=plan/ws143/tests/config-amd64-bt.mk \
FILES_EXTRA="--file /etc/passwd=plan/ws143/tests/passwd --file /etc/group=plan/ws143/tests/group" \
	exec plan/tools/files/build-files-image.sh "${1:-build/ws143-bt}"
