# The SSH guest image of plan/tools/guest/config-amd64-ssh.mk with zedBSD's own ssh, sshd, ssh-keygen and
# sftp-server (the package ssh, userland/base/ssh, on libcrypto-compat) in the place of OpenSSH and OpenSSL, for
# qemu-interop.sh.  Build with plan/tools/guest/test-image.sh plan/tools/ssh/config-amd64-ssh.mk BUILD.
# The tree, found from this file: the packages' nested makes read the configuration from their own
# directories, so the files it includes are named by absolute path.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
TEST_TREE := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/../../..)
include $(TEST_TREE)/plan/ws035/tests/config-amd64-userland.mk
override CONFIG_PCAT_SERIAL_MIRROR := y
override CONFIG_USERLAND_PROGRAMS := $(filter-out openssh openssl,$(CONFIG_USERLAND_PROGRAMS))
override CONFIG_USERLAND_PROGRAMS += libcrypto-compat ssh
