/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Calendar's small 3D renderer (WS155 p000; render3d.h): each triangle is
 * lit once (flat, with a soft highlight), projected, and filled pixel by
 * pixel within its bounding box by its barycentric weights; the depth is
 * 1/z, which interpolates linearly across the screen, and a texture is
 * sampled with perspective-correct coordinates and bilinear filtering.
 */

#include "render3d.h"

#include <errno.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

/* The nearest a corner may be to the camera; a triangle nearer is not drawn. */
#define R3_NEAR			0.05f

/* The highlight's strength and tightness. */
#define R3_SPECULAR		0.22f
#define R3_SHININESS		24.0f

/* The color of a sample: premultiplied components, 0..1. */
struct r3_sample {
	float alpha;
	float red;
	float green;
	float blue;
};

static void r3_texture_sample(const struct r3_texture *texture, float u, float v, struct r3_sample *sample);
static int r3_clamp(int value, int last);
static float r3_edge(float ax, float ay, float bx, float by, float px, float py);
static uint32_t r3_pack(const struct r3_sample *sample);

/*
 * Makes a target of a size, cleared; returns an errno value.
 */
int
r3_target_init(
	struct r3_target *target,
	int width,
	int height)
{
	/* A target has a pixel at least. */
	memset(target, 0, sizeof(target[0]));
	if (width <= 0 || height <= 0)
		return EINVAL;

	/* The colours. */
	target->color = calloc((size_t)width * (size_t)height, sizeof(target->color[0]));
	if (target->color == NULL)
		return ENOMEM;

	/* The depths. */
	target->depth = calloc((size_t)width * (size_t)height, sizeof(target->depth[0]));
	if (target->depth == NULL) {
		free(target->color);
		target->color = NULL;
		return ENOMEM;
	}

	/* Succeeded: the size, the centre, and a light from the upper left in front. */
	target->width = width;
	target->height = height;
	target->focal = (float)width;
	target->cx = (float)width / 2.0f;
	target->cy = (float)height / 2.0f;
	target->light.x = -0.3f;
	target->light.y = 0.45f;
	target->light.z = -0.85f;
	target->light = r3_normalize(target->light);
	target->ambient = 0.62f;
	return 0;
}

/*
 * Frees a target's pixels.
 */
void
r3_target_release(
	struct r3_target *target)
{
	/* The colours and the depths. */
	free(target->color);
	free(target->depth);
	target->color = NULL;
	target->depth = NULL;
}

/*
 * Clears a target to transparent and to nothing in depth.
 */
void
r3_target_clear(
	struct r3_target *target)
{
	size_t count;

	/* Every pixel. */
	count = (size_t)target->width * (size_t)target->height;
	memset(target->color, 0, count * sizeof(target->color[0]));
	memset(target->depth, 0, count * sizeof(target->depth[0]));
}

/*
 * Draws a triangle (its corners in the camera's space) in a color (not
 * premultiplied, its alpha the triangle's opacity), or with a texture
 * whose samples the light shades, in the way the flags say.
 */
void
r3_triangle(
	struct r3_target *target,
	const struct r3_vertex *corners,
	kl_color color,
	const struct r3_texture *texture,
	unsigned flags)
{
	struct r3_sample sample;
	struct r3_vec normal;
	struct r3_vec edge_a;
	struct r3_vec edge_b;
	struct r3_vec view;
	struct r3_vec half;
	float sx[3];
	float sy[3];
	float w[3];
	float area;
	float inverse;
	float shade;
	float specular;
	float facing;
	float diffuse;
	float opacity;
	float b0;
	float b1;
	float b2;
	float depth;
	float u;
	float v;
	float keep;
	float px;
	float py;
	float min_x;
	float max_x;
	float min_y;
	float max_y;
	size_t at;
	int left;
	int right;
	int top;
	int bottom;
	int x;
	int y;
	int i;

	/* A corner behind or too near the camera: not drawn. */
	for (i = 0; i < 3; i++) {
		/* One corner. */
		if (corners[i].position.z < R3_NEAR)
			return;
	}

	/* Its normal, from its first two edges. */
	edge_a.x = corners[1].position.x - corners[0].position.x;
	edge_a.y = corners[1].position.y - corners[0].position.y;
	edge_a.z = corners[1].position.z - corners[0].position.z;
	edge_b.x = corners[2].position.x - corners[0].position.x;
	edge_b.y = corners[2].position.y - corners[0].position.y;
	edge_b.z = corners[2].position.z - corners[0].position.z;
	normal = r3_cross(edge_a, edge_b);
	normal = r3_normalize(normal);

	/* The direction from its middle to the camera. */
	view.x = -(corners[0].position.x + corners[1].position.x + corners[2].position.x) / 3.0f;
	view.y = -(corners[0].position.y + corners[1].position.y + corners[2].position.y) / 3.0f;
	view.z = -(corners[0].position.z + corners[1].position.z + corners[2].position.z) / 3.0f;
	view = r3_normalize(view);

	/* Which way it faces: the back of a one-sided triangle is not drawn, a two-sided one turns to the camera. */
	facing = r3_dot(normal, view);
	if (facing <= 0.0f) {
		/* Its back. */
		if ((flags & R3_TWO_SIDED) == 0U)
			return;
		normal.x = -normal.x;
		normal.y = -normal.y;
		normal.z = -normal.z;
	}

	/* The light on it: the light all round, the light's own, and a soft highlight. */
	shade = 1.0f;
	specular = 0.0f;
	if ((flags & R3_UNLIT) == 0U) {
		/* Lit. */
		diffuse = r3_dot(normal, target->light);
		if (diffuse < 0.0f)
			diffuse = 0.0f;
		shade = target->ambient + (1.0f - target->ambient) * diffuse;

		/* The highlight, between the light and the camera. */
		half.x = target->light.x + view.x;
		half.y = target->light.y + view.y;
		half.z = target->light.z + view.z;
		half = r3_normalize(half);
		specular = r3_dot(normal, half);
		if (specular < 0.0f)
			specular = 0.0f;
		specular = R3_SPECULAR * powf(specular, R3_SHININESS);
	}

	/* The corners on the target, with 1/z for the depth and the texture. */
	for (i = 0; i < 3; i++) {
		/* One corner. */
		w[i] = 1.0f / corners[i].position.z;
		sx[i] = target->cx + target->focal * corners[i].position.x * w[i];
		sy[i] = target->cy - target->focal * corners[i].position.y * w[i];
	}

	/* A triangle with no area covers nothing. */
	area = r3_edge(sx[0], sy[0], sx[1], sy[1], sx[2], sy[2]);
	if (area > -1e-6f && area < 1e-6f)
		return;
	inverse = 1.0f / area;

	/* The pixels it may cover, within the target. */
	min_x = fminf(sx[0], fminf(sx[1], sx[2]));
	max_x = fmaxf(sx[0], fmaxf(sx[1], sx[2]));
	min_y = fminf(sy[0], fminf(sy[1], sy[2]));
	max_y = fmaxf(sy[0], fmaxf(sy[1], sy[2]));
	left = (int)fmaxf(floorf(min_x), 0.0f);
	top = (int)fmaxf(floorf(min_y), 0.0f);
	right = (int)fminf(ceilf(max_x), (float)(target->width - 1));
	bottom = (int)fminf(ceilf(max_y), (float)(target->height - 1));

	/* The color without a texture: its components, premultiplied by its opacity. */
	opacity = (float)((color >> 24) & 0xffU) / 255.0f;
	sample.alpha = opacity;
	sample.red = 0.0f;
	sample.green = 0.0f;
	sample.blue = 0.0f;

	/* Each pixel's centre inside it. */
	for (y = top; y <= bottom; y++) {
		for (x = left; x <= right; x++) {
			/* Its weights; outside when one is below 0. */
			px = (float)x + 0.5f;
			py = (float)y + 0.5f;
			b0 = r3_edge(sx[1], sy[1], sx[2], sy[2], px, py) * inverse;
			b1 = r3_edge(sx[2], sy[2], sx[0], sy[0], px, py) * inverse;
			b2 = 1.0f - b0 - b1;
			if (b0 < 0.0f || b1 < 0.0f || b2 < 0.0f)
				continue;

			/* Behind what is drawn there already. */
			at = (size_t)y * (size_t)target->width + (size_t)x;
			depth = b0 * w[0] + b1 * w[1] + b2 * w[2];
			if (depth <= target->depth[at])
				continue;

			/* Its color: the texture's (perspective-correct), or the triangle's. */
			if (texture != NULL) {
				u = (b0 * corners[0].u * w[0] + b1 * corners[1].u * w[1] + b2 * corners[2].u * w[2]) / depth;
				v = (b0 * corners[0].v * w[0] + b1 * corners[1].v * w[1] + b2 * corners[2].v * w[2]) / depth;
				r3_texture_sample(texture, u, v, &sample);
				sample.alpha *= opacity;
				sample.red *= opacity;
				sample.green *= opacity;
				sample.blue *= opacity;
			} else {
				sample.red = (float)((color >> 16) & 0xffU) / 255.0f * opacity;
				sample.green = (float)((color >> 8) & 0xffU) / 255.0f * opacity;
				sample.blue = (float)(color & 0xffU) / 255.0f * opacity;
			}

			/* The light: the shade on the color, the highlight added as far as it is opaque. */
			sample.red = sample.red * shade + specular * sample.alpha;
			sample.green = sample.green * shade + specular * sample.alpha;
			sample.blue = sample.blue * shade + specular * sample.alpha;

			/* Over what is there. */
			keep = 1.0f - sample.alpha;
			sample.red += (float)((target->color[at] >> 16) & 0xffU) / 255.0f * keep;
			sample.green += (float)((target->color[at] >> 8) & 0xffU) / 255.0f * keep;
			sample.blue += (float)(target->color[at] & 0xffU) / 255.0f * keep;
			sample.alpha += (float)((target->color[at] >> 24) & 0xffU) / 255.0f * keep;
			target->color[at] = r3_pack(&sample);
			sample.alpha = opacity;

			/* Its depth, unless it is seen through. */
			if ((flags & R3_NO_DEPTH_WRITE) == 0U)
				target->depth[at] = depth;
		}
	}
}

