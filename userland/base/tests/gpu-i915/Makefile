include $(dir $(lastword $(MAKEFILE_LIST)))../../package.mk

# Root-only native i915 GPU smoke test for the IGD passthrough boot.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
$(eval $(call ZEDBSD_USERLAND_PACKAGE,gpu-i915-test,Native i915 GPU test,amd64,y,basic,userland/base/tests/gpu-i915/main.c,y,base,base/tests/gpu-i915,,0755,program,,,bin))
