# WS300: the QEMU guest of the devices' suspend and resume round trip (plan/ws052/tests/p004-guest.sh roundtrip) on
# the current build: the SSH guest image with zedBSD's own ssh and sshd (plan/tools/ssh/config-amd64-ssh.mk), with
# sleepctl and systemevents.  plan/ws052/tests/config-amd64-sleep.mk does not build on the current tree (its include
# is relative, and the OpenSSL package of plan/tools/guest/config-amd64-ssh.mk fails).  A test image only:
#   plan/tools/guest/test-image.sh plan/ws300/tests/config-amd64-sleep.mk BUILD
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
TEST_TREE := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/../../..)
include $(TEST_TREE)/plan/tools/ssh/config-amd64-ssh.mk
override CONFIG_USERLAND_PROGRAMS += sleepctl systemevents
