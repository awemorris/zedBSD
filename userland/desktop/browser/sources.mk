# The source list of the browser app, read by its Makefile and apps/browser-win32/Makefile.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
KEILAND_BROWSER_DIR := userland/desktop/browser

KEILAND_BROWSER_SOURCES := $(KEILAND_BROWSER_DIR)/main.c \
	$(KEILAND_BROWSER_DIR)/shell/shell.c \
	$(KEILAND_BROWSER_DIR)/shell/window.c \
	$(KEILAND_BROWSER_DIR)/shell/present.c \
	$(KEILAND_BROWSER_DIR)/shell/titlebar.c \
	$(KEILAND_BROWSER_DIR)/shell/keys.c \
	$(KEILAND_BROWSER_DIR)/shell/touch.c \
	$(KEILAND_BROWSER_DIR)/shell/mail.c \
	userland/desktop/picture/png-write.c
