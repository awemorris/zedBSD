# The x86_64 build of the browser's https over libssl-compat and libcrypto-compat: the platform's default programs
# with the two compat libraries added (the image's /lib/libssl-compat.so and /lib/libcrypto-compat.so, which
# net/tls.c opens when libbrowser is built with -DWB_TLS_COMPAT).
#   make CONFIGMK=plan/ws335/tests/config-p036.mk ...
# (config/config.x86_64.mk written out: the packages' own makes read this file from their directories.)
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
PLATFORM := x86_64
ARCHITECTURE := x86_64
BOARD := pcat
VARIANT := uefi
CONFIG_USERLAND_PROGRAMS := libcrypto-compat libssl-compat
