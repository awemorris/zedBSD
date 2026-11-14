# ws235: config-amd64-rtl8127-perf.mk with the RTL8125-family driver's profile (its interrupt, poll and transmit
# counted and timed, logged once a second while frames move), for plan/tools/rtl8127/rtl-link.sh PROFILE=1.
# Build:  make CONFIGMK=plan/tools/rtl8127/config-amd64-rtl8127-profile.mk BUILD=build/ws235/amd64-p build/ws235/amd64-p/vmunix
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
# The tree, found from this file: the packages' nested makes read the configuration from their own
# directories, so the files it includes are named by absolute path.
TEST_TREE := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/../../..)
include $(TEST_TREE)/plan/tools/rtl8127/config-amd64-rtl8127-perf.mk

override CONFIG_DRIVER_PCI_RTL8125_PROFILE := y
