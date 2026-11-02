# The SSH guest image of libwebp-compat's guest test (webp-guest.sh): the guest harness's image with
# /lib/libwebp-compat.so; the test copies its driver and files in over SSH.  Build (the result is BUILD/hdd-image.img):
#   plan/tools/guest/test-image.sh plan/tools/webp/platform/config-amd64-webp.mk BUILD
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
include plan/tools/guest/config-amd64-ssh.mk
ZEDBSD_USER_PROGRAMS := $(filter-out libwebp-compat,$(ZEDBSD_USER_PROGRAMS)) libwebp-compat
