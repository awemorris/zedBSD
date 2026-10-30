# The rpi4 image of the console's end over the firmware's screen: the boot
# check's image (config-rpi4-boot.mk) with a kernel whose test graphics owner
# takes the screen 20 seconds after the console starts and gives it back 30
# seconds later (KERN_TEST_TEXT_SUSPEND_SECONDS in src/drivers/generic/fb-text.c),
# and that ends the console over the screen's range 90 seconds after the screen
# is mapped, as a display driver's first flip does (KERN_TEST_TEXT_END_SECONDS
# in src/drivers/generic/boot-fb.c).
#   make ZEDBSD_CONFIG=plan/tools/console-end/config-rpi4-end.mk BUILD=build/<dir> disk-image
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
include plan/tools/boot-images/config-rpi4-boot.mk
ZEDBSD_TEST_CPPFLAGS := -DKERN_TEST_TEXT_SUSPEND_SECONDS=20 -DKERN_TEST_TEXT_END_SECONDS=90
