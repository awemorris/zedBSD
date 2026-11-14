# The x86_64 image of the kernel's boot check: the platform's configuration
# with only the programs a text login needs, so the image builds without the
# external packages.
#   make CONFIGMK=plan/tools/boot-images/config-amd64-boot.mk BUILD=build/<dir> disk
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
# The tree, found from this file: the packages' nested makes read the configuration from their own
# directories, so the files it includes are named by absolute path.
TEST_TREE := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/../../..)
include $(TEST_TREE)/config/config.x86_64.mk
override CONFIG_BOOT_SPLASH := n
override CONFIG_GRAPHICAL_LOGIN := n
override CONFIG_ROOTFS_DEVELOPMENT := n
override CONFIG_USERLAND_PROGRAMS := cat chown echo login ls mkdir sh uname
