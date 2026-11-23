/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The cursor's box (ws213-p004): the pointer's place the input thread
 * writes as the reports come and the main thread's warps replace, and the
 * outputs' rectangles it is computed over (cursor-box.h).
 */

#include "cursor-box.h"
#include "kwl.h"
#include "rotate.h"

#include <errno.h>
#include <string.h>
#include <time.h>

static void box_write(struct kwl_cursor_box *box, int32_t x, int32_t y, int32_t fraction_x, int32_t fraction_y, unsigned output, uint64_t event_us, struct kwl_cursor_motion *motion);
static int box_fine(const struct kwl_cursor_box *box);
static int64_t box_floor_div(int64_t value, int64_t divisor);
static void box_geometry(struct kwl_cursor_box *box, const struct kwl_cursor_geometry *geometry);
static void box_geometry_read(const struct kwl_cursor_box *box, struct kwl_cursor_geometry *geometry);
static int32_t geometry_scale(const struct kwl_cursor_axis *axis, uint32_t size, unsigned unit);

/*
 * Makes the box with the pointer at a place of the anchor, whose size is
 * the only output's until the main thread gives the outputs.
 *
 * The change signal uses the monotonic clock, as every timed wait of the
 * compositor's threads does.
 */
int
kwl_cursor_box_init(
	struct kwl_cursor_box *box,
	int32_t x,
	int32_t y,
	uint32_t width,
	uint32_t height)
{
	pthread_condattr_t attributes;
	int error;

	/* The place, on the anchor, and no write yet. */
	memset(box, 0, sizeof(*box));
	box->x = x;
	box->y = y;
	box->output = KWL_PLANE_ANCHOR;
	box->outputs[KWL_PLANE_ANCHOR].width = width;
	box->outputs[KWL_PLANE_ANCHOR].height = height;
	box->output_count = KWL_PLANE_SLOTS;
	box->physical[KWL_PLANE_ANCHOR].width = width;
	box->physical[KWL_PLANE_ANCHOR].height = height;
	box->absolute_slot = KWL_PLANE_ANCHOR;

	/* The cursor lock. */
	error = pthread_mutex_init(&box->lock, NULL);
	if (error != 0)
		return error;

	/* The signal a writer gives the cursor thread, timed by the monotonic clock. */
	error = pthread_condattr_init(&attributes);
	if (error != 0) {
		pthread_mutex_destroy(&box->lock);
		return error;
	}

	/* The clock of the timed waits. */
	error = pthread_condattr_setclock(&attributes, CLOCK_MONOTONIC);
	if (error == 0)
		error = pthread_cond_init(&box->changed, &attributes);
	if (error != 0) {
		pthread_condattr_destroy(&attributes);
		pthread_mutex_destroy(&box->lock);
		return error;
	}

	/* The cursor thread's answers to the main thread, on the same clock. */
	error = pthread_cond_init(&box->answered, &attributes);
	pthread_condattr_destroy(&attributes);
	if (error != 0) {
		pthread_cond_destroy(&box->changed);
		pthread_mutex_destroy(&box->lock);
		return error;
	}

	/* Succeeded: the box can be shared. */
	return 0;
}

/*
 * Destroys the box's lock and signal once no thread uses them.
 */
void
kwl_cursor_box_destroy(
	struct kwl_cursor_box *box)
{
	/* The signals, then the lock. */
	pthread_cond_destroy(&box->answered);
	pthread_cond_destroy(&box->changed);
	pthread_mutex_destroy(&box->lock);
}

/*
 * Moves the pointer by a relative motion over the outputs (the input
 * thread's mouse and touch pad): into the output the motion reaches, or
 * stopped at an edge no output shares (plane.c).
 */
void
kwl_cursor_box_move(
	struct kwl_cursor_box *box,
	int64_t dx,
	int64_t dy,
	uint64_t event_us,
	struct kwl_cursor_motion *motion)
{
	int32_t x;
	int32_t y;
	unsigned output;

	/* The new place computed from the box's and written in one section. */
	pthread_mutex_lock(&box->lock);

