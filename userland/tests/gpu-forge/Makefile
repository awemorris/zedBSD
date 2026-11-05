include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

# A GPU buffer whose fd is not the image its description claims (ws103-p004): the compositor must refuse it.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
GPU_FORGE_TEST_SOURCES := userland/tests/gpu-forge/main.c
$(eval $(call USERLAND_PACKAGE,gpu-forge-test,GPU buffer forgery test,amd64,y,basic,$(GPU_FORGE_TEST_SOURCES),y,tests,tests/gpu-forge,desktop/libvulkan desktop/libwayland,0755,program,,,bin))
