# WS327's QEMU image for the layers' own tests (thread-test.c, socket-test.c built for zedBSD by guest-layers.sh):
# the userland baseline with libkeiland and the SSH guest harness.  No compositor or GPU is needed.
# Build:  plan/tools/guest/test-image.sh plan/ws327/tests/config-amd64-layers.mk build/ws327-layers-img
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
include plan/ws035/tests/config-amd64-userland.mk
CONFIG_PCAT_SERIAL_MIRROR := y
ZEDBSD_USER_PROGRAMS += openssl openssh
