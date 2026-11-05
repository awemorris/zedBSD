include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

# sudo (ws160-p001), set-user-ID root.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
$(eval $(call USERLAND_PACKAGE,sudo,sudo,*,y,basic,userland/base/sudo/main.c userland/base/common/account.c userland/base/login/verify.c,y,base,,,4755))