/*
 * Averages a target down by a factor into a picture of its size divided
 * by the factor (made by the caller).
 */
void
r3_resolve(
	const struct r3_target *target,
	int factor,
	struct kl_image *image)
{
	struct r3_sample sample;
	uint32_t pixel;
	float weight;
	int x;
	int y;
	int dx;
	int dy;

	/* Each pixel of the picture: the mean of the target's pixels it covers. */
	weight = 1.0f / (float)(factor * factor);
	for (y = 0; y < image->height && (y + 1) * factor <= target->height; y++) {
		for (x = 0; x < image->width && (x + 1) * factor <= target->width; x++) {
			/* The pixels it covers. */
			memset(&sample, 0, sizeof(sample));
			for (dy = 0; dy < factor; dy++) {
				for (dx = 0; dx < factor; dx++) {
					/* One of them. */
					pixel = target->color[(size_t)(y * factor + dy) * (size_t)target->width + (size_t)(x * factor + dx)];
					sample.alpha += (float)((pixel >> 24) & 0xffU) / 255.0f * weight;
					sample.red += (float)((pixel >> 16) & 0xffU) / 255.0f * weight;
					sample.green += (float)((pixel >> 8) & 0xffU) / 255.0f * weight;
					sample.blue += (float)(pixel & 0xffU) / 255.0f * weight;
				}
			}

			/* The mean. */
			image->pixels[(size_t)y * image->stride + (size_t)x] = r3_pack(&sample);
		}
	}
}

/*
 * Makes the matrix that changes nothing.
 */
void
r3_identity(
	struct r3_matrix *matrix)
{
	/* Ones on the diagonal, no move. */
	memset(matrix, 0, sizeof(matrix[0]));
	matrix->m[0][0] = 1.0f;
	matrix->m[1][1] = 1.0f;
	matrix->m[2][2] = 1.0f;
}

