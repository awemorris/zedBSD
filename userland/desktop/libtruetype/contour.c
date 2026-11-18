/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * A glyph's outline as the font stores it, for callers that draw curves
 * themselves.
 *
 * The bitmap path in outline.c turns curves into pixel-sized segments.  A
 * caller that writes vector output -- a PDF page, a scalable drawing -- wants
 * the contours unchanged: the points in design units with their on-curve and
 * control flags, which are TrueType's quadratic curves.  This file reads them
 * out of glyf into arrays the caller supplies.
 *
 * A composite glyph is flattened here.  Each component's points are
 * appended to the same arrays and moved by the component's offset and its
 * scale or matrix, so a caller sees one simple outline whatever the font did.
 */

#include "internal.h"

#include <errno.h>
#include <math.h>

/* The flags each point of a simple glyph carries. */
#define CONTOUR_ON_CURVE	0x01U
#define CONTOUR_X_SHORT		0x02U
#define CONTOUR_Y_SHORT		0x04U
#define CONTOUR_REPEAT		0x08U
#define CONTOUR_X_SAME		0x10U
#define CONTOUR_Y_SAME		0x20U

/* The flags each component of a composite glyph carries. */
#define COMPONENT_ARG_WORDS		0x0001U
#define COMPONENT_ARGS_XY		0x0002U
#define COMPONENT_HAVE_SCALE		0x0008U
#define COMPONENT_MORE			0x0020U
#define COMPONENT_HAVE_XY_SCALE		0x0040U
#define COMPONENT_HAVE_MATRIX		0x0080U
#define COMPONENT_SCALED_OFFSET		0x0800U
#define COMPONENT_UNSCALED_OFFSET	0x1000U

/*
 * The most points one flattened glyph may have.
 *
 * A composite can name the same large component many times over and nest
 * that eight deep, so without a bound a small font could ask for an
 * outline of billions of points.  No real glyph comes near this.
 */
#define CONTOUR_POINTS_MAX	65536U

/* The most components one flattened glyph may visit, for the same reason. */
#define CONTOUR_COMPONENTS_MAX	1024U

/*
 * One reading of a glyph's outline in progress.
 *
 * It lives on the stack of truetype_glyph_outline() for one call.  overflow
 * is set once a glyph or a component did not fit the caller's arrays; from
 * then on points are only counted, so the counts still tell the caller how
 * much to allocate, and nothing is moved or written.
 */
struct contour_walk {
	const struct truetype_face *face;
	struct truetype_glyph_outline *outline;
	unsigned components;
	unsigned overflow;
};

/*
 * How one component is placed inside a composite glyph.
 *
 * A point (x, y) of the component lands at (xx * x + yx * y + dx,
 * xy * x + yy * y + dy), which is how the glyf table's 2x2 matrix is
 * applied.
 */
struct contour_transform {
	float xx;
	float xy;
	float yx;
	float yy;
	float dx;
	float dy;
};

static int contour_outline(const struct truetype_face *face, unsigned glyph, struct truetype_glyph_outline *outline);
static void contour_scale(struct truetype_glyph_outline *outline, unsigned from, unsigned to);
static int contour_load_glyph(struct contour_walk *walk, unsigned glyph, unsigned depth);
static int contour_load_simple(struct contour_walk *walk, const uint8_t *entry, uint32_t length);
static int contour_load_composite(struct contour_walk *walk, const uint8_t *entry, uint32_t length, unsigned depth);
static int contour_read_flags(struct truetype_outline_point *points, unsigned count, const uint8_t *entry, uint32_t length, uint32_t *position);
static int contour_read_coordinates(struct truetype_outline_point *points, unsigned count, const uint8_t *entry, uint32_t length, uint32_t *position, unsigned vertical);
static int contour_read_side_bearing(const struct truetype_face *face, unsigned glyph, int *bearing);
static float contour_f2dot14(const uint8_t *bytes);

