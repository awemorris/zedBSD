# The Raspberry Pi 5 build for driving its desktop with plan/tools/aat/aat (the Pi 5's recordings,
# plan/tools/aat/pi5-record.sh): the CI Pi 5 configuration (config/ci/config-rpi5.mk) with the test input injector
# (/dev/input-inject) and aat-input.  A test build only: neither goes into a product's configuration.  Only vmunix and
# bin/aat-input are put on the board; its own userland stays.
# Build:  make BUILD=build/q1898 CONFIGMK=plan/tools/aat/config-rpi5-aat.mk build/q1898/vmunix build/q1898/bin/aat-input
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
# The tree, found from this file: the packages' nested makes read the configuration from their own
# directories, so the files it includes are named by absolute path.
TEST_TREE := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/../../..)
include $(TEST_TREE)/config/config.rpi5.mk
include $(TEST_TREE)/mk/platform-rpi5/defaults.mk

override CONFIG_INPUT_TEST_INJECT := y
override CONFIG_USERLAND_PROGRAMS += aat-input
