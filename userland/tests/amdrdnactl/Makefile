include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

# Reads the amdrdna driver's reports and resets the GPU through its GPU device /dev/gpu0 (test images only).
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
$(eval $(call USERLAND_PACKAGE,amdrdnactl,amdrdna control tool,x86_64,n,basic,userland/tests/amdrdnactl/main.c userland/tests/amdrdnactl/gpu-check.c userland/tests/amdrdnactl/modes.c userland/tests/amdrdnactl/reset.c userland/tests/amdrdnactl/present.c,y,tests,tests/amdrdnactl,,0755,program,,,bin))
