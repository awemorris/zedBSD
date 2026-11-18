/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * A window's glass panels (kl_glass_v1, panels.c): the parts of a
 * surface that stand on the system's frosted glass, shared with the
 * drawing of windows (shell.c).
 */

#ifndef KWL_PANELS_H
#define KWL_PANELS_H

#include "glass.h"

/* The most panels a surface may have. */
#define KWL_PANELS_MAX		32U

/* The kind of panel (the only one so far): a card floating in the window. */
#define KWL_PANEL_CARD		0U

/* The largest corner radius a panel may ask for. */
#define KWL_PANEL_RADIUS_MAX	64

/*
 * One glass panel, in surface coordinates: its rectangle, its corners'
 * radius and its kind.
 */
struct kwl_panel {
	int32_t x;
	int32_t y;
	int32_t width;
	int32_t height;
	int32_t radius;
	uint32_t kind;
};

/*
 * A surface's glass panels: those set for its next commit and those its
 * last commit applied.  The surface owns the record from its first
 * kl_glass_v1 to its own end.
 */
struct kwl_panels {
	struct kwl_panel pending[KWL_PANELS_MAX];
	unsigned pending_count;
	unsigned changed;
	struct kwl_panel current[KWL_PANELS_MAX];
	unsigned count;
	/*
	 * Whether the surface's glass shows the windows under it blurred
	 * (kl_glass_v1.set_blur, version 2, ws075-p029): pending, and
	 * committed.  0, the default, is the blurred wallpaper alone.
	 */
	unsigned pending_blur;
	unsigned blur;
};

int kwl_panels_request(struct kwl_object *object, uint32_t opcode, const unsigned char *bytes, size_t size);
void kwl_panels_commit(struct kwl_object *surface);
void kwl_panels_object_gone(struct kwl_object *object);
unsigned kwl_panels_count(const struct kwl_object *surface);
unsigned kwl_panels_blur(const struct kwl_object *surface);
void kwl_panels_draw(struct kwl_server *server, struct kwl_list *command, const struct kwl_object *surface, const float *place, float opacity, unsigned shadows);
unsigned kwl_panels_holes(const struct kwl_object *surface, const float *place, float opacity, float (*holes)[4], unsigned room);

#endif
