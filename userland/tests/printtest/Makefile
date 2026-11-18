include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

# printtest (ws145-p003): a client of libkeiland's printers for the tests -- lists the printers and the jobs, adds,
# removes and chooses a printer, and prints a PDF, following its job to its end, a line for each answer and change.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
PRINTTEST_SOURCES := userland/tests/printtest/main.c
$(eval $(call USERLAND_PACKAGE,printtest,Print test client,x86_64,n,basic,$(PRINTTEST_SOURCES),y,tests,tests/printtest,desktop/libwayland desktop/libkeiland,0755,program,,,bin))
