/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The rules of the screen edges' gestures (WS181; edge.h says what they
 * are).  Each answers one question the shell or Home asks at one moment:
 * where a press is, what a press held in the top band has become, what a
 * drag on Home is, how far a point is from another, how deep the desktop
 * and Home's content are on Home's way in and out, and where a group of
 * fingers swipes in from (BUG-267).
 */

#include "edge.h"

static void depth_about_middle(float scale, int32_t width, int32_t height, struct kwl_edge_depth *depth);

/*
 * Tells where a press is for the edges' gestures: the bottom edge's strip
 * (the swipe up to App Home; any pointer), the top edge's band (the swipe
 * down to Wiseview; a touch only, the 2026-10-07 user decision, and not
 * over the launcher or the top-right corner), or nowhere special.
 */
unsigned
kwl_edge_classify(
	int32_t x,
	int32_t y,
	int32_t width,
	int32_t height,
	int touch)
{
	/* The bottom edge's strip. */
	if (y >= height - KWL_EDGE_BOTTOM_HEIGHT)
		return KWL_EDGE_BOTTOM_STRIP;

	/* The band is a touch's only. */
	if (!touch)
		return KWL_EDGE_NONE;

	/* Below the band. */
	if (y >= KWL_EDGE_BAND)
		return KWL_EDGE_NONE;

	/* Over the launcher (and App Home's top-left corner) or the top-right corner of Notes. */
	if (x < KWL_EDGE_LAUNCHER_WIDTH)
		return KWL_EDGE_NONE;
	if (x >= width - KWL_EDGE_CORNER)
		return KWL_EDGE_NONE;

	/* Succeeded: the top edge's band. */
	return KWL_EDGE_TOP_BAND;
}

/*
 * Tells what a press held in the top band is after it moved by (dx, dy):
 * down by KWL_EDGE_BAND_START and more down than across, Wiseview's swipe;
 * across or up by more than KWL_EDGE_BAND_SLIP, the press of what is under
 * it; otherwise still waiting.
 */
unsigned
kwl_edge_band_motion(
	int32_t dx,
	int32_t dy)
{
	int32_t across;

	/* How far across, whichever way. */
	across = dx;
	if (across < 0)
		across = -across;

	/* Down far enough, and more down than across: the swipe. */
	if (dy >= KWL_EDGE_BAND_START && dy > across)
		return KWL_EDGE_BAND_WISEVIEW;

	/* Across or up too far: not the swipe. */
	if (across > KWL_EDGE_BAND_SLIP)
		return KWL_EDGE_BAND_REPLAY;
	if (dy < -KWL_EDGE_BAND_SLIP)
		return KWL_EDGE_BAND_REPLAY;

	/* Succeeded: still waiting to know. */
	return KWL_EDGE_BAND_WAIT;
}

/*
 * Tells what a drag on Home that went (dx, dy) is: sideways at least as
 * far as up or down, the pages; more down, closing Home; more up, nothing.
 */
unsigned
kwl_edge_drag_axis(
	int32_t dx,
	int32_t dy)
{
	int32_t across;

	/* How far across, whichever way. */
	across = dx;
	if (across < 0)
		across = -across;

	/* Sideways at least as far as down or up: the pages. */
	if (across >= dy && across >= -dy)
		return KWL_EDGE_DRAG_PAGES;

	/* Down: closing Home. */
	if (dy > 0)
		return KWL_EDGE_DRAG_CLOSE;

	/* Succeeded: up, which does nothing. */
	return KWL_EDGE_DRAG_NONE;
}

/*
 * Gives the distance of (dx, dy) from the origin in whole pixels (rounded
 * down), without floating point: how far a docked title has been pulled.
 */
