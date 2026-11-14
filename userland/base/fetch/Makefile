# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

# fetch's https is libssl-compat and libcrypto-compat, zedBSD's own TLS libraries, which the program links.

FETCH_SOURCES := \
	userland/base/fetch/main.c

$(eval $(call USERLAND_PACKAGE,fetch,fetch,*,y,network,$(FETCH_SOURCES),y,base,base/fetch,base/libssl-compat base/libcrypto-compat,,program,,,bin))
