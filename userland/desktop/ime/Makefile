include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

# The system's input method, /usr/libexec/keiland-ime (WS095, plan/ws095/design.md): the compositor starts it and it
# serves the input method protocols; the language engines (engine-direct.c, ja-*.c) know no Wayland and are
# tested on the host (plan/ws095/tests/host-engine.sh).  The Japanese dictionaries are the package ime-dict-ja.
# Not chosen by default yet (ws095-p004): an image that chooses it also fetches the dictionaries.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
IME_SOURCES := userland/desktop/ime/main.c userland/desktop/ime/method.c userland/desktop/ime/keys.c \
	userland/desktop/ime/output.c userland/desktop/ime/engine-direct.c \
	userland/desktop/ime/ja-kana.c userland/desktop/ime/ja-romaji.c userland/desktop/ime/ja-dict.c \
	userland/desktop/ime/ja-user.c userland/desktop/ime/ja-inflect.c userland/desktop/ime/ja-segment.c \
	userland/desktop/ime/ja-engine.c userland/desktop/ime/ja-keys.c userland/desktop/ime/skk-engine.c userland/desktop/ime/ja-predict.c \
	userland/desktop/ime/popup.c
$(eval $(call ZEDBSD_USERLAND_PACKAGE,keiland-ime,Input method,amd64,n,basic,$(IME_SOURCES),y,desktop,desktop/ime,desktop/libwayland desktop/libkeiland desktop/ime/dict desktop/ime/skk-dict,0755,program,,,usr/libexec))
