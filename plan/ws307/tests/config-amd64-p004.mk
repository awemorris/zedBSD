# WS307 p004: the installer's image (make installer-image) for plan/ws307/tests/p004-gtk.sh.  The release's
# configuration, with the test's own additions to the live root only (J4): OpenSSH and OpenSSL, so the script reaches
# the live system over SSH.  Clang, libc++ and pkgconf are built (the test builds a GTK 4 program with them on the
# installed system); Emacs is left out, as the test does not use it and it is in no item.  The harness's keys and
# net.conf come as ZEDBSD_INSTALLER_TEST_FILES on the command line (plan/tools/guest/guest.py extra-files).
#   make ZEDBSD_CONFIG=plan/ws307/tests/config-amd64-p004.mk BUILD=build/ws307-p004 \
#       ZEDBSD_INSTALLER_TEST_FILES="<guest.py extra-files>" installer-image
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
include config/ci/config-amd64.mk
ZEDBSD_INSTALLER_TEST_UNITS := network-openssh security-openssl
ZEDBSD_INSTALLER_TEST_OMIT := emacs
