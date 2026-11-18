/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The mark, as in zedbsd-boot-splash.png and
 * plan/ws035/kei-identity-design.md: a tall bar with rounded corners (the
 * lower left one wide) and a leaf, a lens leaning from the bar's foot to
 * the upper right, over it.  Each layer is a square of coverage (0..255)
 * sampled sixteen times a pixel; the programs tint them (the bar pale blue,
 * the leaf deeper, the shades deeper still where they are strong).
 *
 * The panes are frosted glass (ws035-p109): the part the two share is a
 * layer of its own, deepest towards the leaf's lower point, so the overlap
 * darkens as two sheets of tinted glass do; a soft band of light runs just
 * inside every edge; and a sheen falls over the upper part of the bar and
 * the tip of the leaf, as the light does on the splash's glass.
 *
 * The mark is measured in units of its own height, centred in the square
 * with a small margin.
 */

#include "mark.h"

#include <math.h>

/* The mark's width in units of its height, and how much of the square its height fills. */
#define MARK_WIDTH		1.00f
#define MARK_FILL		0.94f

/* The bar: its right edge and its foot, and the radii of its corners (top left, top right, bottom left, bottom right). */
#define MARK_BAR_RIGHT		0.47f
#define MARK_BAR_FOOT		0.98f
#define MARK_BAR_TOP_LEFT	0.10f
#define MARK_BAR_TOP_RIGHT	0.14f
#define MARK_BAR_BOTTOM_LEFT	0.24f
#define MARK_BAR_BOTTOM_RIGHT	0.06f

/*
 * The leaf: the part two circles share.  The upper side is an arc that
 * rises steeply from the lower point and runs almost level to the tip; the
 * lower side a rounder arc along the foot and up the right to the tip.  The
 * circles were fitted to points of the two sides in the splash.
 */
#define MARK_LEAF_UPPER_X	0.769f
#define MARK_LEAF_UPPER_Y	0.997f
#define MARK_LEAF_UPPER_RADIUS	0.602f
#define MARK_LEAF_LOWER_X	0.474f
#define MARK_LEAF_LOWER_Y	0.488f
#define MARK_LEAF_LOWER_RADIUS	0.538f

/* The leaf's lower point and its tip, which its shade runs between. */
#define MARK_LEAF_FROM_X	0.17f
#define MARK_LEAF_FROM_Y	0.97f
#define MARK_LEAF_TO_X		0.97f
#define MARK_LEAF_TO_Y		0.42f

/* How far in from an edge its light reaches, in units of the mark's height. */
#define MARK_RIM_WIDTH		0.045f

/* How far down the bar its sheen reaches, in units of the mark's height. */
#define MARK_SHEEN_DEPTH	0.62f

/* How many samples a pixel has along each side. */
#define MARK_SAMPLES		4

/*
 * The leaf as the part two circles share: the circle of its upper side and
 * that of its lower side, their centres and radii.
 */
struct mark_leaf {
	float centre_a[2];
	float centre_b[2];
	float radius_a;
	float radius_b;
};

static void mark_leaf_circles(struct mark_leaf *leaf);
static float mark_bar_depth(float u, float v);
static float mark_corner_depth(float u, float v, float cx, float cy, float radius);
static float mark_circle_depth(float dx, float dy, float radius);
static float mark_leaf_depth(const struct mark_leaf *leaf, float u, float v);
static float mark_leaf_along(float u, float v);
static float mark_rim(float depth);
static float mark_sheen(float bar, float leaf_depth, float u, float v);
static float mark_sample(const struct mark_leaf *leaf, unsigned layer, float u, float v);

/*
 * Renders one layer of the mark into a square of coverage pixels a side
 * (rows stride bytes apart).
 */
void
kl_mark_raster(
	unsigned layer,
	unsigned pixels,
	uint8_t *coverage,
	size_t stride)
{
	struct mark_leaf leaf;
	float scale;
	float left;
	float top;
	float u;
	float v;
	float sum;
	unsigned x;
	unsigned y;
	unsigned sx;
	unsigned sy;

	/* The mark's unit and where it starts in the square. */
	mark_leaf_circles(&leaf);
	scale = (float)pixels * MARK_FILL;
	left = ((float)pixels - MARK_WIDTH * scale) / 2.0f;
	top = ((float)pixels - scale) / 2.0f;

	/* Each pixel: the layer's weight summed over its samples. */
	for (y = 0; y < pixels; y++) {
		for (x = 0; x < pixels; x++) {
			sum = 0.0f;
			for (sy = 0; sy < MARK_SAMPLES; sy++) {
				for (sx = 0; sx < MARK_SAMPLES; sx++) {
					u = ((float)x + ((float)sx + 0.5f) / MARK_SAMPLES - left) / scale;
					v = ((float)y + ((float)sy + 0.5f) / MARK_SAMPLES - top) / scale;
					sum += mark_sample(&leaf, layer, u, v);
				}
			}

			/* The pixel's coverage. */
			coverage[(size_t)y * stride + x] = (uint8_t)(sum * 255.0f / (MARK_SAMPLES * MARK_SAMPLES) + 0.5f);
		}
	}
}

