/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The icons, part of <keiland/keiland.h>.
 */

#ifndef KEILAND_ICON_H
#define KEILAND_ICON_H

#include <keiland/keiland_canvas.h>

#ifdef __cplusplus
extern "C" {
#endif

struct kl_text;

/*
 * The icons drawn with lines (sidebar, toolbar) or filled shapes (items).
 */
enum kl_icon {
	KL_ICON_HOME,
	KL_ICON_DESKTOP,
	KL_ICON_DOCUMENTS,
	KL_ICON_DOWNLOADS,
	KL_ICON_PICTURES,
	KL_ICON_MUSIC,
	KL_ICON_MOVIES,
	KL_ICON_FOLDER_LINE,
	KL_ICON_RECENTS,
	KL_ICON_TRASH,
	KL_ICON_COMPUTER,
	KL_ICON_VOLUME,
	KL_ICON_BACK,
	KL_ICON_FORWARD,
	KL_ICON_SEARCH,
	KL_ICON_GRID,
	KL_ICON_LIST,
	KL_ICON_PREVIEW,
	KL_ICON_CHEVRON,
	KL_ICON_CLOSE,
	KL_ICON_PLUS,
	KL_ICON_UP,
	KL_ICON_DOWN,

	/* The line pictures (Settings' pages), from KL_ICON_TILES on, in the order Settings numbered them. */
	KL_ICON_TILES,
	KL_ICON_WIFI,
	KL_ICON_ETHERNET,
	KL_ICON_BLUETOOTH,
	KL_ICON_SHIELD,
	KL_ICON_GLOBE,
	KL_ICON_PALETTE,
	KL_ICON_PICTURE,
	KL_ICON_BELL,
	KL_ICON_SPEAKER,
	KL_ICON_MONITOR,
	KL_ICON_DISK,
	KL_ICON_BATTERY,
	KL_ICON_KEYBOARD,
	KL_ICON_MOUSE,
	KL_ICON_TOUCHPAD,
	KL_ICON_PRINTER,
	KL_ICON_SHARE,
	KL_ICON_PEOPLE,
	KL_ICON_EYE,
	KL_ICON_LOCK,
	KL_ICON_PERSON,
	KL_ICON_REFRESH,
	KL_ICON_INFO,
	KL_ICON_DISCLOSURE,

	/* Files' sidebar's Today, a calendar's page (KL_VERSION 47). */
	KL_ICON_TODAY,

	/*
	 * KL_VERSION 60: a cross in a circle, leaving a network (Settings' line
	 * picture), and a device's eject, a block over a bar (Files' sidebar),
	 * filled.
	 */
	KL_ICON_DISCONNECT,
	KL_ICON_EJECT
};

/* The icons (icons.c and icons-line.c). */
void kl_icon_draw(struct kl_canvas *canvas, enum kl_icon icon, float x, float y, float size, kl_color color);
void kl_icon_folder(struct kl_canvas *canvas, float x, float y, float size, kl_color tint);
void kl_icon_file(struct kl_canvas *canvas, struct kl_text *text, float x, float y, float size, kl_color band, const char *label);
void kl_icon_tag(struct kl_canvas *canvas, float cx, float cy, float radius, kl_color color);

#ifdef __cplusplus
}
#endif

#endif