/*
 * Reads one glyph's contours out of glyf, in design units.
 *
 * The arrays and their capacities come from the caller.  When they are too
 * small, ENOSPC is reported and the counts say how many are needed.  A
 * glyph of a companion (companion.c) is read from it and scaled to the
 * face's em.
 */
int
truetype_glyph_outline(
	const struct truetype_face *face,
	unsigned glyph,
	struct truetype_glyph_outline *outline)
{
	const struct truetype_face *drawn;
	int error;

	/* Refuses a call without a face. */
	if (face == NULL)
		return EINVAL;

	/* The face among its companions that draws the glyph, and the outline in its units. */
	drawn = truetype_resolve_const(face, &glyph);
	error = contour_outline(drawn, glyph, outline);
	if (error != 0 && error != ENOSPC)
		return error;

	/* In the units of the face asked (also what arrays too small were given, with the counts). */
	contour_scale(outline, drawn->units_per_em, face->units_per_em);
	if (error != 0)
		return error;

	/* Succeeded: the outline holds every contour of the glyph. */
	return 0;
}

/* Reads one glyph's contours out of one face's glyf, in its design units (truetype_glyph_outline). */
static int
contour_outline(
	const struct truetype_face *face,
	unsigned glyph,
	struct truetype_glyph_outline *outline)
{
	struct contour_walk walk;
	const uint8_t *entry;
	uint32_t offset;
	uint32_t length;
	int advance;
	int bearing;
	int error;

	/* Refuses a call without a face or a place to put the outline. */
	if (face == NULL)
		return EINVAL;
	if (outline == NULL)
		return EINVAL;

	/* Refuses a capacity that names an array the caller did not give. */
	if (outline->points == NULL && outline->point_capacity != 0)
		return EINVAL;
	if (outline->contour_ends == NULL && outline->contour_capacity != 0)
		return EINVAL;

	/* Starts the outline empty, so that a failure leaves nothing stale. */
	outline->point_count = 0;
	outline->contour_count = 0;
	outline->advance = 0;
	outline->left_side_bearing = 0;
	outline->x_min = 0;
	outline->y_min = 0;
	outline->x_max = 0;
	outline->y_max = 0;

	/* Finds the glyph's entry, which also refuses a glyph the font lacks. */
	error = truetype_glyph_range(face, glyph, &offset, &length);
	if (error != 0)
		return error;

	/* Reads how far the pen moves after the glyph. */
	error = truetype_glyph_design_advance(face, glyph, &advance);
	if (error != 0)
		return error;

	/* Reads where the glyph's box starts from the pen position. */
	error = contour_read_side_bearing(face, glyph, &bearing);
	if (error != 0)
		return error;

	outline->advance = advance;
	outline->left_side_bearing = bearing;

	/* Copies the box the entry's header records, when it has one. */
	if (length >= 10U) {
		entry = face->glyf + offset;
		outline->x_min = truetype_s16(entry + 2U);
		outline->y_min = truetype_s16(entry + 4U);
		outline->x_max = truetype_s16(entry + 6U);
		outline->y_max = truetype_s16(entry + 8U);
	}

	/* Prepares one walk through the glyph and its components. */
	walk.face = face;
	walk.outline = outline;
	walk.components = 0;
	walk.overflow = 0;

	/* Gathers the contours, flattening components as they come. */
	error = contour_load_glyph(&walk, glyph, 0);
	if (error != 0)
		return error;

	/* Reports arrays too small, with the counts saying what is needed. */
	if (walk.overflow)
		return ENOSPC;

	/* Succeeded: the outline holds every contour of the glyph. */
	return 0;
}