int32_t
kwl_edge_distance(
	int32_t dx,
	int32_t dy)
{
	int64_t square;
	int64_t root;
	int64_t bit;

	/* The square of the distance. */
	square = (int64_t)dx * dx + (int64_t)dy * dy;

	/* Its integer square root, one bit at a time from the highest (a screen is far less than 2^21 pixels across). */
	root = 0;
	for (bit = (int64_t)1 << 21; bit != 0; bit >>= 1) {
		if ((root + bit) * (root + bit) <= square)
			root += bit;
	}

	/* Succeeded: the distance. */
	return (int32_t)root;
}

/*
 * Works out where the desktop layer is when App Home is open by progress
 * (0 closed, 1 open): gone back into the distance about the output's
 * middle, and faded out on the latter part of the way (ws181-p008).
 */
void
kwl_edge_home_desktop(
	float progress,
	int32_t width,
	int32_t height,
	struct kwl_edge_depth *depth)
{
	float scale;
	float opacity;

	/* A progress outside the way is at its ends. */
	if (progress < 0.0f)
		progress = 0.0f;
	if (progress > 1.0f)
		progress = 1.0f;

	/* Smaller the further Home has opened, about the middle. */
	scale = 1.0f - (1.0f - KWL_EDGE_HOME_DESKTOP_DEPTH) * progress;
	depth_about_middle(scale, width, height, depth);

	/* Whole until the fade starts, gone once it ends, evenly between. */
	if (progress <= KWL_EDGE_HOME_FADE_START) {
		opacity = 1.0f;
	} else if (progress >= KWL_EDGE_HOME_FADE_END) {
		opacity = 0.0f;
	} else {
		opacity = (KWL_EDGE_HOME_FADE_END - progress) / (KWL_EDGE_HOME_FADE_END - KWL_EDGE_HOME_FADE_START);
	}

	/* Succeeded: the layer's place and opacity. */
	depth->opacity = opacity;
}

/*
 * Works out where Home's content (its clock, icons, floors and dots) is
 * when Home is open by progress: come forward from the distance about the
 * output's middle, its own size once open.  The content fades in by its
 * own timing (home.c), so its opacity here is whole.
 */
void
kwl_edge_home_content(
	float progress,
	int32_t width,
	int32_t height,
	struct kwl_edge_depth *depth)
{
	float scale;

	/* A progress outside the way is at its ends. */
	if (progress < 0.0f)
		progress = 0.0f;
	if (progress > 1.0f)
		progress = 1.0f;

	/* Larger the further Home has opened, about the middle. */
	scale = KWL_EDGE_HOME_CONTENT_DEPTH + (1.0f - KWL_EDGE_HOME_CONTENT_DEPTH) * progress;
	depth_about_middle(scale, width, height, depth);

	/* Succeeded: whole, its fading being the content's own. */
	depth->opacity = 1.0f;
}

/*
 * Tells which edge a finger touching at (x, y) may swipe in from with
 * other fingers (BUG-267): the nearest of the left side, the right side
 * (both only under the system bar, whose height is top) and the bottom,
 * when it is within KWL_EDGE_GROUP_REACH; otherwise none.
 */
unsigned
kwl_edge_group_side(
	int32_t x,
	int32_t y,
	int32_t width,
	int32_t height,
	int32_t top)
{
	unsigned side;
	unsigned nearest;
	int32_t distance;
	int32_t shortest;

	/* Each edge in turn (the left side, the right side, the bottom): a nearer one wins. */
	nearest = KWL_EDGE_SIDE_NONE;
	shortest = KWL_EDGE_GROUP_REACH + 1;
	for (side = KWL_EDGE_SIDE_LEFT; side <= KWL_EDGE_SIDE_BOTTOM; side++) {
		/* How far the finger is from this edge; -1 when it is not beside it. */
		distance = kwl_edge_group_distance(side, x, y, width, height, top);
		if (distance < 0)
			continue;

		/* A nearer edge than any before. */
		if (distance < shortest) {
			nearest = side;
			shortest = distance;
		}
	}

	/* Succeeded: the nearest edge within reach, or none. */
	return nearest;
}

