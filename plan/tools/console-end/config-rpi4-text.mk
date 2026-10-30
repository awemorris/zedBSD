# The rpi4 image of the console's hand-over check: the boot check's image
# (config-rpi4-boot.mk) with a kernel whose test graphics owner takes the
# screen 20 seconds after the console starts and gives it back 30 seconds
# later (KERN_TEST_TEXT_SUSPEND_SECONDS in src/drivers/generic/fb-text.c).
#   make ZEDBSD_CONFIG=plan/tools/console-end/config-rpi4-text.mk BUILD=build/<dir> disk-image
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
include plan/tools/boot-images/config-rpi4-boot.mk
ZEDBSD_TEST_CPPFLAGS := -DKERN_TEST_TEXT_SUSPEND_SECONDS=20
