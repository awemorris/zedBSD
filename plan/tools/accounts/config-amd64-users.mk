# ws196-p004 (T1): the CI guest image (config/config.x86_64.mk: graphical login, sessiond, Settings, account-admin,
# sudo) with root's useradd, usermod and userdel (ws196-p003), for account-admin's root form (p002) and the three
# commands.  Build (BUILD/hdd-image.img):
#   plan/tools/guest/test-image.sh plan/tools/accounts/config-amd64-users.mk BUILD
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
# The tree, found from this file: the packages' nested makes read the configuration from their own
# directories, so the files it includes are named by absolute path.
TEST_TREE := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/../../..)
include $(TEST_TREE)/config/config.x86_64.mk
include $(TEST_TREE)/mk/platform-x86_64/defaults.mk
override CONFIG_USERLAND_PROGRAMS += account-admin keiland-settings settings sessiond
override CONFIG_USERLAND_PROGRAMS += useradd usermod userdel
