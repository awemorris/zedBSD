include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

# Captures frames from /dev/camN and writes them as JPEG or PPM (test images only).
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
CAMCAP_SOURCES := userland/tests/camcap/main.c userland/tests/camcommon/camcommon.c
$(eval $(call USERLAND_PACKAGE,camcap,camera capture test,*,n,basic,$(CAMCAP_SOURCES),y,tests,tests/camcap,,0755,program,,,bin))
