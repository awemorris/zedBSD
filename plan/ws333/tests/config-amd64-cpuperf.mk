# The amd64 image of the x86 processor driver's boot check: the thermal core's
# small text-login image with the CPU performance control built in, and the
# programs that read the kernel's log.
#   make ZEDBSD_CONFIG=plan/ws333/tests/config-amd64-cpuperf.mk BUILD=<dir> disk-image
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
include plan/ws333/tests/config-amd64-boot.mk
CONFIG_CPU_PERF := y
ZEDBSD_USER_PROGRAMS += dmesg grep
