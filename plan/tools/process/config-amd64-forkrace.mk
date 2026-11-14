# The amd64 image of the fork race test: the text login image of the boot
# check with forkrace and the programs the test runs.
#   make CONFIGMK=plan/tools/process/config-amd64-forkrace.mk BUILD=build/<dir> disk
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
# The tree, found from this file: the packages' nested makes read the configuration from their own
# directories, so the files it includes are named by absolute path.
TEST_TREE := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/../../..)
include $(TEST_TREE)/plan/tools/boot-images/config-amd64-boot.mk
override CONFIG_USERLAND_PROGRAMS += forkrace