/* Fills in the two circles whose shared part is the leaf. */
static void
mark_leaf_circles(
	struct mark_leaf *leaf)
{
	/* The upper side's circle. */
	leaf->centre_a[0] = MARK_LEAF_UPPER_X;
	leaf->centre_a[1] = MARK_LEAF_UPPER_Y;
	leaf->radius_a = MARK_LEAF_UPPER_RADIUS;

	/* The lower side's circle. */
	leaf->centre_b[0] = MARK_LEAF_LOWER_X;
	leaf->centre_b[1] = MARK_LEAF_LOWER_Y;
	leaf->radius_b = MARK_LEAF_LOWER_RADIUS;
}

/*
 * Reports how far a point is inside the bar with its four rounded corners:
 * the distance to its nearest edge, negative outside.
 */
static float
mark_bar_depth(
	float u,
	float v)
{
	float depth;
	float corner;

	/* The nearest of the four straight edges. */
	depth = u;
	if (MARK_BAR_RIGHT - u < depth)
		depth = MARK_BAR_RIGHT - u;
	if (v < depth)
		depth = v;
	if (MARK_BAR_FOOT - v < depth)
		depth = MARK_BAR_FOOT - v;

	/* Outside the box, the straight edges tell. */
	if (depth < 0.0f)
		return depth;

	/* In a corner's square the rounding is the edge. */
	corner = depth;
	if (u < MARK_BAR_TOP_LEFT && v < MARK_BAR_TOP_LEFT) {
		/* The top left corner. */
		corner = mark_corner_depth(u, v, MARK_BAR_TOP_LEFT, MARK_BAR_TOP_LEFT, MARK_BAR_TOP_LEFT);
	} else if (u > MARK_BAR_RIGHT - MARK_BAR_TOP_RIGHT && v < MARK_BAR_TOP_RIGHT) {
		/* The top right corner. */
		corner = mark_corner_depth(u, v, MARK_BAR_RIGHT - MARK_BAR_TOP_RIGHT, MARK_BAR_TOP_RIGHT, MARK_BAR_TOP_RIGHT);
	} else if (u < MARK_BAR_BOTTOM_LEFT && v > MARK_BAR_FOOT - MARK_BAR_BOTTOM_LEFT) {
		/* The wide bottom left corner. */
		corner = mark_corner_depth(u, v, MARK_BAR_BOTTOM_LEFT, MARK_BAR_FOOT - MARK_BAR_BOTTOM_LEFT, MARK_BAR_BOTTOM_LEFT);
	} else if (u > MARK_BAR_RIGHT - MARK_BAR_BOTTOM_RIGHT && v > MARK_BAR_FOOT - MARK_BAR_BOTTOM_RIGHT) {
		/* The bottom right corner. */
		corner = mark_corner_depth(u, v, MARK_BAR_RIGHT - MARK_BAR_BOTTOM_RIGHT, MARK_BAR_FOOT - MARK_BAR_BOTTOM_RIGHT, MARK_BAR_BOTTOM_RIGHT);
	}

	/* The nearer of the rounding and the straight edges. */
	if (corner < depth)
		depth = corner;

	/* Reports the depth in the bar. */
	return depth;
}

/* Reports how far a point is inside a corner's circle, negative outside. */
static float
mark_corner_depth(
	float u,
	float v,
	float cx,
	float cy,
	float radius)
{
	float dx;
	float dy;
	float depth;

	/* The depth under the rounding, from the offset to the corner's centre. */
	dx = u - cx;
	dy = v - cy;
	depth = mark_circle_depth(dx, dy, radius);

	/* Reports the depth under the rounding. */
	return depth;
}

/*
 * Reports how far a point at an offset from a circle's centre is inside
 * the circle, negative outside, as far as the layers need it.
 *
 * The layers read a depth only for its sign and, within MARK_RIM_WIDTH of
 * the edge, for the edge's light.  Outside the circle the depth reported is
 * -1, and deeper in than the rim's band it is the radius (at least the true
 * depth's band); only a point in the band takes the square root, which is
 * slow in the C library (ws035-p129: the compositor spent 1.4 s of its start
 * on the mark's square roots).
 */
static float
mark_circle_depth(
	float dx,
	float dy,
	float radius)
{
	float squared;
	float inner;
	float distance;

	/* Outside the circle only the sign matters. */
	squared = dx * dx + dy * dy;
	if (squared >= radius * radius)
		return -1.0f;

	/* Deeper than the rim's band only that matters. */
	inner = radius - MARK_RIM_WIDTH;
	if (inner > 0.0f && squared < inner * inner)
		return radius;

	/* In the band, the true depth. */
	distance = sqrtf(squared);

	/* Reports the depth under the edge. */
	return radius - distance;
}

/*
 * Reports how far a point is inside the leaf, the part its two circles
 * share: the lesser of its depths in the two, negative outside.
 */
