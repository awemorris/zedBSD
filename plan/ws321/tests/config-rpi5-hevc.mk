# The Raspberry Pi 5 test build of WS321: the CI Pi 5 configuration (config/ci/config-rpi5.mk) with the HEVC decoder
# as Vulkan Video H.265 decode (CONFIG_DRIVER_BCM2712_VIDEO), the compositor's screen capture and the probes.  A test
# build only.  Its kernel, programs and libraries are put on the board by plan/ws321/tests/pi5-hevc.sh.
# Build:  make BUILD=build/p321 ZEDBSD_CONFIG=plan/ws321/tests/config-rpi5-hevc.mk build/p321/vmunix
#         build/p321/bin/vkvideo-probe build/p321/bin/media-probe build/p321/bin/videoplayer build/p321/bin/wayland
#         build/p321/bin/files build/p321/bin/keiland-shot
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
include config/ci/config-rpi5.mk

CONFIG_DRIVER_BCM2712_VIDEO := y
ZEDBSD_TEST_SCREEN_CAPTURE := y
ZEDBSD_USER_PROGRAMS += $(filter-out $(ZEDBSD_USER_PROGRAMS),keiland-shot vkvideo-probe media-probe)
