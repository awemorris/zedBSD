# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

#
# klsudo runs a system action as the administrator, as pkexec does: the
# program the system's definition names (/etc/keiland/admin.d), once the
# person at the screen has allowed it in the compositor's dialog
# (libkeiland's kl_process_start_admin).
#

PROG := klsudo
SRCS := main.c
CLASS := basic
REQUIRE := desktop/libwayland desktop/libkeiland

include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk
