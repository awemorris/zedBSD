WAYLAND_MAKEFILE_DIR := $(dir $(lastword $(MAKEFILE_LIST)))
include $(WAYLAND_MAKEFILE_DIR)../package.mk
# The compositor's operating system (WS131): libkeiland-backend's zedBSD side.
include $(WAYLAND_MAKEFILE_DIR)../libkeiland-backend-zedbsd/sources.mk

# Wayland compositor: every window composed with Vulkan into a VK_KHR_display swapchain.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
# The screen capture only in a test image (ws173-p002); every other compositor, as on Linux and FreeBSD, has none.
KEILAND_SHOT_SOURCE := userland/desktop/wayland/shot-none.c
ifeq ($(ZEDBSD_TEST_SCREEN_CAPTURE),y)
KEILAND_SHOT_SOURCE := userland/desktop/wayland/shot.c
endif
KEILAND_SOURCES := userland/desktop/wayland/main.c userland/desktop/wayland/wire.c \
	userland/desktop/wayland/objects.c userland/desktop/wayland/protocol.c userland/desktop/wayland/display.c \
	$(KL_BACKEND_ZEDBSD_SOURCES) userland/desktop/wayland/gpu-host.c \
	userland/desktop/wayland/seat.c userland/desktop/wayland/input.c userland/desktop/wayland/tablet.c userland/desktop/wayland/touch.c userland/desktop/wayland/touchpad.c userland/desktop/wayland/pointer-accel.c userland/desktop/wayland/network-info.c userland/desktop/wayland/super-tap.c userland/desktop/wayland/apps.c userland/desktop/wayland/apps-bar.c userland/desktop/wayland/switcher.c userland/desktop/wayland/switcher-shell.c userland/desktop/wayland/layout.c userland/desktop/wayland/swipe.c userland/desktop/wayland/edge.c userland/desktop/wayland/arrange.c userland/desktop/wayland/arrange-shell.c userland/desktop/wayland/power-layout.c userland/desktop/wayland/lock-clock.c userland/desktop/wayland/dnd-state.c userland/desktop/wayland/lock-swipe.c userland/desktop/wayland/power-dialog.c userland/desktop/wayland/title-tap.c userland/desktop/wayland/lid.c userland/desktop/wayland/sleep-rules.c userland/desktop/wayland/sleep.c userland/desktop/wayland/output-switch.c userland/desktop/wayland/heads.c userland/desktop/wayland/plane.c userland/desktop/wayland/surface-outputs.c userland/desktop/wayland/displays.c userland/desktop/wayland/displays-shell.c \
	userland/desktop/wayland/compose.c userland/desktop/wayland/import.c userland/desktop/wayland/shm.c \
	userland/desktop/wayland/glass.c userland/desktop/wayland/shell.c userland/desktop/wayland/home.c userland/desktop/wayland/corner.c userland/desktop/wayland/keyboard.c userland/desktop/wayland/keyboard-layout.c userland/desktop/wayland/keyboard-hand.c userland/desktop/wayland/hand-cloud.c \
	userland/desktop/wayland/menu.c userland/desktop/wayland/menu-shell.c userland/desktop/wayland/titlebar.c \
	userland/desktop/wayland/popup.c userland/desktop/wayland/toplevel.c userland/desktop/wayland/sheet.c userland/desktop/wayland/subsurface.c userland/desktop/wayland/keymap.c userland/desktop/wayland/data.c \
	userland/desktop/wayland/icons.c userland/desktop/wayland/titlebar-shell.c \
	userland/desktop/wayland/decoration.c userland/desktop/wayland/cursor.c userland/desktop/wayland/viewport.c userland/desktop/wayland/content-type.c userland/desktop/wayland/scanout.c userland/desktop/wayland/scanout-rules.c userland/desktop/wayland/activation.c userland/desktop/wayland/theme.c \
	userland/desktop/wayland/panels.c userland/desktop/wayland/backdrop.c userland/desktop/wayland/damage.c userland/desktop/wayland/greeter.c userland/desktop/wayland/handoff.c userland/desktop/wayland/role.c userland/desktop/wayland/backend-host.c userland/desktop/wayland/os.c userland/desktop/wayland/primary.c \
	userland/desktop/wayland/network.c userland/desktop/wayland/volume.c userland/desktop/wayland/media.c userland/desktop/wayland/language.c \
	userland/desktop/wayland/settings.c userland/desktop/wayland/settings-store.c $(KEILAND_SHOT_SOURCE) userland/desktop/wayland/system.c userland/desktop/wayland/sysmon.c userland/desktop/wayland/notify.c userland/desktop/wayland/notify-shell.c userland/desktop/wayland/notify-flow.c userland/desktop/wayland/notify-popup.c userland/desktop/wayland/notify-system.c userland/desktop/wayland/notify-log.c userland/desktop/wayland/mail-shell.c userland/desktop/wayland/phone-shell.c userland/desktop/wayland/printers-shell.c userland/desktop/wayland/bluetooth-shell.c userland/desktop/wayland/audio-stream.c userland/desktop/wayland/bluetooth-ask.c userland/desktop/wayland/bluetooth-bar.c userland/desktop/wayland/machine-shell.c userland/desktop/wayland/machine-wait.c userland/desktop/wayland/language-file.c userland/desktop/settings-keys/settings-keys.c \
	userland/desktop/wayland/text-input.c userland/desktop/wayland/input-method.c \
	userland/desktop/wayland/desktop.c userland/desktop/wayland/inset.c userland/desktop/wayland/edit.c userland/desktop/wayland/clipboard.c \
	userland/tests/vkdemo/display.c userland/desktop/artwork/mark.c userland/desktop/picture/color-glyph.c \
	userland/desktop/picture/wallpaper.c
