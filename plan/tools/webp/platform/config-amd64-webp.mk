# The SSH guest image of libwebp-compat's guest test (webp-guest.sh): the guest harness's image with
# /lib/libwebp-compat.so; the test copies its driver and files in over SSH.  Build (the result is BUILD/hdd-image.img):
#   plan/tools/guest/test-image.sh plan/tools/webp/platform/config-amd64-webp.mk BUILD
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
# The tree, found from this file: the packages' nested makes read the configuration from their own
# directories, so the files it includes are named by absolute path.
TEST_TREE := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/../../../..)
include $(TEST_TREE)/plan/tools/guest/config-amd64-ssh.mk
override CONFIG_USERLAND_PROGRAMS := $(filter-out libwebp-compat,$(CONFIG_USERLAND_PROGRAMS)) libwebp-compat
