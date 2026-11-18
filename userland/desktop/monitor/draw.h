/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The System Monitor's drawing primitives (draw.c) and the colour tokens
 * (design.md section 2.4), shared by the 2D plates (scene.c) and the 3D
 * parts (space.c).
 */

#ifndef MONITOR_DRAW_H
#define MONITOR_DRAW_H

#include "app.h"

/* A colour as four floats. */
struct sm_color {
	float r;
	float g;
	float b;
	float a;
};

/* The colour tokens (design.md section 2.4). */
#define TOKEN_BG_DEEP		0x10151eU
#define TOKEN_BG_MID		0x181f2bU
#define TOKEN_SURFACE_0		0x1b232fU
#define TOKEN_SURFACE_1		0x1f2836U
#define TOKEN_SURFACE_2		0x26303fU
#define TOKEN_EDGE_0		0x303d4eU
#define TOKEN_EDGE_1		0x405268U
#define TOKEN_EDGE_2		0x5c7692U
#define TOKEN_TEXT		0xd6e0ecU
#define TOKEN_TEXT_DIM		0x8a9ab0U
#define TOKEN_TEXT_FAINT	0x5e6d82U
#define TOKEN_CYAN		0x5cc4e6U
#define TOKEN_ICE		0xaad6f0U
#define TOKEN_MINT		0x6ed6b0U
#define TOKEN_AMBER		0xe6a84cU
#define TOKEN_CORAL		0xd8664eU

struct sm_color sm_rgb(uint32_t rgb, float alpha);
struct sm_color sm_color_mix(struct sm_color from, struct sm_color to, float amount);
int sm_scene_reserve(struct sm_scene *scene, size_t vertices);
void sm_draw_quad(struct sm_scene *scene, unsigned pipe, const float *corners, const float *locals, const float *shape, struct sm_color color, struct sm_color second);
void sm_draw_rect(struct sm_scene *scene, float x, float y, float width, float height, struct sm_color color);
void sm_draw_gradient(struct sm_scene *scene, float x, float y, float width, float height, struct sm_color top, struct sm_color bottom);
void sm_draw_round(struct sm_scene *scene, float x, float y, float width, float height, float radius, struct sm_color fill, struct sm_color edge);
void sm_draw_plate(struct sm_scene *scene, const struct sm_box *box, unsigned layer, float radius);
void sm_draw_disc(struct sm_scene *scene, float cx, float cy, float radius, struct sm_color color);
float sm_draw_text(struct sm_app *app, enum sm_style style, float x, float baseline, const char *text, struct sm_color color, int align);
void sm_draw_segment(struct sm_scene *scene, float x0, float y0, float x1, float y1, float thickness, struct sm_color color);
struct sm_color sm_level_color(enum sm_level level);
void sm_draw_triangle(struct sm_scene *scene, const float *corners, struct sm_color color);

#endif
