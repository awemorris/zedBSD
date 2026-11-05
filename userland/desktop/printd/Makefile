include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

# keiland-printd (ws145-p002, plan/ws145/design.md §5): the user's printer daemon, which libkeiland-backend starts with a
# socket on descriptor 3.  It copies each document into its spool under $XDG_RUNTIME_DIR and sends it to the printer by
# IPP (plain HTTP/1.1) or LPD; it listens on no socket and is not set-user-ID.  In /usr/libexec (outside PATH).
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
KEILAND_PRINTD_SOURCES := userland/desktop/printd/main.c userland/desktop/printd/spool.c userland/desktop/printd/net.c \
	userland/desktop/printd/ipp.c userland/desktop/printd/lpd.c
$(eval $(call USERLAND_PACKAGE,keiland-printd,keiland-printd - Printer daemon,*,n,basic,$(KEILAND_PRINTD_SOURCES),y,desktop,,,0755,program,,,usr/libexec))