# App Home starts X11 applications through keiland-x11, which starts xserver when it is needed.
ZEDBSD_PACKAGE_INPUTS += userland/desktop/wayland/keiland-x11
ZEDBSD_PACKAGE_FILES += $(if $(filter wayland,$(ZEDBSD_USER_PROGRAMS)),--file /usr/libexec/keiland-x11=userland/desktop/wayland/keiland-x11)
# App Home's default list (ws129-p010), unless the image brings its own (a test's or the demonstration's extra files,
# which would otherwise name the same destination twice).
ZEDBSD_PACKAGE_INPUTS += userland/desktop/wayland/apps.conf
ZEDBSD_PACKAGE_FILES += $(if $(filter wayland,$(ZEDBSD_USER_PROGRAMS)),$(if $(findstring /etc/keiland/apps.conf=,$(ZEDBSD_TEST_EXTRA_FILES)),,\
	--file /etc/keiland/apps.conf=userland/desktop/wayland/apps.conf --mode /etc/keiland/apps.conf=0644))
KEILAND_FONT_DATA := /usr/share/fonts/keiland.ttf=userland/desktop/fonts/Mahora-Regular.ttf \
	/usr/share/fonts/keiland-bold.ttf=userland/desktop/fonts/Mahora-Bold.ttf \
	/usr/share/fonts/keiland-mono.ttf=userland/desktop/fonts/Mahora-Mono.ttf \
	/usr/share/fonts/keiland-fallback-mono.ttf=userland/desktop/fonts/JetBrainsMono-Regular.ttf \
	/usr/share/fonts/keiland-fallback.ttf=userland/desktop/fonts/DroidSansFallbackFull.ttf \
	/usr/share/licenses/keiland-fonts/Mahora-LICENSE.txt=userland/desktop/fonts/Mahora-LICENSE.txt \
	/usr/share/licenses/keiland-fonts/JetBrainsMono-OFL.txt=userland/desktop/fonts/JetBrainsMono-OFL.txt \
	/usr/share/licenses/keiland-fonts/DroidSansFallback-LICENSE.txt=userland/desktop/fonts/DroidSansFallback-LICENSE.txt \
	/usr/share/licenses/keiland-fonts/Apache-2.0.txt=userland/desktop/fonts/Apache-2.0.txt
$(eval $(call ZEDBSD_USERLAND_PACKAGE,wayland,Wayland compositor,amd64,y,basic,$(KEILAND_SOURCES),y,desktop,desktop/wayland,desktop/libvulkan desktop/libtruetype desktop/libkeiland desktop/sessiond base/libpng-compat base/libz-compat base/libjpeg-compat fonts/noto-color-emoji fonts/hand-hershey,0755,program,$(KEILAND_FONT_DATA),,bin))