/* Scales an outline read in one em's units (from) to another's (to); the same em leaves it. */
static void
contour_scale(
	struct truetype_glyph_outline *outline,
	unsigned from,
	unsigned to)
{
	float ratio;
	unsigned index;

	/* The same units, or none to scale from. */
	if (from == to || from == 0U)
		return;
	ratio = (float)to / (float)from;

	/* Each point the caller's array holds. */
	for (index = 0; index < outline->point_count && index < outline->point_capacity; index++) {
		outline->points[index].x *= ratio;
		outline->points[index].y *= ratio;
	}

	/* The advance, the bearing and the box, to the nearest unit. */
	outline->advance = (int)((float)outline->advance * ratio + 0.5f);
	outline->left_side_bearing = (int)lroundf((float)outline->left_side_bearing * ratio);
	outline->x_min = (int)lroundf((float)outline->x_min * ratio);
	outline->y_min = (int)lroundf((float)outline->y_min * ratio);
	outline->x_max = (int)lroundf((float)outline->x_max * ratio);
	outline->y_max = (int)lroundf((float)outline->y_max * ratio);
}

/* Appends one glyph's contours, simple or composite. */
static int
contour_load_glyph(
	struct contour_walk *walk,
	unsigned glyph,
	unsigned depth)
{
	const uint8_t *entry;
	uint32_t offset;
	uint32_t length;
	int contours;
	int error;

	/* Finds the glyph's entry in glyf. */
	error = truetype_glyph_range(walk->face, glyph, &offset, &length);
	if (error != 0)
		return error;

	/* A glyph with no entry has no outline, which is what a space is. */
	if (length == 0)
		return 0;

	/* Refuses an entry too short to hold its own header. */
	if (length < 10U)
		return EINVAL;

	/* A negative contour count marks a composite glyph. */
	entry = walk->face->glyf + offset;
	contours = truetype_s16(entry);

	/* Reads the outline in the form the entry has. */
	if (contours >= 0) {
		error = contour_load_simple(walk, entry, length);
	} else {
		error = contour_load_composite(walk, entry, length, depth);
	}

	/* Reports why the entry could not be read. */
	if (error != 0)
		return error;

	/* Succeeded: the glyph's contours are appended. */
	return 0;
}

/*
 * Appends a simple glyph's contours: its end indices, flags and
 * coordinates, each coordinate stored as a difference from the last.
 */
static int
contour_load_simple(
	struct contour_walk *walk,
	const uint8_t *entry,
	uint32_t length)
{
	struct truetype_glyph_outline *outline;
	struct truetype_outline_point *points;
	uint32_t position;
	uint32_t instructions;
	unsigned contours;
	unsigned count;
	unsigned first;
	unsigned last;
	unsigned previous_end;
	unsigned contour;
	unsigned index;
	unsigned fits;
	int error;

	outline = walk->outline;
	first = outline->point_count;

	/* Reads how many contours the glyph has. */
	contours = truetype_u16(entry);
	if (contours == 0)
		return 0;

	/* Refuses end indices the entry does not hold. */
	if (length < 10U + (uint32_t)contours * 2U + 2U)
		return EINVAL;

	/* The last contour's end index says how many points there are. */
	count = (unsigned)truetype_u16(entry + 10U + (size_t)(contours - 1U) * 2U) + 1U;

	/* Refuses a glyph whose flattened outline would pass the bound. */
	if (count > CONTOUR_POINTS_MAX - first)
		return ENOTSUP;
	if (contours > CONTOUR_POINTS_MAX - outline->contour_count)
		return ENOTSUP;

	/* Decides whether this glyph fits the caller's arrays. */
	fits = 1U;
	if (walk->overflow)
		fits = 0;
	else if (count > outline->point_capacity - first)
		fits = 0;
	else if (contours > outline->contour_capacity - outline->contour_count)
		fits = 0;

	/* A glyph that does not fit is only counted. */
	if (!fits) {
		walk->overflow = 1U;
		outline->point_count += count;
		outline->contour_count += contours;
		return 0;
	}

	/* Records each contour's end, moved past the points already there. */
	previous_end = 0;
	for (contour = 0; contour < contours; contour++) {
		last = truetype_u16(entry + 10U + (size_t)contour * 2U);

		/*
		 * Each contour ends after the one before it, so none is empty
		 * and none runs backward.
		 */
		if (contour != 0 && last <= previous_end)
			return EINVAL;

		outline->contour_ends[outline->contour_count + contour] = first + last;
		previous_end = last;
	}

	/* Skips the hinting instructions, which this reader does not run. */
	position = 10U + (uint32_t)contours * 2U;
	instructions = truetype_u16(entry + position);
	position += 2U;
	if (instructions > length - position)
		return EINVAL;

	position += instructions;
	points = outline->points + first;

	/* Reads the flags, keeping each one in its point until the end. */
	error = contour_read_flags(points, count, entry, length, &position);
	if (error != 0)
		return error;

	/* Reads the horizontal coordinates. */
	error = contour_read_coordinates(points, count, entry, length, &position, 0);
	if (error != 0)
		return error;

	/* Reads the vertical coordinates. */
	error = contour_read_coordinates(points, count, entry, length, &position, 1U);
	if (error != 0)
		return error;

	/* Replaces each point's flags with whether it is on the curve. */
	for (index = 0; index < count; index++) {
		if ((points[index].on_curve & CONTOUR_ON_CURVE) != 0) {
			points[index].on_curve = 1U;
		} else {
			points[index].on_curve = 0;
		}
	}

	/* Publishes the points and contours this glyph added. */
	outline->point_count += count;
	outline->contour_count += contours;

	/* Succeeded: the simple glyph is appended. */
	return 0;
}

