# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

SSH_PACKAGE_DIR := $(dir $(lastword $(MAKEFILE_LIST)))
include $(SSH_PACKAGE_DIR)../package.mk

# zedBSD's own SSH 2 client, on libcrypto-compat (mlkem768x25519-sha256,
# ssh-ed25519, chacha20-poly1305@openssh.com).  The server, the key generator
# and the SFTP server are the packages in sshd/, ssh-keygen/ and sftp-server/
# beside this one, built from the same sources (sources.mk).  A configuration
# chooses all four with the one name ssh (mk/userland.mk).
#
# They are base programs that take the place of OpenSSH: the two install the
# same paths (/usr/bin/ssh, /usr/sbin/sshd, /usr/bin/ssh-keygen,
# /usr/libexec/sftp-server, /etc/ssh, /etc/service.d/sshd), so a
# configuration chooses one of them.

include $(SSH_PACKAGE_DIR)sources.mk

SSH_DATA := /etc/ssh/ssh_config=$(ZEDBSD_SSH_FILES_DIR)/ssh_config

$(eval $(call USERLAND_PACKAGE,ssh,ssh secure shell client,*,n,basic,$(ZEDBSD_SSH_SOURCES),y,base,base/ssh,base/libcrypto-compat,,program,$(SSH_DATA),,usr/bin))
