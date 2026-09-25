# zedBSD
# Copyright (C) 2026 Awe Morris
# SPDX-License-Identifier: Zlib

# The sound server: mixes clients' streams into /dev/dsp0 and hands out
# what it records (plan/ws035/audiod-design.md).
AUDIOD_SOURCES := userland/base/audiod/main.c userland/base/audiod/mix.c \
	userland/base/audiod/device.c
AUDIOD_DATA := /etc/service.d/audiod=userland/base/audiod/audiod.service
$(eval $(call ZEDBSD_USERLAND_PACKAGE,audiod,sound server,*,y,basic,$(AUDIOD_SOURCES),y,base,base/audiod,,0755,program,$(AUDIOD_DATA),,sbin))
