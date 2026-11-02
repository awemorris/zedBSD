# The SSH guest image of plan/tools/guest/config-amd64-ssh.mk with zedBSD's own ssh, sshd, ssh-keygen and
# sftp-server (userland/base/ssh, on libcrypto-compat) in the place of OpenSSH and OpenSSL, for qemu-interop.sh.
# Build with plan/tools/guest/test-image.sh plan/ws336/tests/config-amd64-ssh.mk BUILD.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
include plan/ws035/tests/config-amd64-userland.mk
CONFIG_PCAT_SERIAL_MIRROR := y
ZEDBSD_USER_PROGRAMS := $(filter-out openssh openssl,$(ZEDBSD_USER_PROGRAMS))
ZEDBSD_USER_PROGRAMS += libcrypto-compat ssh sshd ssh-keygen sftp-server
