# ws291-p025: the Pi 4 build of plan/ws309/tests/config-rpi4-p015.mk with the compositor's test-only screen capture and
# its client keiland-shot (the screen's PNG over SSH), for mview's picture on the board (plan/ws291/tests/pi4-mview.sh
# SHOT=1).
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
include plan/ws309/tests/config-rpi4-p015.mk

ZEDBSD_TEST_SCREEN_CAPTURE := y
ZEDBSD_USER_PROGRAMS += $(filter-out $(ZEDBSD_USER_PROGRAMS),keiland-shot)
