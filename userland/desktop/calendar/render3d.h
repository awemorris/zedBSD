/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Calendar's small 3D renderer (WS155 p000): triangles drawn in software
 * into a target of colour and depth, lit by one light, for the mock's
 * desk calendar and its icons.  The compositor's GPU cannot be reached
 * from an application yet; how the real application draws in 3D is
 * decided in p001.
 *
 * Space: the camera is at the origin and looks along +z, with x to the
 * right and y up; a point at (x, y, z) shows at (cx + focal x / z,
 * cy - focal y / z) of the target.  A triangle faces the camera when the
 * cross product of its first two edges points to the camera; the back of
 * one is not drawn unless it is two-sided, and is lit as it faces.
 *
 * The target is drawn larger than it shows (a factor across and down) and
 * averaged down into a picture, which smooths the edges.
 */

#ifndef CALENDAR_RENDER3D_H
#define CALENDAR_RENDER3D_H

#include <keiland/keiland.h>

#include <stddef.h>
#include <stdint.h>

/* The way a triangle is drawn (bits). */
#define R3_TWO_SIDED		1U	/* the back is drawn too, lit as a front */
#define R3_NO_DEPTH_WRITE	2U	/* tested against the depth but leaves it (see-through things) */
#define R3_UNLIT		4U	/* its color as it is, without the light */

/* A point or a direction. */
struct r3_vec {
	float x;
	float y;
	float z;
};

/* A rotation and a move: a point p goes to m p + t. */
struct r3_matrix {
	float m[3][3];
	struct r3_vec t;
};

/* A corner of a triangle: where it is, and where in the texture (0..1). */
struct r3_vertex {
	struct r3_vec position;
	float u;
	float v;
};

/* A texture: premultiplied 0xAARRGGBB pixels. */
struct r3_texture {
	const uint32_t *pixels;
	int width;
	int height;
	size_t stride;
};

/*
 * A target: its premultiplied colour and its depth (1/z, 0 for nothing)
 * per pixel, its size, and the projection: the focal length and the
 * centre (pixels of the target), the direction to the light, and the
 * light everything has without it.
 */
struct r3_target {
	uint32_t *color;
	float *depth;
	int width;
	int height;
	float focal;
	float cx;
	float cy;
	struct r3_vec light;
	float ambient;
};

int r3_target_init(struct r3_target *target, int width, int height);
void r3_target_release(struct r3_target *target);
void r3_target_clear(struct r3_target *target);
void r3_triangle(struct r3_target *target, const struct r3_vertex *corners, kl_color color, const struct r3_texture *texture, unsigned flags);
void r3_resolve(const struct r3_target *target, int factor, struct kl_image *image);

void r3_identity(struct r3_matrix *matrix);
void r3_rotate_x(struct r3_matrix *matrix, float radians);
void r3_rotate_y(struct r3_matrix *matrix, float radians);
void r3_rotate_z(struct r3_matrix *matrix, float radians);
void r3_translate(struct r3_matrix *matrix, float x, float y, float z);
void r3_combine(const struct r3_matrix *outer, const struct r3_matrix *inner, struct r3_matrix *result);
struct r3_vec r3_apply(const struct r3_matrix *matrix, struct r3_vec point);
struct r3_vec r3_cross(struct r3_vec a, struct r3_vec b);
float r3_dot(struct r3_vec a, struct r3_vec b);
struct r3_vec r3_normalize(struct r3_vec a);

#endif
