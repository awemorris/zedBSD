# The Raspberry Pi 4 image of the fork race test (QEMU raspi4b): the text
# login image of the serial console's check (the PL011 is the console
# beside the screen) with forkrace.
#   make CONFIGMK=plan/tools/process/config-rpi4-forkrace.mk BUILD=build/<dir> disk
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
# The tree, found from this file: the packages' nested makes read the configuration from their own
# directories, so the files it includes are named by absolute path.
TEST_TREE := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/../../..)
include $(TEST_TREE)/plan/tools/console-end/config-rpi4-serial.mk
override CONFIG_USERLAND_PROGRAMS += forkrace