/*
 * Makes a rotation about the x axis (positive turns y towards z).
 */
void
r3_rotate_x(
	struct r3_matrix *matrix,
	float radians)
{
	/* The y and z rows. */
	r3_identity(matrix);
	matrix->m[1][1] = cosf(radians);
	matrix->m[1][2] = -sinf(radians);
	matrix->m[2][1] = sinf(radians);
	matrix->m[2][2] = cosf(radians);
}

/*
 * Makes a rotation about the y axis (positive turns z towards x).
 */
void
r3_rotate_y(
	struct r3_matrix *matrix,
	float radians)
{
	/* The x and z rows. */
	r3_identity(matrix);
	matrix->m[0][0] = cosf(radians);
	matrix->m[0][2] = sinf(radians);
	matrix->m[2][0] = -sinf(radians);
	matrix->m[2][2] = cosf(radians);
}

/*
 * Makes a rotation about the z axis (positive turns x towards y).
 */
void
r3_rotate_z(
	struct r3_matrix *matrix,
	float radians)
{
	/* The x and y rows. */
	r3_identity(matrix);
	matrix->m[0][0] = cosf(radians);
	matrix->m[0][1] = -sinf(radians);
	matrix->m[1][0] = sinf(radians);
	matrix->m[1][1] = cosf(radians);
}

/*
 * Makes a move.
 */
void
r3_translate(
	struct r3_matrix *matrix,
	float x,
	float y,
	float z)
{
	/* No rotation, the move. */
	r3_identity(matrix);
	matrix->t.x = x;
	matrix->t.y = y;
	matrix->t.z = z;
}

/*
 * Makes the matrix that does inner and then outer (result may be either).
 */
void
r3_combine(
	const struct r3_matrix *outer,
	const struct r3_matrix *inner,
	struct r3_matrix *result)
{
	struct r3_matrix made;
	int i;
	int j;

	/* The rotations multiplied. */
	for (i = 0; i < 3; i++) {
		for (j = 0; j < 3; j++)
			made.m[i][j] = outer->m[i][0] * inner->m[0][j] + outer->m[i][1] * inner->m[1][j] + outer->m[i][2] * inner->m[2][j];
	}

	/* The inner move turned by the outer rotation, and the outer move. */
	made.t = r3_apply(outer, inner->t);
	*result = made;
}

/*
 * Reports where a matrix takes a point.
 */
struct r3_vec
r3_apply(
	const struct r3_matrix *matrix,
	struct r3_vec point)
{
	struct r3_vec moved;

	/* The rotation, then the move. */
	moved.x = matrix->m[0][0] * point.x + matrix->m[0][1] * point.y + matrix->m[0][2] * point.z + matrix->t.x;
	moved.y = matrix->m[1][0] * point.x + matrix->m[1][1] * point.y + matrix->m[1][2] * point.z + matrix->t.y;
	moved.z = matrix->m[2][0] * point.x + matrix->m[2][1] * point.y + matrix->m[2][2] * point.z + matrix->t.z;

	/* The point moved. */
	return moved;
}

/*
 * Reports the cross product of two directions.
 */
struct r3_vec
r3_cross(
	struct r3_vec a,
	struct r3_vec b)
{
	struct r3_vec product;

	/* Each component. */
	product.x = a.y * b.z - a.z * b.y;
	product.y = a.z * b.x - a.x * b.z;
	product.z = a.x * b.y - a.y * b.x;

	/* The direction across both. */
	return product;
}

/*
 * Reports the dot product of two directions.
 */
float
r3_dot(
	struct r3_vec a,
	struct r3_vec b)
{
	/* The sum of the products. */
	return a.x * b.x + a.y * b.y + a.z * b.z;
}

/*
 * Reports a direction made one long (a zero one as it is).
 */
struct r3_vec
r3_normalize(
	struct r3_vec a)
{
	float length;

