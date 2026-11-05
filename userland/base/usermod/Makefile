include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

# usermod (ws196-p003; docs/reference/user-administration.md): root's front end to account-admin, in /sbin, not set-user-ID.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
$(eval $(call USERLAND_PACKAGE,usermod,usermod,*,y,basic,userland/base/usermod/main.c userland/base/user-control/front.c,y,base,,,0755,program,,,sbin))