/*
 * Appends a composite glyph: each component read in turn and moved into
 * place by its offset and its scale or matrix.
 */
static int
contour_load_composite(
	struct contour_walk *walk,
	const uint8_t *entry,
	uint32_t length,
	unsigned depth)
{
	struct truetype_glyph_outline *outline;
	struct truetype_outline_point *point;
	struct contour_transform transform;
	uint32_t position;
	uint32_t needed;
	unsigned flags;
	unsigned component;
	unsigned composite_first;
	unsigned first;
	unsigned index;
	unsigned parent_point;
	unsigned child_point;
	float offset_x;
	float offset_y;
	float x;
	float y;
	int error;

	outline = walk->outline;

	/* Refuses a font that nests components deeper than this follows. */
	if (depth >= TRUETYPE_COMPOSITE_DEPTH)
		return ELOOP;

	/* The composite's points start where its first component's will. */
	composite_first = outline->point_count;
	position = 10U;

	/* Places each component until one says it is the last. */
	do {
		/* Refuses a component header the entry does not hold. */
		if (position + 4U > length)
			return EINVAL;

		flags = truetype_u16(entry + position);
		component = truetype_u16(entry + position + 2U);
		position += 4U;

		/* Refuses a composite that visits more components than the bound. */
		walk->components++;
		if (walk->components > CONTOUR_COMPONENTS_MAX)
			return ENOTSUP;

		/* Works out how many bytes the arguments and the transform take. */
		needed = 2U;
		if ((flags & COMPONENT_ARG_WORDS) != 0)
			needed = 4U;
		if ((flags & COMPONENT_HAVE_SCALE) != 0)
			needed += 2U;
		else if ((flags & COMPONENT_HAVE_XY_SCALE) != 0)
			needed += 4U;
		else if ((flags & COMPONENT_HAVE_MATRIX) != 0)
			needed += 8U;

		/* Refuses arguments or a transform the entry does not hold. */
		if (needed > length - position)
			return EINVAL;

		/*
		 * The two arguments are an offset when ARGS_XY is set, signed;
		 * otherwise they are point numbers to align, unsigned.
		 */
		if ((flags & COMPONENT_ARG_WORDS) != 0) {
			if ((flags & COMPONENT_ARGS_XY) != 0) {
				offset_x = (float)truetype_s16(entry + position);
				offset_y = (float)truetype_s16(entry + position + 2U);
			} else {
				offset_x = (float)truetype_u16(entry + position);
				offset_y = (float)truetype_u16(entry + position + 2U);
			}
			position += 4U;
		} else {
			if ((flags & COMPONENT_ARGS_XY) != 0) {
				offset_x = (float)(int8_t)entry[position];
				offset_y = (float)(int8_t)entry[position + 1U];
			} else {
				offset_x = (float)entry[position];
				offset_y = (float)entry[position + 1U];
			}
			position += 2U;
		}

		/* Starts from the identity, which a component without a transform keeps. */
		transform.xx = 1.0f;
		transform.xy = 0.0f;
		transform.yx = 0.0f;
		transform.yy = 1.0f;
		transform.dx = 0.0f;
		transform.dy = 0.0f;

		/* Reads the one scale, the two scales or the matrix the flags name. */
		if ((flags & COMPONENT_HAVE_SCALE) != 0) {
			transform.xx = contour_f2dot14(entry + position);
			transform.yy = transform.xx;
			position += 2U;
		} else if ((flags & COMPONENT_HAVE_XY_SCALE) != 0) {
			transform.xx = contour_f2dot14(entry + position);
			transform.yy = contour_f2dot14(entry + position + 2U);
			position += 4U;
		} else if ((flags & COMPONENT_HAVE_MATRIX) != 0) {
			transform.xx = contour_f2dot14(entry + position);
			transform.xy = contour_f2dot14(entry + position + 2U);
			transform.yx = contour_f2dot14(entry + position + 4U);
			transform.yy = contour_f2dot14(entry + position + 6U);
			position += 8U;
		}

		/* Reads the component's own outline onto the end of the arrays. */
		first = outline->point_count;
		error = contour_load_glyph(walk, component, depth + 1U);
		if (error != 0)
			return error;

		/* An outline only being counted has no points to move. */
		if (walk->overflow)
			continue;

		/* Applies the component's scale or matrix to its points. */
		for (index = first; index < outline->point_count; index++) {
			point = &outline->points[index];
			x = point->x;
			y = point->y;
			point->x = transform.xx * x + transform.yx * y;
			point->y = transform.xy * x + transform.yy * y;
		}

		/*
		 * An offset is scaled by the transform only when the component
		 * asks for it; the default, as Windows reads fonts, is not to.
		 * Point alignment moves the component so that its point lands on
		 * a point the composite already has.
		 */
		if ((flags & COMPONENT_ARGS_XY) != 0) {
			if ((flags & COMPONENT_SCALED_OFFSET) != 0 &&
			    (flags & COMPONENT_UNSCALED_OFFSET) == 0) {
				transform.dx = transform.xx * offset_x + transform.yx * offset_y;
				transform.dy = transform.xy * offset_x + transform.yy * offset_y;
			} else {
				transform.dx = offset_x;
				transform.dy = offset_y;
			}
		} else {
			parent_point = composite_first + (unsigned)offset_x;
			child_point = first + (unsigned)offset_y;

			/* Refuses a point number the composite or the component lacks. */
			if (parent_point >= first)
				return EINVAL;
			if (child_point >= outline->point_count)
				return EINVAL;

			transform.dx = outline->points[parent_point].x - outline->points[child_point].x;
			transform.dy = outline->points[parent_point].y - outline->points[child_point].y;
		}

		/* Moves the component's points to where the composite places them. */
		for (index = first; index < outline->point_count; index++) {
			outline->points[index].x += transform.dx;
			outline->points[index].y += transform.dy;
		}
	} while ((flags & COMPONENT_MORE) != 0);

	/* Succeeded: every component is appended in place. */
	return 0;
}

