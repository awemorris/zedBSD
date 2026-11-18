include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

# Runs a command as another user (WS131 p011, T2-021): the tests start an application in a user's desktop session from
# root's SSH, and the compositor serves its system extension only to its own user (plan/ws131/design.md section 4.1,
# D5).  Only for test images.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
RUNAS_SOURCES := userland/tests/runas/main.c
$(eval $(call USERLAND_PACKAGE,runas,run a command as another user,*,n,basic,$(RUNAS_SOURCES),y,tests,tests/runas,,0755,program,,,bin))
