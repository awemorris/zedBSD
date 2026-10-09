include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

# zedBSD
# Copyright (C) 2026 Awe Morris
# SPDX-License-Identifier: Zlib

# The Bluetooth daemon (ws143-p003 and p004, plan/ws143/phase00[34]/phase.md): a root parent opens the controller's node
# /dev/btN for a child running as _bluetooth, which loads Intel's firmware when it needs it, sets up the HCI core, pairs
# (BR/EDR's Secure Simple Pairing, LE's Security Manager), keeps the bonds, and answers on /run/bluetoothd.sock.
BLUETOOTHD_SOURCES := userland/base/bluetoothd/main.c userland/base/bluetoothd/session.c userland/base/bluetoothd/hci.c \
	userland/base/bluetoothd/intel.c userland/base/bluetoothd/privsep.c userland/base/bluetoothd/pair.c \
	userland/base/bluetoothd/smp.c userland/base/bluetoothd/crypto.c userland/base/bluetoothd/acl.c \
	userland/base/bluetoothd/l2cap.c userland/base/bluetoothd/keys.c userland/base/bluetoothd/sdp.c \
	userland/base/bluetoothd/hidp.c userland/base/bluetoothd/att.c userland/base/bluetoothd/hidcache.c \
	userland/base/bluetoothd/snoop.c userland/base/bluetoothd/router.c userland/base/bluetoothd/linkmgr.c \
	userland/base/bluetoothd/hid.c \
	userland/base/bluetoothd/hog.c userland/base/bluetoothd/rfcomm.c userland/base/bluetoothd/obex.c \
	userland/base/bluetoothd/sdps.c userland/base/bluetoothd/phone.c
BLUETOOTHD_DATA := /etc/service.d/bluetoothd=userland/base/bluetoothd/bluetoothd.service
$(eval $(call ZEDBSD_USERLAND_PACKAGE,bluetoothd,Bluetooth daemon,amd64,n,basic,$(BLUETOOTHD_SOURCES),y,base,base/bluetoothd,,0755,program,$(BLUETOOTHD_DATA),,sbin))
