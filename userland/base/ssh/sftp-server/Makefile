# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

SSH_PACKAGE_DIR := $(dir $(lastword $(MAKEFILE_LIST)))
include $(SSH_PACKAGE_DIR)../../package.mk

# The SFTP server that zedBSD's own sshd starts (see ../Makefile).
include $(SSH_PACKAGE_DIR)../sources.mk

$(eval $(call USERLAND_PACKAGE,sftp-server,SFTP server of sshd,*,n,basic,$(ZEDBSD_SFTP_SERVER_SOURCES),y,base,base/ssh,base/libcrypto-compat,,program,,,usr/libexec))
