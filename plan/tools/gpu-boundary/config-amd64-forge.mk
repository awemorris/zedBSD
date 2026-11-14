# ws103-p004: the criteria image (plan/ws099/tests/config-amd64-criteria.mk) with the GPU buffer forgery test
# (/bin/gpu-forge-test), wltest (a real client after the refusal) and acquire-fence-test (plan/ws035/tests/zdesktop-p054.sh).  Build: sh plan/tools/gpu-boundary/build-forge-image.sh [BUILD]
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
# The tree, found from this file: the packages' nested makes read the configuration from their own
# directories, so the files it includes are named by absolute path.
TEST_TREE := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/../../..)
include $(TEST_TREE)/plan/ws099/tests/config-amd64-criteria.mk
override CONFIG_USERLAND_PROGRAMS += gpu-forge-test wltest acquire-fence-test
