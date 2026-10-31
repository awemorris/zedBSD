# ws291-p025: the Pi 4 build for the desktop's frames (plan/ws291/tests/pi4-desk.sh): plan/ws291/tests/config-rpi4-shot.mk
# with the test-only input injection (aat-input, which plan/tools/frame-ops/frame-ops.sh drives).
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
include plan/ws291/tests/config-rpi4-shot.mk

CONFIG_INPUT_TEST_INJECT := y
ZEDBSD_USER_PROGRAMS += $(filter-out $(ZEDBSD_USER_PROGRAMS),aat-input)
