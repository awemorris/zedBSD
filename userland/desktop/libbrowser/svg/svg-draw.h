/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Drawing a picture: its commands turned, for one placement on the
 * device, into filled polylines with their paints and clips, handed to a
 * sink.  The sink decides what to do with them: the CPU bitmap
 * (svg-bitmap.h) rasterizes and composites them, the page's display list
 * keeps them as path items for the renderers.
 */

#ifndef KEILAND_BROWSER_SVG_DRAW_H
#define KEILAND_BROWSER_SVG_DRAW_H

#include "svg/svg-picture.h"
#include "vector/vec-raster.h"
#include "vector/vec-stroke.h"

/*
 * One part of a clip as the sink sees it: device-space polylines and
 * their rule.  A clip is the union of its parts (vec_coverage_unite of
 * their coverages), and a clip with a parent is met with it
 * (vec_coverage_intersect).
 */
struct svg_draw_clip_part {
	const struct vec_lines *lines;
	enum vec_fill_rule rule;
};

/*
 * A clip as the sink sees it: the index of the picture's clip (so a sink
 * may keep its coverage across the commands that share it), its parts,
 * and its parent's index (-1 for none; the parent was described to the
 * sink by an earlier clip call).
 */
struct svg_draw_clip {
	int index;
	const struct svg_draw_clip_part *parts;
	size_t part_count;
	int parent;
};

/*
 * A paint as the sink sees it: kind (svg_server_kind, never NONE: a
 * command with nothing to paint is not passed), its straight color for
 * COLOR, or for GRADIENT the gradient whose to_gradient maps device
 * pixels to gradient space (the gradient is valid only during the call,
 * its stops as long as the picture); alpha the
 * factor its alpha is multiplied by (the command's fill-opacity or
 * stroke-opacity).
 */
struct svg_draw_paint {
	enum svg_server_kind kind;
	uint32_t color;
	const struct vec_gradient *gradient;
	float alpha;
};

/*
 * A sink: what receives the drawing.  Each function returns 0 to go on or
 * an errno value that ends the drawing (and that svg_picture_draw then
 * returns).
 *
 *   clip(context, clip)      describes a clip before the first fill that
 *                            uses it; each clip of the picture is
 *                            described at most once per drawing, its
 *                            parent first.  The parts' polylines are valid
 *                            only during the call.
 *   fill(context, lines, rule, paint, clip)
 *                            fills device-space polylines (each contour
 *                            closed) with the rule and the paint, clipped
 *                            by the clip of that index (-1 for none).  The
 *                            polylines are valid only during the call.
 *   group(context, opacity)  starts a group composited with opacity when
 *                            it ends.
 *   group_end(context)       ends the innermost group.
 */
struct svg_sink {
	void *context;
	int (*clip)(void *context, const struct svg_draw_clip *clip);
	int (*fill)(void *context, const struct vec_lines *lines, enum vec_fill_rule rule, const struct svg_draw_paint *paint, int clip);
	int (*group)(void *context, float opacity);
	int (*group_end)(void *context);
};

/*
 * Draws a picture through a sink.
 *
 * For each command in order, with M = placement x command matrix (the
 * command's user space to device pixels):
 *   FILL    the path flattened by vec_flatten with M and tolerance, passed
 *           to fill with the command's rule.
 *   STROKE  the path flattened in user space (vec_flatten with no matrix
 *           and the tolerance max(tolerance / vec_matrix_scale_max(M),
 *           VEC_TOLERANCE_MIN)), stroked by vec_stroke with the command's
 *           style, tolerance as given and to_device M, passed to fill with
 *           VEC_FILL_NONZERO.  A command whose M has a scale of 0 draws
 *           nothing.
 *   GROUP, GROUP_END  passed to group and group_end.
 * A command's paint goes to the sink with its gradient's to_gradient
 * composed with the inverse of M (so it maps device pixels), and its alpha
 * the command's opacity.  A gradient whose M cannot be inverted paints
 * nothing (the command is skipped).  Its clip, when it has one, is
 * described first (svg_draw_clip with the clip parts flattened with their
 * matrices composed with placement), parents before children.
 *
 * Input: picture built; placement maps the picture's space to device
 * pixels (valid); tolerance >= VEC_TOLERANCE_MIN, in device pixels; sink
 * with all four functions.
 * Errors: EINVAL for a placement that is not valid or a tolerance out of
 * range; ENOMEM, ENOBUFS from flattening and stroking (ENOBUFS, and
 * EINVAL from a command whose mapped points leave the coordinate range,
 * skip that command and the drawing goes on, returning 0 if nothing else
 * fails; ENOMEM ends the drawing); a sink's error, which ends the
 * drawing.  Groups started are ended (group_end is called for them)
 * before an error is returned, except for a sink's own error.
 */
int svg_picture_draw(const struct svg_picture *picture, const struct vec_matrix *placement, float tolerance, const struct svg_sink *sink);

#endif
