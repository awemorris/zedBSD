# The riscv64 QEMU virt image of the fork race test: the port's text login
# configuration with forkrace, booted from its root.img ramdisk.
#   make ZEDBSD_CONFIG=plan/tools/process/config-riscv64-forkrace.mk BUILD=build/<dir> build/<dir>/vmunix build/<dir>/root.img
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
include config/ci/config-riscv64-virt.mk
ZEDBSD_USER_PROGRAMS += forkrace
