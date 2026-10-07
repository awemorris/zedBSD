include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

# zedBSD
# Copyright (C) 2026 Awe Morris
# SPDX-License-Identifier: Zlib

# The Bluetooth daemon (ws143-p003, plan/ws143/phase003/phase.md): opens the controller's node /dev/btN, loads Intel's
# firmware when it needs it, sets up the HCI core, and answers on /run/bluetoothd.sock (the state, a scan).
BLUETOOTHD_SOURCES := userland/base/bluetoothd/main.c userland/base/bluetoothd/session.c userland/base/bluetoothd/hci.c \
	userland/base/bluetoothd/intel.c
BLUETOOTHD_DATA := /etc/service.d/bluetoothd=userland/base/bluetoothd/bluetoothd.service
$(eval $(call ZEDBSD_USERLAND_PACKAGE,bluetoothd,Bluetooth daemon,amd64,n,basic,$(BLUETOOTHD_SOURCES),y,base,base/bluetoothd,,0755,program,$(BLUETOOTHD_DATA),,sbin))
