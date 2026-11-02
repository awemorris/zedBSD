include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

# zedBSD's own SSH 2 programs: ssh, sshd, ssh-keygen and sftp-server, on libcrypto-compat (mlkem768x25519-sha256,
# ssh-ed25519, chacha20-poly1305@openssh.com).  Each program is its main file, its own program files and every
# library module (ssh-*.c other than the programs' own); sshd also takes the console login's password check.  They
# are linked by their own rules in platform/common/userland-dynamic.mk (libcrypto-compat.so and libc.so).
#
# They sit in the test menu and are off by default while OpenSSH is the image's: the two install the same paths
# (/usr/bin/ssh, /usr/sbin/sshd, /usr/libexec/sftp-server, /etc/ssh, /etc/service.d/sshd), so an image holds one.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib

ZEDBSD_SSH_DIR := userland/base/ssh
ZEDBSD_SSH_MODULES := $(filter-out %-main.c $(ZEDBSD_SSH_DIR)/ssh-client-%.c,\
	$(sort $(wildcard $(ZEDBSD_SSH_DIR)/ssh-*.c)))
ZEDBSD_SSH_SOURCES := $(ZEDBSD_SSH_DIR)/ssh-main.c \
	$(sort $(wildcard $(ZEDBSD_SSH_DIR)/ssh-client-*.c)) $(ZEDBSD_SSH_MODULES)
ZEDBSD_SSHD_SOURCES := $(sort $(wildcard $(ZEDBSD_SSH_DIR)/sshd-*.c)) userland/base/login/verify.c \
	$(ZEDBSD_SSH_MODULES)
ZEDBSD_SSH_KEYGEN_SOURCES := $(ZEDBSD_SSH_DIR)/ssh-keygen-main.c $(ZEDBSD_SSH_MODULES)
ZEDBSD_SFTP_SERVER_SOURCES := $(ZEDBSD_SSH_DIR)/sftp-server-main.c $(ZEDBSD_SSH_MODULES)

$(eval $(call ZEDBSD_USERLAND_PACKAGE,ssh,ssh secure shell client,*,n,basic,$(ZEDBSD_SSH_SOURCES),y,tests,base/ssh,base/libcrypto-compat,,program,,,usr/bin))
$(eval $(call ZEDBSD_USERLAND_PACKAGE,sshd,ssh secure shell server,*,n,basic,$(ZEDBSD_SSHD_SOURCES),y,tests,base/ssh,base/libcrypto-compat,,program,,,usr/sbin))
$(eval $(call ZEDBSD_USERLAND_PACKAGE,ssh-keygen,ssh key generator,*,n,basic,$(ZEDBSD_SSH_KEYGEN_SOURCES),y,tests,base/ssh,base/libcrypto-compat,,program,,,usr/bin))
$(eval $(call ZEDBSD_USERLAND_PACKAGE,sftp-server,SFTP server of sshd,*,n,basic,$(ZEDBSD_SFTP_SERVER_SOURCES),y,tests,base/ssh,base/libcrypto-compat,,program,,,usr/libexec))

# The files that come with the programs: the server's configuration (root only, it names the host key), the
# service definition init reads, and the script it runs, which makes the host key on the machine the first time.
# The client's configuration comes with ssh.
ZEDBSD_SSH_FILES_DIR := $(ZEDBSD_SSH_DIR)/files
ZEDBSD_PACKAGE_INPUTS += $(if $(filter sshd,$(ZEDBSD_USER_PROGRAMS)),\
	$(ZEDBSD_SSH_FILES_DIR)/sshd_config $(ZEDBSD_SSH_FILES_DIR)/sshd.service \
	$(ZEDBSD_SSH_FILES_DIR)/sshd-start)
ZEDBSD_PACKAGE_FILES += $(if $(filter sshd,$(ZEDBSD_USER_PROGRAMS)),\
	--file /etc/ssh/sshd_config=$(ZEDBSD_SSH_FILES_DIR)/sshd_config \
	--file /etc/service.d/sshd=$(ZEDBSD_SSH_FILES_DIR)/sshd.service \
	--file /usr/libexec/sshd-start=$(ZEDBSD_SSH_FILES_DIR)/sshd-start \
	--mode /etc/ssh/sshd_config=0600 --mode /usr/libexec/sshd-start=0755)
ZEDBSD_PACKAGE_INPUTS += $(if $(filter ssh,$(ZEDBSD_USER_PROGRAMS)),$(ZEDBSD_SSH_FILES_DIR)/ssh_config)
ZEDBSD_PACKAGE_FILES += $(if $(filter ssh,$(ZEDBSD_USER_PROGRAMS)),\
	--file /etc/ssh/ssh_config=$(ZEDBSD_SSH_FILES_DIR)/ssh_config)