static float
mark_leaf_depth(
	const struct mark_leaf *leaf,
	float u,
	float v)
{
	float dx;
	float dy;
	float depth_a;
	float depth_b;

	/* The depth in the upper side's circle. */
	dx = u - leaf->centre_a[0];
	dy = v - leaf->centre_a[1];
	depth_a = mark_circle_depth(dx, dy, leaf->radius_a);

	/* The depth in the lower side's circle. */
	dx = u - leaf->centre_b[0];
	dy = v - leaf->centre_b[1];
	depth_b = mark_circle_depth(dx, dy, leaf->radius_b);

	/* The lower side's circle is the shallower one here. */
	if (depth_b < depth_a)
		return depth_b;

	/* Reports the depth in the upper side's circle, the shallower one. */
	return depth_a;
}

/* Reports how far along the leaf a point is, from its lower point (0) to its tip (1). */
static float
mark_leaf_along(
	float u,
	float v)
{
	float dx;
	float dy;
	float along;

	/* The point's share of the way from the lower point to the tip. */
	dx = MARK_LEAF_TO_X - MARK_LEAF_FROM_X;
	dy = MARK_LEAF_TO_Y - MARK_LEAF_FROM_Y;
	along = ((u - MARK_LEAF_FROM_X) * dx + (v - MARK_LEAF_FROM_Y) * dy) / (dx * dx + dy * dy);

	/* Points past either end count as the end. */
	if (along < 0.0f)
		along = 0.0f;
	if (along > 1.0f)
		along = 1.0f;

	/* Reports the share. */
	return along;
}

/*
 * Reports how strong the light along an edge is at a depth inside a pane:
 * full at the edge, gone at the rim's width.
 */
static float
mark_rim(
	float depth)
{
	float fade;

	/* Outside the pane, or past the band, there is none. */
	if (depth < 0.0f || depth > MARK_RIM_WIDTH)
		return 0.0f;

	/* The light falls off softly from the edge inwards. */
	fade = 1.0f - depth / MARK_RIM_WIDTH;

	/* Reports the strength. */
	return fade * fade;
}

/*
 * Reports the sheen at a point, from the depths in the two panes: over the
 * bar's upper part, fading downwards, and over the leaf towards its tip.
 */
static float
mark_sheen(
	float bar,
	float leaf_depth,
	float u,
	float v)
{
	float light;
	float along;
	float tip;

	/* The bar's upper part. */
	light = 0.0f;
	if (bar >= 0.0f && v < MARK_SHEEN_DEPTH) {
		light = 1.0f - v / MARK_SHEEN_DEPTH;
		light = light * light;
	}

	/* The leaf towards its tip, where the splash's glass is palest. */
	if (leaf_depth >= 0.0f) {
		along = mark_leaf_along(u, v);
		tip = along * along * 0.8f;
		if (tip > light)
			light = tip;
	}

	/* Reports the stronger of the two. */
	return light;
}

/* Reports how much a layer weighs at one sample point (0 outside its shape). */
static float
mark_sample(
	const struct mark_leaf *leaf,
	unsigned layer,
	float u,
	float v)
{
	float bar;
	float leaf_depth;
	float along;
	float light;
	float other;

	/* How deep the point is in each pane. */
	bar = mark_bar_depth(u, v);
	leaf_depth = mark_leaf_depth(leaf, u, v);

	/* Weighs the point by the layer it is sampled for. */
	switch (layer) {
	case KL_MARK_BAR:
		/* The bar is even. */
		if (bar < 0.0f)
			return 0.0f;
		return 1.0f;
	case KL_MARK_BAR_SHADE:
		/* The bar's shade deepens towards its foot. */
		if (bar < 0.0f)
			return 0.0f;
		return v / MARK_BAR_FOOT;
	case KL_MARK_LEAF:
		/* The leaf is even. */
		if (leaf_depth < 0.0f)
			return 0.0f;
		return 1.0f;
	case KL_MARK_LEAF_SHADE:
		/* The leaf's shade gathers towards its lower point. */
		if (leaf_depth < 0.0f)
			return 0.0f;
		along = 1.0f - mark_leaf_along(u, v);
		return along * along;
	case KL_MARK_OVERLAP:
		/* Where both panes lie, deepest towards the leaf's lower point. */
		if (bar < 0.0f || leaf_depth < 0.0f)
			return 0.0f;
		along = mark_leaf_along(u, v);
		return 0.55f + 0.45f * (1.0f - along);
	case KL_MARK_RIM:
		/* The brighter of the two panes' edge lights. */
		light = mark_rim(bar);
		other = mark_rim(leaf_depth);
		if (other > light)
			light = other;
		return light;
	case KL_MARK_SHEEN:
		/* The light over the upper part of the glass. */
		light = mark_sheen(bar, leaf_depth, u, v);
		return light;
	default:
		break;
	}

	/* A layer the mark does not have weighs nothing. */
	return 0.0f;
}
