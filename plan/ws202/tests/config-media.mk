# Native media image selection for Q1/T1; the implementation session builds named targets only.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
include config/ci/config-amd64.mk
ZEDBSD_USER_PROGRAMS := $(filter-out libavcodec,$(ZEDBSD_USER_PROGRAMS)) media-probe
