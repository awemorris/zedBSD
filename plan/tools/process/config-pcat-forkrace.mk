# The pcat (i386) image of the fork race test: the text login image of the
# boot check with forkrace.
#   make ZEDBSD_CONFIG=plan/tools/process/config-pcat-forkrace.mk BUILD=build/<dir> disk-image
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
include plan/tools/boot-images/config-pcat-boot.mk
ZEDBSD_USER_PROGRAMS += forkrace
