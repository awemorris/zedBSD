# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

#
# The Bluetooth daemon
#

BLUETOOTHD_SOURCES := \
	userland/base/bluetoothd/main.c \
	userland/base/bluetoothd/session.c \
	userland/base/bluetoothd/hci.c \
	userland/base/bluetoothd/intel.c \
	userland/base/bluetoothd/broadcom.c \
	userland/base/bluetoothd/privsep.c \
	userland/base/bluetoothd/pair.c \
	userland/base/bluetoothd/smp.c \
	userland/base/bluetoothd/crypto.c \
	userland/base/bluetoothd/acl.c \
	userland/base/bluetoothd/l2cap.c \
	userland/base/bluetoothd/keys.c \
	userland/base/bluetoothd/sdp.c \
	userland/base/bluetoothd/hidp.c \
	userland/base/bluetoothd/att.c \
	userland/base/bluetoothd/hidcache.c \
	userland/base/bluetoothd/snoop.c \
	userland/base/bluetoothd/router.c \
	userland/base/bluetoothd/linkmgr.c \
	userland/base/bluetoothd/hid.c \
	userland/base/bluetoothd/hog.c \
	userland/base/bluetoothd/rfcomm.c \
	userland/base/bluetoothd/obex.c \
	userland/base/bluetoothd/sdps.c \
	userland/base/bluetoothd/phone.c \
	userland/base/bluetoothd/phonerec.c \
	userland/base/bluetoothd/outq.c \
	userland/base/bluetoothd/phoneio.c \
	userland/base/bluetoothd/mapxml.c \
	userland/base/bluetoothd/bmsg.c \
	userland/base/bluetoothd/mms.c \
	userland/base/bluetoothd/map.c \
	userland/base/bluetoothd/vcard.c \
	userland/base/bluetoothd/phonemux.c \
	userland/base/bluetoothd/pbap.c \
	userland/base/bluetoothd/hfp.c \
	userland/base/bluetoothd/at.c \
	userland/desktop/libmms/mms.c

BLUETOOTHD_DATA := /etc/service.d/bluetoothd=userland/base/bluetoothd/bluetoothd.service

$(eval $(call USERLAND_PACKAGE,bluetoothd,bluetoothd,*,n,basic,$(BLUETOOTHD_SOURCES),y,base,base/bluetoothd,,0755,program,$(BLUETOOTHD_DATA),,sbin))
