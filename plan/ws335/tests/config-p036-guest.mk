# The x86_64 image of the browser's https over libssl-compat and libcrypto-compat in QEMU: the platform's default
# programs (the browser, ca-certificates' /etc/ssl/cert.pem) with the two compat libraries added, and the console
# mirrored to the serial line (the test logs in there).
#   make CONFIGMK=plan/ws335/tests/config-p036-guest.mk ... disk
# (config/config.x86_64.mk written out: the packages' own makes read this file from their directories.)
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
PLATFORM := x86_64
ARCHITECTURE := x86_64
BOARD := pcat
VARIANT := uefi
CONFIG_PCAT_SERIAL_MIRROR := y
CONFIG_USERLAND_PROGRAMS := libcrypto-compat libssl-compat