	output = kwl_plane_move(box->outputs, box->output_count, box->output, box->x, box->y, (int32_t)dx, (int32_t)dy, &x, &y);
	box_write(box, x, y, box->fraction_x, box->fraction_y, output, event_us, motion);

	pthread_mutex_unlock(&box->lock);
}

/*
 * Moves the pointer by a relative motion in 256ths of a unit (a mouse's
 * motion on an output at another scale than 100 %, kwl_cursor_box_fine):
 * the place's part between units is kept, and the whole units it adds up
 * to move the pointer over the outputs as kwl_cursor_box_move does.  An
 * axis stopped at an edge no output shares loses its part.
 */
void
kwl_cursor_box_move_fine(
	struct kwl_cursor_box *box,
	int64_t dx,
	int64_t dy,
	uint64_t event_us,
	struct kwl_cursor_motion *motion)
{
	int32_t whole_x;
	int32_t whole_y;
	int32_t fraction_x;
	int32_t fraction_y;
	int32_t x;
	int32_t y;
	unsigned output;

	/* The new place computed from the box's and written in one section. */
	pthread_mutex_lock(&box->lock);

	/* The motion with the place's part between units, as whole units and the part left. */
	kwl_cursor_fine_split((int64_t)box->fraction_x + dx, &whole_x, &fraction_x);
	kwl_cursor_fine_split((int64_t)box->fraction_y + dy, &whole_y, &fraction_y);

	/* The whole units over the outputs. */
	output = kwl_plane_move(box->outputs, box->output_count, box->output, box->x, box->y, whole_x, whole_y, &x, &y);

	/* An axis held at an edge on the same output keeps no part beyond it. */
	if (output == box->output && (int64_t)x != (int64_t)box->x + whole_x)
		fraction_x = 0;
	if (output == box->output && (int64_t)y != (int64_t)box->y + whole_y)
		fraction_y = 0;
	box_write(box, x, y, fraction_x, fraction_y, output, event_us, motion);

	pthread_mutex_unlock(&box->lock);
}

/*
 * Tells whether the pointer keeps its place between units (the anchor shown
 * at another scale than 100 %): the input thread then gives a mouse's
 * motion in 256ths of a unit (kwl_cursor_box_move_fine).
 */
unsigned
kwl_cursor_box_fine(
	struct kwl_cursor_box *box)
{
	int fine;

	/* The scale as the main thread last gave it. */
	pthread_mutex_lock(&box->lock);

	fine = box_fine(box);

	pthread_mutex_unlock(&box->lock);

	/* Whether the place keeps its part between units. */
	if (fine)
		return 1U;
	return 0U;
}

/*
 * Places the pointer where an absolute pointer's report maps (QEMU's
 * tablet, design 6.1): on the panel of its output, turned back by the
 * output's angle (WS217), the same report's relative motion added and kept
 * on that output.  The warp count does not move: the main thread hears of
 * it in the order the input thread wrote it.
 */
void
kwl_cursor_box_place(
	struct kwl_cursor_box *box,
	int32_t value_x,
	int32_t minimum_x,
	int32_t maximum_x,
	int32_t value_y,
	int32_t minimum_y,
	int32_t maximum_y,
	int64_t dx,
	int64_t dy,
	uint64_t event_us,
	struct kwl_cursor_motion *motion)
{
	struct kwl_cursor_axis across;
	struct kwl_cursor_axis down;
	const struct kwl_plane_rect *rect;
	struct kwl_cursor_geometry geometry;
	unsigned slot;
	int32_t x;
	int32_t y;
	int32_t fraction_x;
	int32_t fraction_y;
	int fine;

	/* The report's axes. */
	across.value = value_x;
	across.minimum = minimum_x;
	across.maximum = maximum_x;
	down.value = value_y;
	down.minimum = minimum_y;
	down.maximum = maximum_y;

	/* The place mapped over the geometry as it is now, written in one section. */
	pthread_mutex_lock(&box->lock);

	box_geometry_read(box, &geometry);

	/* The output the absolute pointer maps onto: the anchor while that one is not shown. */
	slot = geometry.absolute_slot;
	if (slot >= KWL_PLANE_SLOTS || geometry.outputs[slot].width == 0U)
		slot = KWL_PLANE_ANCHOR;

