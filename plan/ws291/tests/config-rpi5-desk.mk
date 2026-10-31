# ws291-p021: the Pi 5 build for the desktop's frames (plan/ws291/tests/rpi5-desk.sh): plan/ws291/tests/config-rpi5.mk
# with the test-only input injection (aat-input, which plan/tools/frame-ops/frame-ops.sh drives).
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
include plan/ws291/tests/config-rpi5.mk

CONFIG_INPUT_TEST_INJECT := y
ZEDBSD_USER_PROGRAMS += $(filter-out $(ZEDBSD_USER_PROGRAMS),aat-input)
