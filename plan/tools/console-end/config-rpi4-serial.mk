# The rpi4 image of the serial console's check: the boot check's image
# (config-rpi4-boot.mk) with the PL011 as the console beside the screen.
#   make CONFIGMK=plan/tools/console-end/config-rpi4-serial.mk BUILD=build/<dir> disk
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
# The tree, found from this file: the packages' nested makes read the configuration from their own
# directories, so the files it includes are named by absolute path.
TEST_TREE := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/../../..)
include $(TEST_TREE)/plan/tools/boot-images/config-rpi4-boot.mk
override CONFIG_RPI4_SERIAL_CONSOLE := y