	/*
	 * The report's place on it, a direct device's (its panel turned back by
	 * its angle): in whole units at 100 %, in 256ths of a unit on an output
	 * at another scale, where the part between units is kept.
	 */
	fraction_x = 0;
	fraction_y = 0;
	fine = box_fine(box);
	if (fine) {
		kwl_cursor_geometry_place(&geometry, slot, 1U, 256U, &across, &down, &x, &y);
		kwl_cursor_fine_split(x, &x, &fraction_x);
		kwl_cursor_fine_split(y, &y, &fraction_y);
	} else {
		kwl_cursor_geometry_place(&geometry, slot, 1U, 1U, &across, &down, &x, &y);
	}

	/* The report's relative motion added, kept on the output. */
	rect = &geometry.outputs[slot];
	x = rect->x + kwl_cursor_clamp((int64_t)(x - rect->x) + dx, rect->width);
	y = rect->y + kwl_cursor_clamp((int64_t)(y - rect->y) + dy, rect->height);
	box_write(box, x, y, fraction_x, fraction_y, slot, event_us, motion);

	pthread_mutex_unlock(&box->lock);
}

/*
 * Moves the pointer for the main thread (a warp, design 6.1 N-3): the move
 * computes the new place from the box's own, not from the main thread's
 * view that may be behind it, and the geometry (when given: the outputs'
 * rectangles, panels and angles, WS217) changes in the same section, so the input thread never computes a motion
 * from a new place over old rectangles or the reverse.  The warp count
 * moves, so the input thread's motions computed before it are not taken
 * as the pointer's place.
 */
void
kwl_cursor_box_warp(
	struct kwl_cursor_box *box,
	const struct kwl_cursor_geometry *geometry,
	kwl_cursor_box_move_fn *move,
	const void *context,
	struct kwl_cursor_motion *motion)
{
	int32_t x;
	int32_t y;
	unsigned output;

	/* The geometry, the place and the count in one section. */
	pthread_mutex_lock(&box->lock);

	if (geometry != NULL)
		box_geometry(box, geometry);

	/* The warp's new place from the box's own. */
	x = box->x;
	y = box->y;
	output = box->output;
	move(context, box->outputs, &x, &y, &output);
	box->warp++;
	box_write(box, x, y, 0, 0, output, 0U, motion);

	pthread_mutex_unlock(&box->lock);
}

/*
 * Gives the box the geometry as the main thread has it now (a display
 * opened, closed, moved, resized or turned), for the motions from now on.
 * A pointer the change takes off its output is carried by a warp.  Another
 * panel or angle (or another output for the absolute pointer) moves the
 * warp count, so that a place the input thread computed over the old
 * geometry is not taken as the pointer's (ws217-p003 review M7); *warp
 * gets the count the main thread holds from now on.
 */
void
kwl_cursor_box_outputs(
	struct kwl_cursor_box *box,
	const struct kwl_cursor_geometry *geometry,
	uint64_t *warp)
{
	int panels;

	/* The geometry replaced whole, and the count moved for another mapping. */
	pthread_mutex_lock(&box->lock);

	panels = memcmp(box->physical, geometry->physical, sizeof(box->physical));
	if (panels != 0 || box->absolute_slot != geometry->absolute_slot)
		box->warp++;
	box_geometry(box, geometry);
	*warp = box->warp;

	pthread_mutex_unlock(&box->lock);
}

/*
 * Reads the pointer's place and output as the box has them now.
 */
void
kwl_cursor_box_read(
	struct kwl_cursor_box *box,
	int32_t *x,
	int32_t *y,
	unsigned *output)
{
	/* One consistent place. */
	pthread_mutex_lock(&box->lock);

	*x = box->x;
	*y = box->y;
	*output = box->output;

	pthread_mutex_unlock(&box->lock);
}

/*
 * Takes the cursor thread's counts of the window and starts them again.
 */
void
kwl_cursor_box_take_stats(
	struct kwl_cursor_box *box,
	struct kwl_cursor_stats *stats)
{
	/* The counts, and zero for the next window. */
	pthread_mutex_lock(&box->lock);

