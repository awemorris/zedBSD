# The Raspberry Pi 5 build for driving its desktop with plan/tools/aat/aat (the Pi 5's recordings,
# plan/tools/aat/pi5-record.sh): the CI Pi 5 configuration (config/ci/config-rpi5.mk) with the test input injector
# (/dev/input-inject) and aat-input.  A test build only: neither goes into a product's configuration.  Only vmunix and
# bin/aat-input are put on the board; its own userland stays.
# Build:  make BUILD=build/q1898 ZEDBSD_CONFIG=plan/tools/aat/config-rpi5-aat.mk build/q1898/vmunix build/q1898/bin/aat-input
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
include config/ci/config-rpi5.mk

CONFIG_INPUT_TEST_INJECT := y
ZEDBSD_USER_PROGRAMS += $(filter-out $(ZEDBSD_USER_PROGRAMS),aat-input)