/*
 * Tells how far a point is from one edge (a KWL_EDGE_SIDE_*), in pixels:
 * 0 on the edge's own row or column.  A point above the system bar (top)
 * is beside neither side, a point off the output is beside no edge, and
 * KWL_EDGE_SIDE_NONE is no edge: those give -1.
 */
int32_t
kwl_edge_group_distance(
	unsigned side,
	int32_t x,
	int32_t y,
	int32_t width,
	int32_t height,
	int32_t top)
{
	int32_t distance;

	/* The distance from the edge's outermost row or column. */
	switch (side) {
	case KWL_EDGE_SIDE_LEFT:
		distance = x;
		break;
	case KWL_EDGE_SIDE_RIGHT:
		distance = width - 1 - x;
		break;
	case KWL_EDGE_SIDE_BOTTOM:
		distance = height - 1 - y;
		break;
	default:
		return -1;
	}

	/* The sides start under the system bar (the desktops' swipe starts there). */
	if (side != KWL_EDGE_SIDE_BOTTOM && y < top)
		return -1;

	/* A point off the output is not beside the edge. */
	if (distance < 0)
		return -1;

	/* Succeeded: the distance. */
	return distance;
}

/*
 * Tells what a finger of a group swiping in from an edge is after it moved
 * by (dx, dy) from where it touched: in by KWL_EDGE_GROUP_START and more in
 * than across, going in; out, or along the edge, that far without going
 * in, something else (a pinch, a scroll); otherwise still waiting.
 */
unsigned
kwl_edge_group_motion(
	unsigned side,
	int32_t dx,
	int32_t dy)
{
	int32_t inward;
	int32_t across;

	/* How far in from the edge, and how far along it. */
	switch (side) {
	case KWL_EDGE_SIDE_LEFT:
		inward = dx;
		across = dy;
		break;
	case KWL_EDGE_SIDE_RIGHT:
		inward = -dx;
		across = dy;
		break;
	case KWL_EDGE_SIDE_BOTTOM:
		inward = -dy;
		across = dx;
		break;
	default:
		return KWL_EDGE_GROUP_OTHER;
	}

	/* Along the edge whichever way. */
	if (across < 0)
		across = -across;

	/* In far enough, and more in than across: the swipe's way. */
	if (inward >= KWL_EDGE_GROUP_START && inward > across)
		return KWL_EDGE_GROUP_IN;

	/* Out, or along the edge, far enough: something else. */
	if (inward <= -KWL_EDGE_GROUP_START)
		return KWL_EDGE_GROUP_OTHER;
	if (across >= KWL_EDGE_GROUP_START)
		return KWL_EDGE_GROUP_OTHER;

	/* Succeeded: not far enough to tell yet. */
	return KWL_EDGE_GROUP_WAIT;
}

/*
 * Gives the point on an edge where a group's swipe starts, for the finger
 * nearest the edge touching at (x, y): the same row or column, on the
 * edge's outermost pixel, where a single finger's swipe would start.
 */
void
kwl_edge_group_point(
	unsigned side,
	int32_t x,
	int32_t y,
	int32_t width,
	int32_t height,
	int32_t *edge_x,
	int32_t *edge_y)
{
	/* The finger's own point, unless the edge moves it. */
	*edge_x = x;
	*edge_y = y;

	/* Onto the edge. */
	switch (side) {
	case KWL_EDGE_SIDE_LEFT:
		*edge_x = 0;
		break;
	case KWL_EDGE_SIDE_RIGHT:
		*edge_x = width - 1;
		break;
	case KWL_EDGE_SIDE_BOTTOM:
		*edge_y = height - 1;
		break;
	default:
		break;
	}
}

/* Places a layer of the output's size scaled about the output's middle. */
static void
depth_about_middle(
	float scale,
	int32_t width,
	int32_t height,
	struct kwl_edge_depth *depth)
{
	/* The middle stays where it is: the corner moves in by half of what the layer lost. */
	depth->scale = scale;
	depth->x = (float)width * (1.0f - scale) * 0.5f;
	depth->y = (float)height * (1.0f - scale) * 0.5f;
}
