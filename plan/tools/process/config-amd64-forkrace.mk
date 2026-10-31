# The amd64 image of the fork race test: the text login image of the boot
# check with forkrace and the programs the test runs.
#   make ZEDBSD_CONFIG=plan/tools/process/config-amd64-forkrace.mk BUILD=build/<dir> disk-image
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
include plan/tools/boot-images/config-amd64-boot.mk
ZEDBSD_USER_PROGRAMS += forkrace
