# WS327's QEMU regression of the moved applications (guest-apps.sh): the AAT image (plan/tools/aat/config-amd64-aat.mk,
# the UAT image with the test input and the screen's capture) without the development environment (clang, libc++)
# and Emacs, which the applications' scenarios do not use and whose build is the toolchain's.
# Build:  plan/tools/guest/test-image.sh plan/ws327/tests/config-amd64-aat.mk BUILD \
#             --file /usr/share/keiland/wallpaper.png=userland/desktop/wallpapers/Birch-Lake.png
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
include plan/tools/aat/config-amd64-aat.mk
ZEDBSD_USER_PROGRAMS := $(filter-out clang libcxx emacs,$(ZEDBSD_USER_PROGRAMS))
