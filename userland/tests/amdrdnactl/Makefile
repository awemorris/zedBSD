include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

# Reads and drives the amdrdna driver's control device /dev/amdrdna0 (test images only).
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
$(eval $(call USERLAND_PACKAGE,amdrdnactl,amdrdna control tool,amd64,n,basic,userland/tests/amdrdnactl/main.c userland/tests/amdrdnactl/gpu-check.c userland/tests/amdrdnactl/modes.c userland/tests/amdrdnactl/reset.c userland/tests/amdrdnactl/present.c,y,tests,tests/amdrdnactl,,0755,program,,,bin))
