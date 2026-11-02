# config/ci/config-zybo.mk, armv7 (ZYBO), with zedBSD's own ssh, sshd,
# ssh-keygen and sftp-server (userland/base/ssh, on libcrypto-compat) in the place of OpenSSH and OpenSSL, for the
# builds of the programs on this architecture.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
include config/ci/config-zybo.mk
ZEDBSD_USER_PROGRAMS := $(filter-out openssh openssl,$(ZEDBSD_USER_PROGRAMS))
ZEDBSD_USER_PROGRAMS += libcrypto-compat ssh sshd ssh-keygen sftp-server
