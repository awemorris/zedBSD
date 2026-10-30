# The rpi4 image of the kernel's boot check: the port's configuration with
# only the programs a text login needs, so the image builds without the
# external packages.
#   make ZEDBSD_CONFIG=plan/ws331/tests/config-rpi4-boot.mk BUILD=build/<dir> disk-image
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
include plan/tools/rpi4/config-rpi4-sd.mk
ZEDBSD_GRAPHICAL_BOOT := n
ZEDBSD_GRAPHICAL_LOGIN := n
ZEDBSD_USER_PROGRAMS := cat chown echo login ls mkdir sh uname
