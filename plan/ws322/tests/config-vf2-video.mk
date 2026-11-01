# The VisionFive 2 kernel and programs of ws322-p004's board check: the board's regression build
# (plan/ws291/tests/config-vf2-shot.mk: the desktop, keiland-shot and the GPU checks) with the Wave511 decoder brought up
# at boot as the render node's video engine (CONFIG_DRIVER_VF2_VPU), and the decode checks media-probe and
# vkvideo-probe beside Video Player.
#   make ZEDBSD_CONFIG=plan/ws322/tests/config-vf2-video.mk BUILD=build/<dir> build/<dir>/vmunix build/<dir>/bin/...
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
include plan/ws291/tests/config-vf2-shot.mk
CONFIG_DRIVER_VF2_VPU := y
ZEDBSD_USER_PROGRAMS += $(filter-out $(ZEDBSD_USER_PROGRAMS),media-probe vkvideo-probe)
