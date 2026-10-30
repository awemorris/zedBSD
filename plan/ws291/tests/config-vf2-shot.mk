# ws291-p020: config-vf2.mk with the compositor's test-only screen capture and its client keiland-shot (the screen's PNG
# over SSH), for the board's regression of the desktop and mview's picture (plan/ws291/tests/vf2-regress.sh).
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
include plan/ws291/tests/config-vf2.mk

ZEDBSD_TEST_SCREEN_CAPTURE := y
ZEDBSD_USER_PROGRAMS += $(filter-out $(ZEDBSD_USER_PROGRAMS),keiland-shot)
