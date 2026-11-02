# config/ci/config-rpi5.mk, aarch64 (Raspberry Pi 5), with zedBSD's own ssh, sshd,
# ssh-keygen and sftp-server (userland/base/ssh, on libcrypto-compat) in the place of OpenSSH (OpenSSL stays for curl), for the
# builds of the default image with them.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
include config/ci/config-rpi5.mk
ZEDBSD_USER_PROGRAMS := $(filter-out openssh,$(ZEDBSD_USER_PROGRAMS))
ZEDBSD_USER_PROGRAMS += libcrypto-compat ssh sshd ssh-keygen sftp-server
