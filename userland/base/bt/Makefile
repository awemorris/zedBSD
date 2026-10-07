include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

# zedBSD
# Copyright (C) 2026 Awe Morris
# SPDX-License-Identifier: Zlib

# The Bluetooth command (ws143-p003, p004): bt show, scan, devices, pair, forget, bonds, agent, asked of bluetoothd on its socket.
$(eval $(call ZEDBSD_USERLAND_PACKAGE,bt,Bluetooth command,amd64,n,basic,userland/base/bt/main.c,y,base,base/bt,,0755,program,,,bin))
