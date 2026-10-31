# The ZYBO (armv7) image of the fork race test, for QEMU xilinx-zynq-a9
# with its root as a ramdisk: the port's configuration with only the
# programs a text login needs, and forkrace.
#   make ZEDBSD_CONFIG=plan/tools/process/config-zybo-forkrace.mk BUILD=build/<dir> build/<dir>/vmunix build/<dir>/root.img
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
include config/ci/config-zybo.mk
ZEDBSD_USER_PROGRAMS := cat echo login ls sh uname forkrace
ARMV7_ROOT_EXTRA_COMMANDS := forkrace
