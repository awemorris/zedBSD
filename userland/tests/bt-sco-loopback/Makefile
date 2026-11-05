include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

# ws197-p007a2 L2/L3: streams SCO through a Bluetooth controller's HCI Local Loopback and measures what comes back
# (test images only; bluetoothd must be stopped, since the node has one open at a time).
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
$(eval $(call USERLAND_PACKAGE,bt-sco-loopback,Bluetooth SCO loopback test,x86_64,n,basic,userland/tests/bt-sco-loopback/main.c,y,tests,tests/bt-sco-loopback,,0755,program,,,bin))
