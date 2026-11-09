# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

SSH_PACKAGE_DIR := $(dir $(lastword $(MAKEFILE_LIST)))
include $(SSH_PACKAGE_DIR)../../package.mk

# zedBSD's own SSH 2 server (see ../Makefile), with its configuration, its
# service and the startup script that makes the machine's host key.
include $(SSH_PACKAGE_DIR)../sources.mk

SSHD_DATA := \
	/etc/ssh/sshd_config=$(ZEDBSD_SSH_FILES_DIR)/sshd_config \
	/etc/service.d/sshd=$(ZEDBSD_SSH_FILES_DIR)/sshd.service \
	/usr/libexec/sshd-start=$(ZEDBSD_SSH_FILES_DIR)/sshd-start

$(eval $(call USERLAND_PACKAGE,sshd,ssh secure shell server,*,n,basic,$(ZEDBSD_SSHD_SOURCES),y,base,base/ssh,base/libcrypto-compat,,program,$(SSHD_DATA),,usr/sbin))
