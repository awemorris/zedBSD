# ws291-p020: the VisionFive 2 (PowerVR) measurement build: the CI Keiland image of the board with mview, whose kernel
# and changed files replace the board's one by one over SSH (plan/ws291/tests/vf2-mview.sh).
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
include config/ci/config-visionfive2-keiland.mk

# The 3D scene.
ZEDBSD_USER_PROGRAMS += $(filter-out $(ZEDBSD_USER_PROGRAMS),mview)
