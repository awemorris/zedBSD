include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

# The PowerVR stage test (one line a stage, each in a child with a time limit).
# gpu-selftest-boot adds the oneshot service that runs it at boot with its lines on the
# console; a test image selects it to run the test on its own (rc.conf names it optional).
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
GPU_SELFTEST_BOOT_DATA := /etc/service.d/gpu_selftest=userland/tests/gpu-selftest/service
$(eval $(call USERLAND_PACKAGE,gpu-selftest,PowerVR stage test,visionfive2,n,basic,userland/tests/gpu-selftest/main.c,y,tests,tests/gpu-selftest,tests/vkcompute tests/vkimage tests/vkstage tests/vkdemo,0755,program,,,bin))
$(eval $(call USERLAND_PACKAGE,gpu-selftest-boot,PowerVR stage test at boot,visionfive2,n,data,,y,tests,tests/gpu-selftest-boot,tests/gpu-selftest,,data,$(GPU_SELFTEST_BOOT_DATA)))
