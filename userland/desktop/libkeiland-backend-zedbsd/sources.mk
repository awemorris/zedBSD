# The zedBSD side of libkeiland-backend (WS131, plan/ws131/design.md section 3.6): the compositor's
# operating system, linked into the compositor.  This fragment is not a package Makefile (the top-level
# Makefile includes every userland/*/*/Makefile as a package); the compositor's Makefile includes it.  Since
# ws131-p011 nothing else links it: libkeiland reaches the system only through the compositor.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
KL_BACKEND_ZEDBSD_SOURCES := userland/desktop/libkeiland-backend/backend.c userland/desktop/libkeiland-backend/media/media.c \
	userland/desktop/libkeiland-backend/print/print.c \
	userland/desktop/libkeiland-backend/machine/machine.c \
	userland/desktop/libkeiland-backend/machine/filesystems.c \
	userland/desktop/libkeiland-backend/machine/users.c \
	userland/desktop/libkeiland-backend/machine/mounts.c \
	userland/desktop/libkeiland-backend/machine/mounts-mntent.c \
	userland/desktop/libkeiland-backend-zedbsd/network-zedbsd.c \
	userland/desktop/libkeiland-backend-zedbsd/network-link-zedbsd.c \
	userland/desktop/libkeiland-backend-zedbsd/monitor-zedbsd.c \
	userland/desktop/libkeiland-backend-zedbsd/audio-zedbsd.c \
	userland/desktop/libkeiland-backend-zedbsd/audio-stream-zedbsd.c \
	userland/base/net/protocol.c userland/base/net/wifi-conf.c userland/base/net/wifi-store.c userland/base/net/netconf.c \
	userland/desktop/libkeiland-backend-zedbsd/sharing-zedbsd.c userland/base/common/sha256.c \
	userland/desktop/libkeiland-backend-zedbsd/power-zedbsd.c userland/desktop/libkeiland-backend-zedbsd/power-outcome.c \
	userland/desktop/libkeiland-backend-zedbsd/backlight-zedbsd.c \
	userland/desktop/libkeiland-backend-zedbsd/volume-zedbsd.c \
	userland/desktop/libkeiland-backend-zedbsd/bluetooth-zedbsd.c \
	userland/desktop/libkeiland-backend-zedbsd/phone-zedbsd.c userland/desktop/libmms/mms.c \
	userland/desktop/libkeiland-backend-zedbsd/events-zedbsd.c \
	userland/desktop/libkeiland-backend-zedbsd/account-zedbsd.c \
	userland/desktop/libkeiland-backend-zedbsd/session-zedbsd.c \
	userland/desktop/libkeiland-backend/unsupported/seat-unsupported.c \
	userland/desktop/libkeiland-backend-zedbsd/input-zedbsd.c \
	userland/desktop/libkeiland-backend-zedbsd/display-zedbsd.c \
	userland/desktop/libkeiland-backend/peer/peer-getpeereid.c \
	userland/desktop/libkeiland-backend-zedbsd/peer-zedbsd.c \
	userland/desktop/libkeiland-backend-zedbsd/gpu-zedbsd.c \
	userland/desktop/libkeiland-backend-zedbsd/gpu-buffer-zedbsd.c \
	userland/desktop/libkeiland-backend-zedbsd/scanout-zedbsd.c
