# The VisionFive 2 kernel and programs of ws322-p007's board check: config-vf2-video.mk (the Wave511 decoder as the
# render node's video engine, the GPU checks, media-probe, vkvideo-probe, Video Player) with /dev/wave5-test, whose
# read gives the engine's counts and the core's health and whose writes size the task buffer ("task <thousandths>")
# and have picture completions go unseen ("lose <count>") for the recovery's test.
#   make ZEDBSD_CONFIG=plan/ws322/tests/config-vf2-video-test.mk BUILD=build/<dir> build/<dir>/vmunix build/<dir>/bin/...
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
include plan/ws322/tests/config-vf2-video.mk
ZEDBSD_TEST_CPPFLAGS += -DWAVE5_VIDEO_TEST=1
