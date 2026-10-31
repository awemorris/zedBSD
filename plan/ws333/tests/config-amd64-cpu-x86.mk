# The amd64 image of the x86 processor driver's boot check without the CPU
# performance control (the default): the description of the CPUs only.
#   plan/ws333/tests/cpu-x86-qemu.sh OUT plan/ws333/tests/config-amd64-cpu-x86.mk
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
include plan/ws333/tests/config-amd64-boot.mk
ZEDBSD_USER_PROGRAMS += dmesg grep
