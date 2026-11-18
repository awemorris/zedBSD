/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Calendar's 3D things (WS155 p000): meshes made of boxes, quads,
 * extruded outlines, ellipsoids and rings in a model's own space, drawn
 * with render3d.h through a matrix; the desk calendar with its pages and
 * its rings, and the icons of the kinds of event.
 */

#ifndef CALENDAR_SCENE_H
#define CALENDAR_SCENE_H

#include "render3d.h"

/* The most triangles of a mesh. */
#define SC_TRIANGLES_MAX	3072U

/* The textures a mesh's triangles may name: none, the page shown, the page under it. */
#define SC_TEXTURE_NONE		-1
#define SC_TEXTURE_PAGE		0
#define SC_TEXTURE_NEXT		1
#define SC_TEXTURES		2

/* The kinds of event, whose icons are drawn in 3D. */
enum sc_icon {
	SC_ICON_WORK,
	SC_ICON_PERSONAL,
	SC_ICON_STUDY,
	SC_ICON_FAMILY,
	SC_ICONS
};

/* One triangle of a mesh: its corners, its color, its texture (SC_TEXTURE_*) and how it is drawn (R3_*). */
struct sc_triangle {
	struct r3_vertex corners[3];
	kl_color color;
	int texture;
	unsigned flags;
};

/* A mesh: its triangles. */
struct sc_mesh {
	struct sc_triangle triangles[SC_TRIANGLES_MAX];
	size_t count;
};

void sc_mesh_clear(struct sc_mesh *mesh);
void sc_quad(struct sc_mesh *mesh, const struct r3_vec *corners, struct r3_vec normal, kl_color color, int texture, unsigned flags);
void sc_box(struct sc_mesh *mesh, struct r3_vec low, struct r3_vec high, kl_color color);
void sc_extrude(struct sc_mesh *mesh, const float *outline, size_t count, float front, float back, kl_color color);
void sc_ellipsoid(struct sc_mesh *mesh, struct r3_vec centre, struct r3_vec radii, kl_color color);
void sc_ring(struct sc_mesh *mesh, struct r3_vec centre, float major, float minor, kl_color color);
void sc_desk_calendar(struct sc_mesh *mesh, float flip, float opacity);
void sc_icon(struct sc_mesh *mesh, enum sc_icon icon);
void sc_draw(struct r3_target *target, const struct sc_mesh *mesh, const struct r3_matrix *matrix, const struct r3_texture *textures);

#endif
