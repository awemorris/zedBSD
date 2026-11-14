# The x86_64 image with the platform's default programs, for fetch's https in QEMU (run-fetch-qemu.sh), without the
# firmware packages: QEMU needs none, and each one asks for its license to be accepted (x86_64 has no
# firmware-licenses target that answers for an unattended build).
# The tree, found from this file: the packages' nested makes read the configuration from their own directories,
# so the files it includes are named by absolute path; the defaults are read first so that they can be filtered.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
TEST_TREE := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/../../..)
include $(TEST_TREE)/config/config.x86_64.mk
include $(TEST_TREE)/mk/platform-x86_64/defaults.mk
override CONFIG_USERLAND_PROGRAMS := $(filter-out %-firmware,$(CONFIG_USERLAND_PROGRAMS))
