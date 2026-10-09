# ws083-p007: the image for the video engine's recovery from a hang on the Latitude 5330 (the steps in
# plan/ws083/phase007/phase.md).  It is the current UAT image (config/current-uat.mk) booted with i915.debug=video,
# with vkvideo-probe and the test streams in /root/ws083, and a kernel whose video decodes 2, 4, 6 and 8 since the
# start run into a loop that does not finish (the compile-time fault injection I915_TEST_VIDEO_HANG_AT of
# src/drivers/gpu/i915/render/video.c), so one boot shows the recovery after each of three resets and the stop at
# the fourth hang:
#     make -j16 ZEDBSD_CONFIG=plan/ws083/tests/config-video-hang.mk BUILD=build/ws083-hang disk-image
# or, for a machine that already runs the UAT image of the same tree with i915.debug=video, the kernel alone:
#     make -j16 ZEDBSD_CONFIG=plan/ws083/tests/config-video-hang.mk BUILD=build/ws083-hang build/ws083-hang/vmunix
# WS083_HANG chooses other runs (each choice needs its own BUILD, the stamp records the flags).
# Never a release image: the kernel hangs the video engine on purpose.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
include config/current-uat.mk
ZEDBSD_BOOT_EXTRA_LINES += i915.debug=video
ZEDBSD_USER_PROGRAMS += vkvideo-probe
ZEDBSD_TEST_EXTRA_FILES += $(foreach f,$(notdir $(wildcard plan/ws083/tests/streams/*)),--file /root/ws083/$(f)=plan/ws083/tests/streams/$(f))
WS083_HANG ?= -DI915_TEST_VIDEO_HANG_AT=2 -DI915_TEST_VIDEO_HANG_STEP=2 -DI915_TEST_VIDEO_HANG_COUNT=4
ZEDBSD_TEST_CPPFLAGS := $(WS083_HANG)
