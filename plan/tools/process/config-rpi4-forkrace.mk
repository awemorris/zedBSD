# The Raspberry Pi 4 image of the fork race test (QEMU raspi4b): the text
# login image of the boot check with forkrace.
#   make ZEDBSD_CONFIG=plan/tools/process/config-rpi4-forkrace.mk BUILD=build/<dir> disk-image
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
include plan/tools/boot-images/config-rpi4-boot.mk
ZEDBSD_USER_PROGRAMS += forkrace
