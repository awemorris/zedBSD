/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The glass look's drawing, shared by glass.c (images, glyphs, shapes) and
 * shell.c (windows, title bars, the system bar).
 */

#ifndef ZWL_GLASS_H
#define ZWL_GLASS_H

#include "compose.h"

/* The atlas's index of the multiplication sign (the close button). */
#define GLASS_CLOSE_GLYPH	95U

/* The shapes the panel shader draws (shaders/panel.frag). */
#define MODE_GLASS		0.0f
#define MODE_SHADOW		1.0f
#define MODE_IMAGE		2.0f
#define MODE_SOLID		3.0f
#define MODE_RING		4.0f
#define MODE_TEXT		5.0f

/* The corner radius of title bars and bodies. */
#define GLASS_RADIUS		14.0f

/* The text sizes: the system bar, the titles, the close sign. */
enum glass_size {
	SIZE_BAR,
	SIZE_TITLE,
	SIZE_SIGN
};

/*
 * One shape for the panel shader, in output pixels: the quad drawn, the
 * rounded box the shader measures from (it may reach past the quad), the
 * part of the image, a color, and how the shape is drawn.  opacity fades the
 * whole shape.  A shape with no image of its own gives the shader the
 * blurred wallpaper.
 */
struct glass_shape {
	float quad[4];
	float box[4];
	float uv[4];
	float color[4];
	float radius;
	float mode;
	float soft;
	float opaque;
	float edge;
	float opacity;
	VkDescriptorSet set;
};

void glass_shape_init(struct glass_shape *shape, float x, float y, float width, float height);
void glass_shape_draw(struct zwl_server *server, VkCommandBuffer command, const struct glass_shape *shape);
void glass_draw_solid(struct zwl_server *server, VkCommandBuffer command, float x, float y, float width, float height, float radius, const float *color);
int32_t glass_text_width(struct zwl_server *server, enum glass_size size, const char *text);
void glass_draw_text(struct zwl_server *server, VkCommandBuffer command, enum glass_size size, int32_t x, int32_t baseline, const char *text, int32_t limit, const float *color);
void glass_draw_glyph(struct zwl_server *server, VkCommandBuffer command, enum glass_size size, unsigned index, int32_t x, int32_t baseline, const float *color);
int32_t glass_glyph_advance(struct zwl_server *server, enum glass_size size, unsigned index);
VkDescriptorSet glass_wallpaper_set(struct zwl_server *server);

#endif
