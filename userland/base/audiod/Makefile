# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

#
# audiod: The sound server. Nixes clients' streams into /dev/dsp0 and
# hands out what it records.
#

include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

AUDIOD_SOURCES := \
	userland/base/audiod/main.c \
	userland/base/audiod/mix.c \
	userland/base/audiod/device.c \
	userland/base/audiod/route.c \
	userland/base/audiod/asrc.c

AUDIOD_DATA := \
	/etc/service.d/audiod=userland/base/audiod/audiod.service

$(eval $(call USERLAND_PACKAGE,audiod,audiod,*,y,basic,$(AUDIOD_SOURCES),y,base,base/audiod,,0755,program,$(AUDIOD_DATA),,sbin))
