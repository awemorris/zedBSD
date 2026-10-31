# The rpi4 image of the serial console's check: the boot check's image
# (config-rpi4-boot.mk) with the PL011 as the console beside the screen.
#   make ZEDBSD_CONFIG=plan/tools/console-end/config-rpi4-serial.mk BUILD=build/<dir> disk-image
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
include plan/tools/boot-images/config-rpi4-boot.mk
CONFIG_RPI4_SERIAL_CONSOLE := y