	*stats = box->stats;
	memset(&box->stats, 0, sizeof(box->stats));

	pthread_mutex_unlock(&box->lock);
}

/*
 * Keeps a pointer coordinate inside 0 .. size - 1.
 */
int32_t
kwl_cursor_clamp(
	int64_t position,
	uint32_t size)
{
	/* Nothing lies left of or above the output. */
	if (position < 0)
		return 0;

	/* Nothing lies right of or below the output. */
	if (position >= (int64_t)size)
		return (int32_t)size - 1;

	/* Succeeded: the position is already on the output. */
	return (int32_t)position;
}

/*
 * Maps an absolute device's report onto an output of the plane (WS217,
 * plan/ws217/phase001 6.1), in pixels (unit 1) or 1/256 pixels (unit 256):
 * a direct device (a touch screen, a pen on the display, an absolute
 * pointer) onto the output's panel, then turned back by its angle to the
 * logical place; any other (a pen tablet on the desk) onto the logical
 * picture as it is, not turned.  The place is of the plane: the output's
 * corner is added.  An output not shown is the anchor.
 */
void
kwl_cursor_geometry_place(
	const struct kwl_cursor_geometry *geometry,
	unsigned slot,
	unsigned direct,
	unsigned unit,
	const struct kwl_cursor_axis *across,
	const struct kwl_cursor_axis *down,
	int32_t *x,
	int32_t *y)
{
	const struct kwl_plane_rect *rect;
	const struct kwl_cursor_physical *physical;

	/* The output, or the anchor for one not shown. */
	if (slot >= KWL_PLANE_SLOTS || geometry->outputs[slot].width == 0U)
		slot = KWL_PLANE_ANCHOR;
	rect = &geometry->outputs[slot];
	physical = &geometry->physical[slot];

	/* A device on the display: the point of its panel, then the logical point it shows. */
	if (direct) {
		*x = geometry_scale(across, physical->width, unit);
		*y = geometry_scale(down, physical->height, unit);
		kwl_rotate_pixel_back(physical->rotation, physical->width * unit, physical->height * unit, x, y);
	} else {
		/* A device off the display: the logical picture as it is. */
		*x = geometry_scale(across, rect->width, unit);
		*y = geometry_scale(down, rect->height, unit);
	}

	/* The output's corner of the plane. */
	*x += rect->x * (int32_t)unit;
	*y += rect->y * (int32_t)unit;
}

/*
 * Gives the pixel a place of the plane (whole units and 256ths of a unit)
 * is shown at, snapped as scale.h snaps a unit: floor(place * scale + 1/2).
 * At 100 % (or a scale not known) it is the unit itself.
 */
int32_t
kwl_cursor_pixel(
	int32_t unit,
	int32_t fraction,
	unsigned scale120)
{
	int64_t fixed;
	int64_t pixel;

	/* At 100 % a unit is a pixel. */
	if (scale120 == 0U || scale120 == KWL_SCALE_ONE)
		return unit;

	/* floor((place * scale / 120) + 1/2), the place in 256ths. */
	fixed = (int64_t)unit * 256 + fraction;
	pixel = box_floor_div(fixed * (int64_t)scale120 + 128 * (int64_t)KWL_SCALE_ONE, 256 * (int64_t)KWL_SCALE_ONE);

	/* Succeeded: the pixel. */
	return (int32_t)pixel;
}

/*
 * Splits a place or a motion in 256ths of a unit into whole units (rounded
 * down, also below 0) and the part left (0 to 255).
 */
void
kwl_cursor_fine_split(
	int64_t fine,
	int32_t *whole,
	int32_t *fraction)
{
	int64_t units;

	/* The whole units, rounded toward minus infinity. */
	units = box_floor_div(fine, 256);
	*whole = (int32_t)units;
	*fraction = (int32_t)(fine - units * 256);
}

/* Writes a place (and its part between units) into the box (its lock held), wakes the cursor thread and reports the write. */
static void
box_write(
	struct kwl_cursor_box *box,
	int32_t x,
	int32_t y,
	int32_t fraction_x,
	int32_t fraction_y,
	unsigned output,
	uint64_t event_us,
	struct kwl_cursor_motion *motion)
{
	uint64_t now;