/*
 * Reads a simple glyph's flags, one per point, expanding repeats.
 *
 * Each flag is kept in its point's on_curve field until the coordinates are
 * read, because the coordinates' encoding depends on it.
 */
static int
contour_read_flags(
	struct truetype_outline_point *points,
	unsigned count,
	const uint8_t *entry,
	uint32_t length,
	uint32_t *position)
{
	unsigned index;
	unsigned repeat;
	unsigned flags;

	/* Reads flags until every point has one. */
	index = 0;
	while (index < count) {
		/* Refuses a flag the entry does not hold. */
		if (*position >= length)
			return EINVAL;

		flags = entry[*position];
		(*position)++;
		points[index].on_curve = flags;
		index++;

		/* A flag without REPEAT stands for its own point only. */
		if ((flags & CONTOUR_REPEAT) == 0)
			continue;

		/* Refuses a repeat count the entry does not hold. */
		if (*position >= length)
			return EINVAL;

		repeat = entry[*position];
		(*position)++;

		/* Gives the same flag to the points the repeat count covers. */
		while (repeat > 0 && index < count) {
			points[index].on_curve = flags;
			index++;
			repeat--;
		}
	}

	/* Succeeded: every point holds its flag. */
	return 0;
}

/*
 * Reads one axis of a simple glyph's coordinates.
 *
 * Each coordinate is a difference from the one before.  A short one is a
 * byte whose sign is in the SAME flag; a long one is two signed bytes; and a
 * point with neither flag set repeats the previous coordinate.
 */
