# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

#
# The sources of zedBSD's own SSH 2 programs (ssh, sshd, ssh-keygen and
# sftp-server), read by each program's Makefile after ../package.mk.  Each
# program is its main file, its own program files and every library module
# (ssh-*.c other than the programs' own); sshd also takes the console
# login's password check.  The paths are from the top of the tree, which is
# where package.mk compiles them; the files are found from REPO_ROOT, since
# the program's make runs in its own directory.
#

ZEDBSD_SSH_DIR := userland/base/ssh
zedbsd_ssh_files = $(sort $(patsubst $(REPO_ROOT)/%,%,$(wildcard $(REPO_ROOT)/$(ZEDBSD_SSH_DIR)/$(1))))

ZEDBSD_SSH_MODULES := $(filter-out %-main.c $(ZEDBSD_SSH_DIR)/ssh-client-%.c,\
	$(call zedbsd_ssh_files,ssh-*.c))
ZEDBSD_SSH_SOURCES := $(ZEDBSD_SSH_DIR)/ssh-main.c \
	$(call zedbsd_ssh_files,ssh-client-*.c) $(ZEDBSD_SSH_MODULES)
ZEDBSD_SSHD_SOURCES := $(call zedbsd_ssh_files,sshd-*.c) userland/base/login/verify.c \
	$(ZEDBSD_SSH_MODULES)
ZEDBSD_SSH_KEYGEN_SOURCES := $(ZEDBSD_SSH_DIR)/ssh-keygen-main.c $(ZEDBSD_SSH_MODULES)
ZEDBSD_SFTP_SERVER_SOURCES := $(ZEDBSD_SSH_DIR)/sftp-server-main.c $(ZEDBSD_SSH_MODULES)

ZEDBSD_SSH_FILES_DIR := $(ZEDBSD_SSH_DIR)/files
