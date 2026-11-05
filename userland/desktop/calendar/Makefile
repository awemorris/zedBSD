include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

# Calendar (WS155): Keiland's calendar -- the months, a week or a day with the events of four calendars (kept as
# iCalendar files under ~/Documents/Calendar), a panel to add an event by dragging its kind onto a date, the memo, and a
# desk calendar drawn in 3D by a small software renderer.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
CALENDAR_SOURCES := userland/desktop/calendar/main.c userland/desktop/calendar/view.c \
	userland/desktop/calendar/store.c userland/desktop/calendar/date.c \
	userland/desktop/calendar/scene.c userland/desktop/calendar/render3d.c
$(eval $(call USERLAND_PACKAGE,calendar,calendar - Calendar,amd64,n,basic,$(CALENDAR_SOURCES),y,desktop,desktop/calendar,desktop/libvulkan desktop/libwayland desktop/libkeiland desktop/libtruetype,0755,program,,,bin))
