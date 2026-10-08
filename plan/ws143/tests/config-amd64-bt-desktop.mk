# ws143-p006: the image of the Bluetooth desktop's AAT (apps.settings.bluetooth-pair, desktop.bar.bluetooth-menu): the AAT
# image (plan/tools/aat/config-amd64-aat.mk) with the test kernel's loopback controller (CONFIG_BT_TEST_LOOPBACK:
# /dev/bluetooth0, the devices 0A:0B:0C:0D:0E:01 numeric comparison and :07 Just Works), bluetoothd and bt.  The base's
# /etc/passwd has _bluetooth; bluetoothd is started by the scenario (root).  A test image only.
# Build as plan/tools/aat/build-image.sh does, with this file in place of config-amd64-aat.mk
# (plan/tools/guest/test-image.sh --no-harness plan/ws143/tests/config-amd64-bt-desktop.mk BUILD and the harness's files).
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
include plan/tools/aat/config-amd64-aat.mk
CONFIG_BT_TEST_LOOPBACK := y
ZEDBSD_USER_PROGRAMS += bluetoothd bt
