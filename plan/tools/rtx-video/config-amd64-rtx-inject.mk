# The RTX 2070 machine's test kernel with the AAT's input injection (q1692-i01, Notes typed into on the RTX): the
# product config with CONFIG_INPUT_TEST_INJECT and the aat-input program (copied to the machine, not in an image).
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
# The tree, found from this file: the packages' nested makes read the configuration from their own
# directories, so the files it includes are named by absolute path.
TEST_TREE := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/../../..)
include $(TEST_TREE)/config/config.x86_64.mk
include $(TEST_TREE)/mk/platform-x86_64/defaults.mk
override CONFIG_INPUT_TEST_INJECT := y
override CONFIG_USERLAND_PROGRAMS += aat-input