	/* A zero direction has no length to divide by. */
	length = sqrtf(a.x * a.x + a.y * a.y + a.z * a.z);
	if (length <= 0.0f)
		return a;

	/* Divided by its length. */
	a.x /= length;
	a.y /= length;
	a.z /= length;
	return a;
}

/*
 * Samples a texture at (u, v) between its four nearest pixels (the edge's
 * pixels outside it).
 */
static void
r3_texture_sample(
	const struct r3_texture *texture,
	float u,
	float v,
	struct r3_sample *sample)
{
	const uint32_t *row0;
	const uint32_t *row1;
	uint32_t pixels[4];
	float weights[4];
	float fx;
	float fy;
	float ax;
	float ay;
	int x0;
	int x1;
	int y0;
	int y1;
	int i;

	/* The pixel to the upper left of the point and how far past it the point is. */
	fx = u * (float)texture->width - 0.5f;
	fy = v * (float)texture->height - 0.5f;
	x0 = (int)floorf(fx);
	y0 = (int)floorf(fy);
	ax = fx - (float)x0;
	ay = fy - (float)y0;

	/* The four pixels' columns and rows, within the texture. */
	x1 = x0 + 1;
	y1 = y0 + 1;
	x0 = r3_clamp(x0, texture->width - 1);
	x1 = r3_clamp(x1, texture->width - 1);
	y0 = r3_clamp(y0, texture->height - 1);
	y1 = r3_clamp(y1, texture->height - 1);

	/* The four and their weights. */
	row0 = texture->pixels + (size_t)y0 * texture->stride;
	row1 = texture->pixels + (size_t)y1 * texture->stride;
	pixels[0] = row0[x0];
	pixels[1] = row0[x1];
	pixels[2] = row1[x0];
	pixels[3] = row1[x1];
	weights[0] = (1.0f - ax) * (1.0f - ay) / 255.0f;
	weights[1] = ax * (1.0f - ay) / 255.0f;
	weights[2] = (1.0f - ax) * ay / 255.0f;
	weights[3] = ax * ay / 255.0f;

	/* Their weighted sum. */
	memset(sample, 0, sizeof(sample[0]));
	for (i = 0; i < 4; i++) {
		sample->alpha += (float)((pixels[i] >> 24) & 0xffU) * weights[i];
		sample->red += (float)((pixels[i] >> 16) & 0xffU) * weights[i];
		sample->green += (float)((pixels[i] >> 8) & 0xffU) * weights[i];
		sample->blue += (float)(pixels[i] & 0xffU) * weights[i];
	}
}

/*
 * Reports a coordinate within 0 and a last one.
 */
static int
r3_clamp(
	int value,
	int last)
{
	/* Below the first. */
	if (value < 0)
		return 0;

	/* Past the last. */
	if (value > last)
		return last;

	/* Within. */
	return value;
}

/*
 * Reports which side of the line from a to b a point is on, and twice the
 * area of the triangle the three make (signed).
 */
static float
r3_edge(
	float ax,
	float ay,
	float bx,
	float by,
	float px,
	float py)
{
	/* The cross product of the two edges from a. */
	return (bx - ax) * (py - ay) - (by - ay) * (px - ax);
}

/*
 * Packs a sample into a premultiplied 0xAARRGGBB pixel, each component
 * within 0..1 and the color within the alpha.
 */
static uint32_t
r3_pack(
	const struct r3_sample *sample)
{
	float component[4];
	uint32_t pixel;
	int i;

	/* The alpha first, then each color no more than it. */
	component[0] = fminf(fmaxf(sample->alpha, 0.0f), 1.0f);
	component[1] = fminf(fmaxf(sample->red, 0.0f), component[0]);
	component[2] = fminf(fmaxf(sample->green, 0.0f), component[0]);
	component[3] = fminf(fmaxf(sample->blue, 0.0f), component[0]);

	/* The bytes. */
	pixel = 0;
	for (i = 0; i < 4; i++)
		pixel = (pixel << 8) | (uint32_t)(component[i] * 255.0f + 0.5f);

	/* The pixel. */
	return pixel;
}
