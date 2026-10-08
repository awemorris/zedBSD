/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * What the bar's applications (apps-bar.c, ws142-p004) share with the
 * switcher (switcher-shell.c, ws142-p005): the desktop's applications
 * gathered from its windows, the icons' places, the previews' layout, and
 * the bar's previews shown and hidden.
 */

#ifndef KWL_APPS_BAR_H
#define KWL_APPS_BAR_H

#include "kwl.h"
#include "apps.h"

#include <stdint.h>

/*
 * An icon's place in the bar and its mark's size, and the padding of the
 * applications' pill at its ends (pixels; ws099-p034: 26-pixel tiles 8
 * apart in one pill).
 */
#define ICON_WIDTH		34
#define ICON_MARK		26
#define ICON_PILL_PAD		6

/* The panel of previews: its distance under the bar, its padding, the gap between previews and the room for a preview's label (pixels). */
#define PANEL_DROP		8
#define PANEL_PAD		16
#define PREVIEW_GAP		16
#define PREVIEW_LABEL		46

/* The most windows looked at on a desktop. */
#define VIEW_WINDOWS		64U

/* A rectangle on the output. */
struct apps_rect {
	int32_t x;
	int32_t y;
	int32_t width;
	int32_t height;
};

/*
 * The desktop's applications this moment on an output's bar, and (with
 * room in the bar) where their icons go: the output (the system bar's
 * anchor, whose bar and switcher have the desktop's every window, or a
 * head, whose bar has the windows on it, ws113-p015), its rectangle and
 * its bar's top in the plane.
 */
struct apps_view {
	unsigned output;
	struct kwl_plane_rect area;
	int32_t top;
	struct kwl_apps apps;
	struct kwl_apps_window described[VIEW_WINDOWS];
	struct kwl_object *surfaces[VIEW_WINDOWS];
	unsigned window_count;
	int32_t left;
	int32_t right;
	unsigned shown;
	unsigned hidden;
	int current;
};

/* The previews of one application. */
struct apps_panel {
	int app;
	unsigned count;
	struct kwl_object *surfaces[KWL_APPS_WINDOWS];
	struct apps_rect tiles[KWL_APPS_WINDOWS];
	struct apps_rect rect;
};

int kwl_apps_view_build(struct kwl_server *server, struct apps_view *view);
int kwl_apps_view_build_on(struct kwl_server *server, unsigned slot, struct apps_view *view);
int kwl_apps_view_collect(struct kwl_server *server, struct apps_view *view);
void kwl_apps_tiles_layout(struct kwl_server *server, const struct apps_view *view, unsigned found, struct apps_panel *panel);
int kwl_apps_tile_at(const struct apps_rect *tiles, unsigned count, int32_t x, int32_t y);
int kwl_apps_inside(const struct apps_rect *rect, int32_t x, int32_t y);
void kwl_apps_bar_show(struct kwl_server *server, const struct apps_view *view, const char *key, unsigned via);
void kwl_apps_bar_hide(struct kwl_server *server, const char *why);
int kwl_apps_bar_drag_motion(struct kwl_server *server);
void kwl_apps_bar_drag_end(struct kwl_server *server);
int kwl_apps_bar_panel(struct kwl_server *server, const struct apps_view *view, const char *key, struct apps_panel *panel);

#endif
