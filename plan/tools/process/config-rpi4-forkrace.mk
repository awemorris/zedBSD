# The Raspberry Pi 4 image of the fork race test (QEMU raspi4b): the text
# login image of the serial console's check (the PL011 is the console
# beside the screen) with forkrace.
#   make ZEDBSD_CONFIG=plan/tools/process/config-rpi4-forkrace.mk BUILD=build/<dir> disk-image
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
include plan/tools/console-end/config-rpi4-serial.mk
ZEDBSD_USER_PROGRAMS += forkrace
