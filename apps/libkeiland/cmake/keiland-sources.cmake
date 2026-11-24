# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

#
# The source lists of libkeiland for the CMake builds of the programs that use it.
# Paths are relative to the root of the tree; the including build prepends its root.
#
#   KEILAND_COMMON_SOURCES  the files every system shares
#   KEILAND_WIN32_SOURCES   a Windows program's files: the platform layer, the Windows
#                           backend (apps/libkeiland/win32) and its stubs of what
#                           Windows has not
#   KEILAND_POSIX_SOURCES   the platform layer of a POSIX system (threads, sockets)
#   KEILAND_ANDROID_SOURCES an Android program's files: the Android backend
#                           (apps/libkeiland/android) and its stubs of what Android has not;
#                           with KEILAND_POSIX_SOURCES
#
set(KEILAND_COMMON_SOURCES
	userland/desktop/libkeiland/version.c
	userland/desktop/libkeiland/motion.c
	userland/desktop/libkeiland/scroll.c
	userland/desktop/libkeiland/gesture.c
	userland/desktop/libkeiland/settings-cache.c
	userland/desktop/settings-keys/settings-keys.c
	userland/desktop/libkeiland/translate-follow.c
	userland/desktop/libkeiland/system/system-monitor-rate.c
	userland/desktop/libkeiland/ui/canvas.c
	userland/desktop/libkeiland/ui/drop-look.c
	userland/desktop/libkeiland/ui/icons.c
	userland/desktop/libkeiland/ui/icons-line.c
	userland/desktop/libkeiland/ui/theme.c
	userland/desktop/libkeiland/ui/input.c
	userland/desktop/libkeiland/ui/scroll.c
	userland/desktop/libkeiland/ui/scroll-bar.c
	userland/desktop/libkeiland/ui/text-touch.c
	userland/desktop/libkeiland/ui/text-bar.c
	userland/desktop/libkeiland/ui/text-select.c
	userland/desktop/libkeiland/ui/ui.c
	userland/desktop/libkeiland/launch.c
	userland/desktop/libkeiland/ui/declare.c
	userland/desktop/libkeiland/ui/present-copy.c
	userland/desktop/libkeiland/ui/present-damage.c
	userland/desktop/libkeiland/ui/text-area.c
	userland/desktop/libkeiland/ui/widgets.c
	userland/desktop/libkeiland/ui/field.c
	userland/desktop/libkeiland/ui/list.c
	userland/desktop/libkeiland/ui/views.c
	userland/desktop/libkeiland/ui/cards.c
	userland/desktop/libkeiland/ui/chooser-view.c
	userland/desktop/libkeiland/ui/picker-view.c
	userland/desktop/libkeiland/ui/text.c
	userland/desktop/picture/color-glyph.c)

set(KEILAND_WIN32_SOURCES
	userland/desktop/libkeiland/platform/platform-win32.c
	userland/desktop/libkeiland/platform/thread-win32.c
	userland/desktop/libkeiland/platform/socket-win32.c
	apps/libkeiland/win32/keys-table.c
	apps/libkeiland/win32/frame.c
	apps/libkeiland/win32/wide.c
	apps/libkeiland/win32/input.c
	apps/libkeiland/win32/cf-html.c
	apps/libkeiland/win32/app.c
	apps/libkeiland/win32/window.c
	apps/libkeiland/win32/present.c
	apps/libkeiland/win32/places.c
	apps/libkeiland/win32/theme.c
	apps/libkeiland/win32/titlebar.c
	apps/libkeiland/win32/ime.c
	apps/libkeiland/win32/clipboard.c
	apps/libkeiland/win32/pointer.c
	apps/libkeiland/win32/drag.c
	apps/libkeiland/win32/drop.c
	apps/libkeiland/win32/unsupported.c)

set(KEILAND_POSIX_SOURCES
	userland/desktop/libkeiland/platform/platform-posix.c
	userland/desktop/libkeiland/platform/thread-posix.c
	userland/desktop/libkeiland/platform/socket-posix.c)

set(KEILAND_ANDROID_SOURCES
	apps/libkeiland/android/activity.c
	apps/libkeiland/android/app.c
	apps/libkeiland/android/window.c
	apps/libkeiland/android/present.c
	apps/libkeiland/android/input.c
	apps/libkeiland/android/keys-table.c
	apps/libkeiland/android/ime.c
	apps/libkeiland/android/clipboard.c
	apps/libkeiland/android/titlebar.c
	apps/libkeiland/android/tabs.c
	apps/libkeiland/android/places.c
	apps/libkeiland/android/theme.c
	apps/libkeiland/android/bridge.c
	apps/libkeiland/android/bridge-jni.c
	apps/libkeiland/android/unsupported.c)
