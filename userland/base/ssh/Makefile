# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

#
# zedBSD's own SSH 2 programs: the client ssh, the server sshd, the key
# generator ssh-keygen and the SFTP server sftp-server, on libcrypto-compat
# (mlkem768x25519-sha256, ssh-ed25519, chacha20-poly1305@openssh.com).  The
# one package ssh installs all four.
#
# Each program is its main file, its own program files and every library
# module (ssh-*.c other than the programs' own); sshd also takes the console
# login's password check.  The paths are from the top of the tree, which is
# where package.mk compiles them; the files are found from REPO_ROOT, since
# this make runs in the package's directory.
#
# They take the place of OpenSSH: the two install the same paths
# (/usr/bin/ssh, /usr/sbin/sshd, /usr/bin/ssh-keygen,
# /usr/libexec/sftp-server, /etc/ssh, /etc/service.d/sshd), so a
# configuration chooses one of them.
#

SSH_DIR := userland/base/ssh
ssh_files = $(sort $(patsubst $(REPO_ROOT)/%,%,$(wildcard $(REPO_ROOT)/$(SSH_DIR)/$(1))))

SSH_MODULES := $(filter-out %-main.c $(SSH_DIR)/ssh-client-%.c,$(call ssh_files,ssh-*.c))
SSH_FILES_DIR := $(SSH_DIR)/files

# [ssh] The client.
SSH_SOURCES := $(SSH_DIR)/ssh-main.c $(call ssh_files,ssh-client-*.c) $(SSH_MODULES)
SSH_DATA := /etc/ssh/ssh_config=$(SSH_FILES_DIR)/ssh_config
$(eval $(call USERLAND_PACKAGE,ssh,ssh secure shell client,*,n,basic,$(SSH_SOURCES),y,base,base/ssh,base/libcrypto-compat,,program,$(SSH_DATA),,usr/bin))

# [sshd] The server, with its configuration, its service and the startup
# script that makes the machine's host key.
SSHD_SOURCES := $(call ssh_files,sshd-*.c) userland/base/login/verify.c $(SSH_MODULES)
SSHD_DATA := \
	/etc/ssh/sshd_config=$(SSH_FILES_DIR)/sshd_config \
	/etc/service.d/sshd=$(SSH_FILES_DIR)/sshd.service \
	/usr/libexec/sshd-start=$(SSH_FILES_DIR)/sshd-start
$(eval $(call USERLAND_PACKAGE,sshd,ssh secure shell server,*,n,basic,$(SSHD_SOURCES),y,base,base/ssh,base/libcrypto-compat,,program,$(SSHD_DATA),,usr/sbin))

# [ssh-keygen] The key generator.
SSH_KEYGEN_SOURCES := $(SSH_DIR)/ssh-keygen-main.c $(SSH_MODULES)
$(eval $(call USERLAND_PACKAGE,ssh-keygen,ssh key generator,*,n,basic,$(SSH_KEYGEN_SOURCES),y,base,base/ssh,base/libcrypto-compat,,program,,,usr/bin))

# [sftp-server] The SFTP server that sshd starts.
SFTP_SERVER_SOURCES := $(SSH_DIR)/sftp-server-main.c $(SSH_MODULES)
$(eval $(call USERLAND_PACKAGE,sftp-server,SFTP server of sshd,*,n,basic,$(SFTP_SERVER_SOURCES),y,base,base/ssh,base/libcrypto-compat,,program,,,usr/libexec))