static int
contour_read_coordinates(
	struct truetype_outline_point *points,
	unsigned count,
	const uint8_t *entry,
	uint32_t length,
	uint32_t *position,
	unsigned vertical)
{
	unsigned short_flag;
	unsigned same_flag;
	unsigned index;
	int value;

	/* Picks the pair of flags this axis is encoded by. */
	short_flag = CONTOUR_X_SHORT;
	same_flag = CONTOUR_X_SAME;
	if (vertical) {
		short_flag = CONTOUR_Y_SHORT;
		same_flag = CONTOUR_Y_SAME;
	}

	/* Adds up each point's difference into its coordinate. */
	value = 0;
	for (index = 0; index < count; index++) {
		if ((points[index].on_curve & short_flag) != 0) {
			/* Refuses a coordinate the entry does not hold. */
			if (*position >= length)
				return EINVAL;

			/* A short difference carries its sign in the SAME flag. */
			if ((points[index].on_curve & same_flag) != 0) {
				value += (int)entry[*position];
			} else {
				value -= (int)entry[*position];
			}
			(*position)++;
		} else if ((points[index].on_curve & same_flag) == 0) {
			/* Refuses a coordinate the entry does not hold. */
			if (2U > length - *position)
				return EINVAL;

			value += truetype_s16(entry + *position);
			*position += 2U;
		}

		/* Stores the running coordinate on the axis being read. */
		if (vertical) {
			points[index].y = (float)value;
		} else {
			points[index].x = (float)value;
		}
	}

	/* Succeeded: every point has this axis. */
	return 0;
}

/*
 * Reads a glyph's left side bearing out of hmtx.
 *
 * Glyphs past the long metrics keep the last advance but have a bearing of
 * their own, in the array of bearings that follows.
 */
static int
contour_read_side_bearing(
	const struct truetype_face *face,
	unsigned glyph,
	int *bearing)
{
	uint32_t offset;

	/* Finds where the glyph's bearing is stored. */
	if (glyph < face->hmetric_count) {
		offset = (uint32_t)glyph * 4U + 2U;
	} else {
		offset = (uint32_t)face->hmetric_count * 4U +
			 (uint32_t)(glyph - face->hmetric_count) * 2U;
	}

	/* Refuses a bearing the table does not hold. */
	if (offset > face->hmtx_size)
		return EINVAL;
	if (2U > face->hmtx_size - offset)
		return EINVAL;

	*bearing = truetype_s16(face->hmtx + offset);

	/* Succeeded: the bearing is read. */
	return 0;
}

/* Reads a signed 2.14 fixed-point number, which a component's transform uses. */
static float
contour_f2dot14(
	const uint8_t *bytes)
{
	float value;

	value = (float)truetype_s16(bytes) / 16384.0f;

	/* Reports the number as a float. */
	return value;
}
