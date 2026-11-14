# ws190-p002: the AAT image (plan/tools/aat/config-amd64-aat.mk, whose aat-input has the touch screen) with kuidemo
# (userland/tests/kuidemo: the fields and the text area the fingers' selection is tried on).  A test image only.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
# The tree, found from this file: the packages' nested makes read the configuration from their own
# directories, so the files it includes are named by absolute path.
TEST_TREE := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/../../..)
include $(TEST_TREE)/plan/tools/aat/config-amd64-aat.mk
override CONFIG_USERLAND_PROGRAMS += kuidemo
