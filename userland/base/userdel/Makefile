include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

# userdel (ws196-p003; docs/reference/user-administration.md): root's front end to account-admin, in /sbin, not set-user-ID.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
$(eval $(call USERLAND_PACKAGE,userdel,userdel,*,y,basic,userland/base/userdel/main.c userland/base/user-control/front.c,y,base,,,0755,program,,,sbin))