	/* What changed, before the place is replaced. */
	motion->moved = 0U;
	if (x != box->x || y != box->y || output != box->output)
		motion->moved = 1U;
	if (fraction_x != box->fraction_x || fraction_y != box->fraction_y)
		motion->moved = 1U;
	motion->crossed = 0U;
	if (output != box->output)
		motion->crossed = 1U;

	/*
	 * The place, and the times of its report and of this write: the box's
	 * lag is measured between them (design 6.4).  A warp has no report.
	 */
	now = kwl_microseconds();
	box->x = x;
	box->y = y;
	box->fraction_x = fraction_x;
	box->fraction_y = fraction_y;
	box->output = output;
	if (event_us != 0U)
		box->event_us = event_us;
	box->write_us = now;
	box->writes++;
	box->sequence++;

	/* What the writer tells the main thread. */
	motion->x = x;
	motion->y = y;
	motion->fraction_x = fraction_x;
	motion->fraction_y = fraction_y;
	motion->output = output;
	motion->warp = box->warp;
	motion->event_us = event_us;
	motion->write_us = now;

	/* The cursor thread wakes for a new place (its wait is under this lock). */
	if (motion->moved)
		pthread_cond_broadcast(&box->changed);
}

/* Replaces the box's geometry (its lock held). */
static void
box_geometry(
	struct kwl_cursor_box *box,
	const struct kwl_cursor_geometry *geometry)
{
	/* The rectangles, the panels and the absolute pointer's output, together. */
	memcpy(box->outputs, geometry->outputs, sizeof(box->outputs));
	box->output_count = geometry->output_count;
	memcpy(box->physical, geometry->physical, sizeof(box->physical));
	box->absolute_slot = geometry->absolute_slot;
	box->scale120 = geometry->scale120;
}

/* Copies the box's geometry out (its lock held). */
static void
box_geometry_read(
	const struct kwl_cursor_box *box,
	struct kwl_cursor_geometry *geometry)
{
	/* The rectangles, the panels and the absolute pointer's output, together. */
	memcpy(geometry->outputs, box->outputs, sizeof(geometry->outputs));
	geometry->output_count = box->output_count;
	memcpy(geometry->physical, box->physical, sizeof(geometry->physical));
	geometry->absolute_slot = box->absolute_slot;
	geometry->scale120 = box->scale120;
}

/*
 * Maps one absolute axis value onto 0 .. size - 1 pixels, in units of
 * 1/unit pixel (the absolute pointer's arithmetic before WS217 at unit 1).
 */
static int32_t
geometry_scale(
	const struct kwl_cursor_axis *axis,
	uint32_t size,
	unsigned unit)
{
	int64_t offset;
	int64_t range;

	/* The low end of the range, and anything below it, is the first pixel. */
	if (size <= 1U || axis->value <= axis->minimum)
		return 0;

	/* The high end of the range, and anything above it, is the last pixel. */
	if (axis->value >= axis->maximum)
		return ((int32_t)size - 1) * (int32_t)unit;

	/* Everything between scales linearly, rounding down. */
	offset = (int64_t)axis->value - axis->minimum;
	range = (int64_t)axis->maximum - axis->minimum;

	/* Succeeded: the place the value falls on. */
	return (int32_t)((offset * ((int64_t)size - 1) * (int64_t)unit) / range);
}

/* Tells whether the box keeps the place's part between units: its scale is known and not 100 % (its lock held). */
static int
box_fine(
	const struct kwl_cursor_box *box)
{
	/* At 100 %, or before the main thread gave a scale, whole units. */
	if (box->scale120 == 0U || box->scale120 == KWL_SCALE_ONE)
		return 0;

	/* Another scale. */
	return 1;
}

/* Divides rounding toward minus infinity (the divisor above 0). */
static int64_t
box_floor_div(
	int64_t value,
	int64_t divisor)
{
	int64_t quotient;

	/* C rounds toward 0: one less for a negative value left with a remainder. */
	quotient = value / divisor;
	if (value % divisor != 0 && value < 0)
		quotient--;

	/* The quotient. */
	return quotient;
}
