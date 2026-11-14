# The Raspberry Pi 5 test build of WS321: the Pi 5 configuration (config/config.rpi5.mk, whose GPU driver carries the
# HEVC decoder as Vulkan Video H.265 decode) with the compositor's screen capture and the probes.  A test build only.
# Its kernel, programs and libraries are put on the board by plan/ws321/tests/pi5-hevc.sh.
# Build:  make -j4 BUILD=build/p321 CONFIGMK=plan/ws321/tests/config-rpi5-hevc.mk llvm-toolchain-cached
#         make -j4 BUILD=build/p321 CONFIGMK=plan/ws321/tests/config-rpi5-hevc.mk vmunix
#         build/p321/packages/vkvideo-probe/.stamp build/p321/packages/media-probe/.stamp
#         build/p321/packages/videoplayer/.stamp build/p321/packages/wayland/.stamp build/p321/packages/files/.stamp
#         build/p321/packages/keiland-shot/.stamp
#         (the programs land in build/p321/bin, the shared libraries in build/p321/dynamic)
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
include $(abspath $(dir $(lastword $(MAKEFILE_LIST)))../../..)/config/config.rpi5.mk

ZEDBSD_TEST_SCREEN_CAPTURE := y
CONFIG_USERLAND_PROGRAMS += keiland-shot vkvideo-probe media-probe
