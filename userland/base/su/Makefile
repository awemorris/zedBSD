include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

# su (ws160-p001), set-user-ID root.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
$(eval $(call USERLAND_PACKAGE,su,su,*,y,basic,userland/base/su/main.c userland/base/common/account.c userland/base/login/verify.c,y,base,,,4755))
