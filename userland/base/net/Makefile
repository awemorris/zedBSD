include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

# zedBSD userland package
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
$(eval $(call USERLAND_PACKAGE,net,net,*,y,network,userland/base/net/main.c userland/base/net/protocol.c userland/base/net/netconf.c userland/base/net/reconcile.c userland/base/net/wifi-conf.c userland/base/net/wifi-store.c userland/base/libedit/readline.c userland/base/service/rcconf.c userland/base/service/service-config.c,n,base,base/net,base/libedit,0755,program,,,sbin))
