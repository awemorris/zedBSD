# The source list of libkeiland, read by its Makefile, Makefile.linux and apps/browser-win32/Makefile:
# the Keiland extensions, the settings, the system and the audio, then the desktop's shared widgets and
# controls (WS090; their own library until WS131 p012, named kl_ since p013, <keiland/keiland.h>): the
# drawing layer, the scroll and the input, the window, the widgets and the file chooser.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
LIBKEILAND_SOURCES := userland/desktop/libkeiland/version.c userland/desktop/libkeiland/menu.c \
	userland/desktop/libkeiland/recent.c userland/desktop/libkeiland/titlebar.c \
	userland/desktop/libkeiland/glass.c userland/desktop/libkeiland/instance.c userland/desktop/libkeiland/appearance.c \
	userland/desktop/libkeiland/motion.c userland/desktop/libkeiland/scroll.c \
	userland/desktop/libkeiland/gesture.c \
	userland/desktop/libkeiland/desktop.c \
	userland/desktop/libkeiland/keyboard-inset.c userland/desktop/libkeiland/edit.c \
	userland/desktop/libkeiland/settings.c userland/desktop/libkeiland/settings-cache.c \
	userland/desktop/libkeiland/settings-app.c userland/desktop/settings-keys/settings-keys.c \
	userland/desktop/libkeiland/translate.c userland/desktop/libkeiland/translate-follow.c \
	userland/desktop/libkeiland/system/system.c userland/desktop/libkeiland/system/system-view.c \
	userland/desktop/libkeiland/system/system-protocol.c userland/desktop/libkeiland/system/system-monitor.c \
	userland/desktop/libkeiland/system/system-monitor-rate.c \
	userland/desktop/libkeiland/audio/audio.c userland/desktop/libkeiland/audio/audio-protocol.c \
	userland/desktop/libkeiland/ui/canvas.c \
	userland/desktop/libkeiland/ui/drop-look.c \
	userland/desktop/libkeiland/ui/text.c \
	userland/desktop/libkeiland/ui/icons.c \
	userland/desktop/libkeiland/ui/icons-line.c \
	userland/desktop/libkeiland/ui/theme.c \
	userland/desktop/libkeiland/ui/input.c \
	userland/desktop/libkeiland/ui/scroll.c \
	userland/desktop/libkeiland/ui/scroll-bar.c \
	userland/desktop/libkeiland/ui/text-touch.c \
	userland/desktop/libkeiland/ui/text-bar.c \
	userland/desktop/libkeiland/ui/text-select.c \
	userland/desktop/libkeiland/ui/ui.c \
	userland/desktop/libkeiland/ui/window.c \
	userland/desktop/libkeiland/ui/app.c \
	userland/desktop/libkeiland/platform/platform-posix.c \
	userland/desktop/libkeiland/platform/process-posix.c \
	userland/desktop/libkeiland/platform/thread-posix.c \
	userland/desktop/libkeiland/platform/socket-posix.c \
	userland/desktop/libkeiland/process.c \
	userland/desktop/libkeiland/launch.c \
	userland/desktop/libkeiland/ui/declare.c \
	userland/desktop/libkeiland/ui/globals.c \
	userland/desktop/libkeiland/ui/window-declare.c \
	userland/desktop/libkeiland/ui/present.c \
	userland/desktop/libkeiland/ui/present-copy.c \
	userland/desktop/libkeiland/ui/present-damage.c \
	userland/desktop/libkeiland/ui/trace.c \
	userland/desktop/libkeiland/ui/present-shm.c \
	userland/desktop/libkeiland/ui/clipboard.c \
	userland/desktop/libkeiland/ui/primary.c \
	userland/desktop/libkeiland/ui/text-input.c \
	userland/desktop/libkeiland/ui/text-area.c \
	userland/desktop/libkeiland/ui/tablet.c \
	userland/desktop/libkeiland/ui/edit.c \
	userland/desktop/libkeiland/ui/widgets.c \
	userland/desktop/libkeiland/ui/field.c \
	userland/desktop/libkeiland/ui/list.c \
	userland/desktop/libkeiland/ui/views.c \
	userland/desktop/libkeiland/ui/cards.c \
	userland/desktop/libkeiland/ui/chooser.c \
	userland/desktop/libkeiland/ui/chooser-model.c \
	userland/desktop/libkeiland/ui/chooser-view.c \
	userland/desktop/libkeiland/ui/picker.c \
	userland/desktop/libkeiland/ui/picker-model.c \
	userland/desktop/libkeiland/ui/picker-view.c \
	userland/desktop/picture/color-glyph.c
