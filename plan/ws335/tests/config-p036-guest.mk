# The x86_64 image of the browser's https over libssl-compat and libcrypto-compat in QEMU (run-browser-guest.sh):
# the platform's default programs (the browser, the two compat libraries, ca-certificates' /etc/ssl/cert.pem) with
# the console mirrored to the serial line (the test logs in there).  The platform's defaults set the graphical
# login after this file is read, so the test's make gives CONFIG_GRAPHICAL_LOGIN=n on its command line.
#   make CONFIGMK=plan/ws335/tests/config-p036-guest.mk CONFIG_GRAPHICAL_LOGIN=n ... disk
# (config/config.x86_64.mk written out: the packages' own makes read this file from their directories.)
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
PLATFORM := x86_64
ARCHITECTURE := x86_64
BOARD := pcat
VARIANT := uefi
CONFIG_PCAT_SERIAL_MIRROR := y
