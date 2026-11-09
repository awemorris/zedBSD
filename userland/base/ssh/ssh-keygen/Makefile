# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

SSH_PACKAGE_DIR := $(dir $(lastword $(MAKEFILE_LIST)))
include $(SSH_PACKAGE_DIR)../../package.mk

# zedBSD's own SSH key generator (see ../Makefile).
include $(SSH_PACKAGE_DIR)../sources.mk

$(eval $(call USERLAND_PACKAGE,ssh-keygen,ssh key generator,*,n,basic,$(ZEDBSD_SSH_KEYGEN_SOURCES),y,base,base/ssh,base/libcrypto-compat,,program,,,usr/bin))
