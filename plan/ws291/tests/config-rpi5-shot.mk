# ws291-p021: the Pi 5 build of plan/ws291/tests/config-rpi5-desk.mk with the compositor's test-only screen capture and
# its client keiland-shot (the screen's PNG over SSH), for the desktop's and mview's picture on the board.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
include plan/ws291/tests/config-rpi5-desk.mk

ZEDBSD_TEST_SCREEN_CAPTURE := y
ZEDBSD_USER_PROGRAMS += $(filter-out $(ZEDBSD_USER_PROGRAMS),keiland-shot)
